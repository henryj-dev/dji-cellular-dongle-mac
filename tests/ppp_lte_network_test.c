#define _DARWIN_C_SOURCE
#include "ppp_lte_network.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-network line=%d\n", __LINE__); } } while (0)
enum fault { NONE, ADDRESS_ERROR, ADDRESS_MISSING, ADDRESS_DUPLICATE, ADDRESS_PEER,
    ADDRESS_SELF, ADDRESS_CHANGE, IF_CHANGE, STOP_GATE, CLOCK_ERROR, CLOCK_BACKWARD,
    TIMEOUT, ROUTE_SOCKET, ROUTE_FCNTL, ROUTE_WRITE, ROUTE_SHORT, ROUTE_WAIT,
    ROUTE_TYPE, ROUTE_VERSION, ROUTE_PID, ROUTE_SEQ, ROUTE_INDEX, ROUTE_DOWN,
    ROUTE_REJECT, ROUTE_BLACKHOLE, ROUTE_ERROR, ROUTE_MASK, ROUTE_LENGTH,
    ROUTE_SA_LENGTH, ROUTE_SA_FAMILY, ROUTE_DEST, ROUTE_TRAILING,
    TCP_SOCKET, TCP_FCNTL, NOSIGPIPE, BOUND_SET, BOUND_GET, BOUND_WRONG,
    CONNECT_ERROR, CONNECT_PENDING, CONNECT_SOERROR, POLL_ERROR, SEND_ERROR,
    SEND_ZERO, SEND_OVERSIZE, SHUTDOWN_ERROR, RECEIVE_ERROR, RECEIVE_SHORT,
    RECEIVE_WRONG, RECEIVE_EXTRA, RECEIVE_WAIT, TRANSIENT, NETWORK_ROUTE, DEFAULT_ROUTE,
    COMPACT_MASK, TRUNCATED_MASK, BAD_PREFIX, NONCONTIGUOUS_MASK,
    GATE_DEADLINE_ROUTE, GATE_DEADLINE_CONNECT, GATE_DEADLINE_SEND, ROUTE_FOREIGN, ADDRESS_LOST,
    ADDRESS_BLIP, ADDRESS_BLIP_ROUTE, ADDRESS_FLAP, ADDRESS_DUPLICATE_LATE, ADDRESS_LOST_OPEN,
    ADDRESS_CHANGE_CONNECT };
static struct {
    enum fault fault;
    uint64_t now, first_address_ms, lost_address_ms, repin_ms, route_socket_ms, last_route_socket_ms;
    unsigned clocks, gates, addresses, sockets, route_sockets, tcp_sockets;
    unsigned connects, sends, receives, shutdowns, route_writes, route_reads;
    unsigned route_closes, tcp_closes, fcntls, polls, blipped, stale_sent;
    size_t sent, received;
    unsigned char request[512], stale[512]; size_t request_size, stale_size;
    struct sockaddr_in local, peer;
    struct ifaddrs first, second;
} f;
static int fake_clock(clockid_t id, struct timespec *ts) {
    CHECK(id == CLOCK_MONOTONIC); f.clocks++;
    if (f.fault == CLOCK_ERROR) return -1;
    f.now += f.fault == TIMEOUT ? 40000 : 1;
    ts->tv_sec = (time_t)(f.now / 1000); ts->tv_nsec = (long)(f.now % 1000) * 1000000;
    if (f.fault == CLOCK_BACKWARD) ts->tv_sec = ts->tv_nsec = 0;
    return 0;
}
static int gate(void *context, struct ppp_trial_interface *i) {
    CHECK(context == &f); f.gates++;
    if (f.fault == STOP_GATE) return -1;
    if ((f.fault == GATE_DEADLINE_ROUTE && f.route_sockets) ||
        (f.fault == GATE_DEADLINE_CONNECT && f.tcp_sockets) ||
        (f.fault == GATE_DEADLINE_SEND && f.connects)) f.now += 60000;
    *i = (struct ppp_trial_interface){.name = "ppp-fixture", .index = 7};
    if (f.fault == IF_CHANGE && f.gates > 3) i->index = 8;
    return 0;
}
static int fake_getifaddrs(struct ifaddrs **list) {
    f.addresses++;
    /* The readiness gate's last clock read set last_ms just before this call. */
    if (f.addresses == 1) f.first_address_ms = f.now;
    /* The read that first lacks (ADDRESS_LOST, _BLIP) or changes (ADDRESS_CHANGE) the pin. */
    if ((f.fault == ADDRESS_LOST && f.addresses == 2) || (f.fault == ADDRESS_CHANGE && f.addresses == 6) ||
        (f.fault == ADDRESS_BLIP && f.addresses == 3))
        f.lost_address_ms = f.now;
    /* After the IP stage: the first read once the TCP socket exists / connect ran. */
    if (!f.lost_address_ms && ((f.fault == ADDRESS_LOST_OPEN && f.tcp_sockets) ||
        (f.fault == ADDRESS_CHANGE_CONNECT && f.connects)))
        f.lost_address_ms = f.now;
    /* ADDRESS_BLIP: the pair is back on read 6 and re-pinned there. */
    if (f.fault == ADDRESS_BLIP && f.addresses == 6) f.repin_ms = f.now;
    if (f.fault == ADDRESS_ERROR) return -1;
    f.local = (struct sockaddr_in){.sin_len = sizeof(f.local), .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(0x0a000001)};
    f.peer = f.local; f.peer.sin_addr.s_addr = htonl(0x0a000002);
    if (f.fault == ADDRESS_SELF) f.local.sin_addr.s_addr = htonl(0xc0000201);
    if (f.fault == ADDRESS_CHANGE && f.addresses > 5) f.local.sin_addr.s_addr++;
    if (f.fault == ADDRESS_CHANGE_CONNECT && f.connects) f.local.sin_addr.s_addr++;
    /* ADDRESS_FLAP: the local address alternates every 10 reads (about 1s), never settling. */
    if (f.fault == ADDRESS_FLAP && (f.addresses / 10) % 2) f.local.sin_addr.s_addr++;
    f.first = (struct ifaddrs){.ifa_name = "ppp-fixture", .ifa_flags = IFF_UP | IFF_POINTOPOINT,
        .ifa_addr = (struct sockaddr *)&f.local, .ifa_dstaddr = (struct sockaddr *)&f.peer};
    if (f.fault == ADDRESS_PEER) f.first.ifa_dstaddr = NULL;
    if (f.fault == ADDRESS_DUPLICATE || (f.fault == ADDRESS_DUPLICATE_LATE && f.addresses >= 3)) {
        f.second = f.first; f.first.ifa_next = &f.second;
    }
    /* ADDRESS_LOST: one point-to-point IPv4 is pinned, then it disappears for good.
     * ADDRESS_BLIP: gone for reads 3-5 only. ADDRESS_BLIP_ROUTE: gone for the one
     * read right after the first RTM_GET, while that query is still pending. */
    int gone = f.fault == ADDRESS_MISSING || (f.fault == ADDRESS_LOST && f.addresses > 1) ||
        (f.fault == ADDRESS_BLIP && f.addresses >= 3 && f.addresses <= 5) ||
        (f.fault == ADDRESS_LOST_OPEN && f.tcp_sockets);
    if (f.fault == ADDRESS_BLIP_ROUTE && f.route_writes == 1 && !f.blipped) gone = f.blipped = 1;
    *list = gone ? NULL : &f.first;
    return 0;
}
static void fake_freeifaddrs(struct ifaddrs *list) { CHECK(!list || list == &f.first); }
static int fake_socket(int domain, int type, int protocol) {
    f.sockets++;
    if (domain == AF_ROUTE) {
        CHECK(type == SOCK_RAW && protocol == 0); f.route_sockets++;
        if (!f.route_socket_ms) f.route_socket_ms = f.now;
        f.last_route_socket_ms = f.now;
        return f.fault == ROUTE_SOCKET ? -1 : 40;
    }
    CHECK(domain == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    f.tcp_sockets++;
    return f.fault == TCP_SOCKET ? -1 : 41;
}
static int fake_close(int fd) {
    CHECK(fd == 40 || fd == 41);
    if (fd == 40) f.route_closes++; else f.tcp_closes++;
    return 0;
}
static int fake_fcntl(int fd, int command, ...) {
    CHECK(fd == 40 || fd == 41); f.fcntls++;
    if ((fd == 40 && f.fault == ROUTE_FCNTL) || (fd == 41 && f.fault == TCP_FCNTL)) return -1;
    CHECK(command == F_GETFL || command == F_SETFL);
    if (command == F_SETFL) {
        va_list args; va_start(args, command); int flags = va_arg(args, int); va_end(args);
        CHECK(flags & O_NONBLOCK);
    }
    return 0;
}
static int fake_setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    CHECK(fd == 41 && size == sizeof(int));
    if (level == SOL_SOCKET) {
        CHECK(option == SO_NOSIGPIPE && *(const int *)value == 1);
        return f.fault == NOSIGPIPE ? -1 : 0;
    }
    CHECK(level == IPPROTO_IP && option == IP_BOUND_IF && *(const int *)value == 7);
    return f.fault == BOUND_SET ? -1 : 0;
}
static int fake_getsockopt(int fd, int level, int option, void *value, socklen_t *size) {
    CHECK(fd == 41 && *size == sizeof(int));
    if (level == IPPROTO_IP) {
        CHECK(option == IP_BOUND_IF);
        if (f.fault == BOUND_GET) return -1;
        *(int *)value = f.fault == BOUND_WRONG ? 8 : 7;
    } else {
        CHECK(level == SOL_SOCKET && option == SO_ERROR);
        *(int *)value = f.fault == CONNECT_SOERROR ? ECONNREFUSED : 0;
    }
    return 0;
}
static pid_t fake_getpid(void) { return 456; }
static ssize_t fake_write(int fd, const void *data, size_t size) {
    CHECK(fd == 40 && size <= sizeof(f.request)); f.route_writes++;
    struct rt_msghdr h; memcpy(&h, data, sizeof(h));
    CHECK(h.rtm_type == RTM_GET && h.rtm_version == RTM_VERSION && h.rtm_index == 7 &&
        (h.rtm_flags & RTF_IFSCOPE) && h.rtm_addrs == RTA_DST);
    if (f.fault == ROUTE_WRITE) return -1;
    if (f.fault == TRANSIENT && f.route_writes == 1) { errno = EINTR; return -1; }
    memcpy(f.request, data, size); f.request_size = size;
    /* ADDRESS_BLIP_ROUTE keeps the first, abandoned query to replay its reply later. */
    if (f.route_writes == 1) { memcpy(f.stale, data, size); f.stale_size = size; }
    return (ssize_t)size;
}
static ssize_t fake_read(int fd, void *data, size_t size) {
    CHECK(fd == 40 && size == 512); f.route_reads++;
    if (f.fault == ROUTE_WAIT || (f.fault == TRANSIENT && f.route_reads == 1)) {
        errno = EAGAIN; return -1;
    }
    if (f.fault == ROUTE_SHORT) return 2;
    /* The reply to the query abandoned at the reset would arrive first (same pid,
     * old seq), but only on the socket that sent it: a closed route socket's
     * queue is gone with it. */
    if (f.fault == ADDRESS_BLIP_ROUTE && f.route_writes == 2 && !f.stale_sent && !f.route_closes) {
        f.stale_sent = 1; memcpy(data, f.stale, f.stale_size); return (ssize_t)f.stale_size;
    }
    /* Three broadcasts from other senders precede every reply: an ifa_msghdr
     * NEWADDR (20B), an ifma_msghdr NEWMADDR (16B), both shorter than
     * rt_msghdr, and a full-size RTM_ADD from another pid. */
    if (f.fault == ROUTE_FOREIGN && f.route_reads % 4) {
        unsigned phase = f.route_reads % 4;
        struct rt_msghdr other = {.rtm_version = RTM_VERSION, .rtm_type = RTM_ADD, .rtm_pid = 999};
        u_short length = phase == 1 ? sizeof(struct ifa_msghdr) :
            phase == 2 ? sizeof(struct ifma_msghdr) : sizeof(other);
        if (phase == 1) other.rtm_type = RTM_NEWADDR;
        if (phase == 2) other.rtm_type = RTM_NEWMADDR;
        other.rtm_msglen = length;
        memcpy(data, &other, length); return (ssize_t)length;
    }
    unsigned char message[512] = {0}; memcpy(message, f.request, f.request_size);
    struct rt_msghdr h; memcpy(&h, message, sizeof(h));
    size_t count = f.request_size;
    if (f.fault == ROUTE_TYPE) h.rtm_type = RTM_ADD;
    if (f.fault == ROUTE_VERSION) h.rtm_version++;
    if (f.fault == ROUTE_PID) h.rtm_pid++;
    if (f.fault == ROUTE_SEQ) h.rtm_seq++;
    if (f.fault == ROUTE_INDEX) h.rtm_index++;
    if (f.fault == ROUTE_DOWN) h.rtm_flags &= ~RTF_UP;
    if (f.fault == ROUTE_REJECT) h.rtm_flags |= RTF_REJECT;
    if (f.fault == ROUTE_BLACKHOLE) h.rtm_flags |= RTF_BLACKHOLE;
    if (f.fault == ROUTE_ERROR) h.rtm_errno = ESRCH;
    if (f.fault == ROUTE_MASK) h.rtm_addrs |= 1 << RTAX_MAX;
    if (f.fault == ROUTE_LENGTH) h.rtm_msglen++;
    if (f.fault == ROUTE_SA_LENGTH) message[sizeof(h)] = 255;
    if (f.fault == ROUTE_SA_FAMILY) message[sizeof(h) + 1] = AF_INET6;
    if (f.fault == ROUTE_DEST) message[sizeof(h) + 4]++;
    if (f.fault == ROUTE_TRAILING) { count++; h.rtm_msglen++; }
    if (f.fault >= NETWORK_ROUTE && f.fault <= NONCONTIGUOUS_MASK) {
        h.rtm_flags &= ~RTF_HOST; h.rtm_addrs |= RTA_NETMASK;
        struct sockaddr_in dst; memcpy(&dst, message + sizeof(h), sizeof(dst));
        dst.sin_addr.s_addr = htonl(f.fault == DEFAULT_ROUTE ? 0 :
            f.fault == BAD_PREFIX ? 0xc6336400 : 0xc0000200);
        memcpy(message + sizeof(h), &dst, sizeof(dst));
        unsigned char mask[sizeof(struct sockaddr_in)] = {0};
        size_t length = f.fault == DEFAULT_ROUTE ? 0 : f.fault == COMPACT_MASK ? 7 : sizeof(mask);
        mask[0] = (unsigned char)length; mask[1] = AF_UNSPEC;
        mask[4] = 255; mask[5] = 255; mask[6] = f.fault == NONCONTIGUOUS_MASK ? 1 : 255;
        size_t aligned = length ? (length + 3u) & ~3u : 4;
        memcpy(message + count, mask, aligned); count += aligned;
        h.rtm_msglen = (u_short)count;
        if (f.fault == TRUNCATED_MASK) { count--; h.rtm_msglen--; }
    }
    memcpy(message, &h, sizeof(h)); memcpy(data, message, count);
    return (ssize_t)count;
}
static int fake_connect(int fd, const struct sockaddr *address, socklen_t size) {
    CHECK(fd == 41 && size == sizeof(struct sockaddr_in) && address->sa_family == AF_INET);
    f.connects++;
    if (f.fault == CONNECT_ERROR) { errno = ECONNREFUSED; return -1; }
    if (f.fault == CONNECT_PENDING || f.fault == CONNECT_SOERROR || f.fault == POLL_ERROR ||
        f.fault == TRANSIENT) { errno = EINPROGRESS; return -1; }
    return 0;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout) {
    CHECK(count == 1 && timeout == 0 && fds[0].fd == 41); f.polls++;
    if (f.fault == POLL_ERROR) return -1;
    if (f.fault == TRANSIENT && f.polls == 1) { errno = EINTR; return -1; }
    fds[0].revents = POLLOUT; return 1;
}
static ssize_t fake_send(int fd, const void *data, size_t size, int flags) {
    CHECK(fd == 41 && !flags && size && f.sent + size == sizeof(PPP_LTE_PROBE) - 1);
    CHECK(!memcmp(data, &PPP_LTE_PROBE[f.sent], size)); f.sends++;
    if (f.fault == SEND_ERROR) return -1;
    if (f.fault == SEND_ZERO) return 0;
    if (f.fault == SEND_OVERSIZE) return (ssize_t)size + 1;
    if (f.fault == TRANSIENT && f.sends == 1) { errno = EAGAIN; return -1; }
    size_t part = size > 2 ? 2 : size; f.sent += part; return (ssize_t)part;
}
static int fake_shutdown(int fd, int how) {
    CHECK(fd == 41 && how == SHUT_WR && f.sent == sizeof(PPP_LTE_PROBE) - 1);
    f.shutdowns++; return f.fault == SHUTDOWN_ERROR ? -1 : 0;
}
static ssize_t fake_recv(int fd, void *data, size_t size, int flags) {
    CHECK(fd == 41 && size == 1 && !flags && f.shutdowns == 1); f.receives++;
    if (f.fault == RECEIVE_ERROR) return -1;
    if (f.fault == RECEIVE_WAIT || (f.fault == TRANSIENT && f.receives == 1)) { errno = EINTR; return -1; }
    if (f.fault == RECEIVE_SHORT && f.received == 2) return 0;
    if (f.received == sizeof(PPP_LTE_PROBE) - 1) {
        if (f.fault == RECEIVE_EXTRA) { *(char *)data = 'x'; return 1; }
        return 0;
    }
    *(char *)data = f.fault == RECEIVE_WRONG ? 'x' : PPP_LTE_PROBE[f.received++];
    return 1;
}
#define clock_gettime fake_clock
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#define socket fake_socket
#define close fake_close
#define fcntl fake_fcntl
#define setsockopt fake_setsockopt
#define getsockopt fake_getsockopt
#define getpid fake_getpid
#define write fake_write
#define read fake_read
#define connect fake_connect
#define poll fake_poll
#define send fake_send
#define shutdown fake_shutdown
#define recv fake_recv
#include "../src/ppp_lte_network.c"

static void transaction(enum fault fault) {
    memset(&f, 0, sizeof(f)); f.fault = fault; f.now = 1000;
    struct ppp_lte_network n;
    struct in_addr destination = {.s_addr = htonl(0xc0000201)};
    int result = ppp_lte_network_init(&n, destination, 1234, 1000, gate, &f);
    for (unsigned slices = 0; !result && slices < 1000; slices++) {
        result = ppp_lte_network_step(&n);
        /* Simulated owner yields to runner/USB after EVERY bounded stage. */
        if (fault == ROUTE_WAIT || fault == RECEIVE_WAIT || fault == ADDRESS_MISSING ||
            fault == ROUTE_TYPE || fault == ROUTE_PID || fault == ROUTE_SEQ) f.now += 1000;
        /* Settle wait: nothing past the address read until one pair held 3s. Only
         * ADDRESS_BLIP_ROUTE resets after the route socket and one query exist. */
        if (n.stage == PPP_LTE_IP && !n.stable_ms)
            CHECK(!f.tcp_sockets && !f.connects &&
                  (fault == ADDRESS_BLIP_ROUTE || (!f.route_sockets && !f.route_writes)));
        /* The owner's slices take time while waiting; the fixture clock alone is 1ms/read. */
        if (n.stage == PPP_LTE_IP && !n.stable_ms) f.now += 100;
    }
    /* A reply that is not ours is skipped, never accepted: wait until the deadline. */
    if (fault == ROUTE_TYPE || fault == ROUTE_PID || fault == ROUTE_SEQ) CHECK(f.route_reads > 1);
    if (fault == ROUTE_FOREIGN) CHECK(f.route_reads == 4 * f.route_writes);
    int reset = fault == ADDRESS_CHANGE || fault == ADDRESS_BLIP || fault == ADDRESS_BLIP_ROUTE;
    if (fault == NONE || fault == CONNECT_PENDING || fault == TRANSIENT || fault == ROUTE_FOREIGN ||
        fault == NETWORK_ROUTE || fault == DEFAULT_ROUTE || fault == COMPACT_MASK || reset) {
        CHECK(result == 1 && n.stage == PPP_LTE_DONE);
        CHECK(n.sent == sizeof(PPP_LTE_PROBE) - 1 && n.received == sizeof(PPP_LTE_PROBE) - 1);
        CHECK(f.shutdowns == 1 && f.connects == 1);
        /* Success never records a failure stage; the pin time is the first address
         * read, kept across an IP-stage re-pin. */
        CHECK(n.failed_stage == PPP_LTE_FAILED && n.pinned && n.pinned_ms == f.first_address_ms);
        /* The last pin held the whole settle window before anything else ran. */
        CHECK(n.stable_ms && n.stable_ms - n.settle_start_ms >= PPP_LTE_ADDRESS_SETTLE_MS &&
              n.address_resets == (reset ? 1u : 0u));
        if (fault != ADDRESS_BLIP_ROUTE) CHECK(f.route_socket_ms >= n.stable_ms);
        if (!reset) CHECK(n.settle_start_ms == n.pinned_ms);
    } else CHECK(result == -1 && n.stage == PPP_LTE_FAILED);
    /* Settled on the first pin: exactly one reply read per query. */
    if (fault == NONE)
        CHECK(n.stable_ms - n.pinned_ms >= PPP_LTE_ADDRESS_SETTLE_MS && f.route_writes == f.route_reads);
    /* Vanished for reads 3-5: the wait restarts at the read-6 re-pin. */
    if (fault == ADDRESS_BLIP)
        CHECK(f.lost_address_ms > n.pinned_ms && f.repin_ms > f.lost_address_ms &&
              n.settle_start_ms == f.repin_ms && n.stable_ms - f.repin_ms >= PPP_LTE_ADDRESS_SETTLE_MS);
    /* Replaced on read 6: the new pair is pinned on read 7, settles, and is the one kept. */
    if (fault == ADDRESS_CHANGE)
        CHECK(n.local.s_addr == f.local.sin_addr.s_addr && n.local.s_addr != htonl(0x0a000001) &&
              n.settle_start_ms > f.lost_address_ms &&
              n.stable_ms - f.lost_address_ms > PPP_LTE_ADDRESS_SETTLE_MS);
    /* Reset while the first RTM_GET was pending: the route socket is closed at the
     * reset (its unread queue would otherwise fill during the new wait), so the
     * abandoned reply never arrives; a fresh socket and query (new sequence)
     * follow the second settle. */
    if (fault == ADDRESS_BLIP_ROUTE)
        /* Every socket stage sends a fresh RTM_GET; only the first one, abandoned
         * at the reset, is never read. */
        CHECK(f.blipped && !f.stale_sent && f.route_sockets == 2 && f.route_writes == f.route_reads + 1 &&
              n.sequence == (int)f.route_writes && f.route_socket_ms < n.stable_ms &&
              f.last_route_socket_ms >= n.stable_ms);
    /* Never stable within the IP deadline: the existing readiness failure, nothing sent. */
    if (fault == ADDRESS_LOST || fault == ADDRESS_FLAP)
        CHECK(!strcmp(n.reason, "readiness-gate") && n.failed_stage == PPP_LTE_IP && !n.stable_ms &&
              !f.sockets && !f.route_writes && !f.connects && n.pinned_ms == f.first_address_ms);
    if (fault == ADDRESS_LOST) CHECK(!n.pinned && n.address_resets == 1);
    if (fault == ADDRESS_FLAP) CHECK(n.address_resets >= 2);
    /* Ambiguity stays an immediate failure in the IP stage, pinned or not. */
    if (fault == ADDRESS_DUPLICATE || fault == ADDRESS_DUPLICATE_LATE)
        CHECK(!strcmp(n.reason, "address-ambiguous") && n.failed_stage == PPP_LTE_IP &&
              !n.address_resets && !f.sockets && !f.connects && n.pinned == (fault == ADDRESS_DUPLICATE_LATE));
    /* After the IP stage the old rules hold: loss and change fail at once. */
    if (fault == ADDRESS_LOST_OPEN)
        CHECK(!strcmp(n.reason, "address-missing") && n.failed_stage == PPP_LTE_OPEN && !f.connects);
    if (fault == ADDRESS_CHANGE_CONNECT)
        CHECK(!strcmp(n.reason, "address-changed") && n.failed_stage == PPP_LTE_CONNECTED &&
              f.connects == 1 && !f.sends);
    if (fault == ADDRESS_LOST_OPEN || fault == ADDRESS_CHANGE_CONNECT) CHECK(!n.address_resets);
    const char *first_reason = n.reason;
    enum ppp_lte_stage first_stage = n.failed_stage;
    ppp_lte_network_abort(&n, "repeat-abort"); ppp_lte_network_abort(&n, "repeat-abort");
    /* The first reason and its stage are sticky across later fail()/abort calls. */
    CHECK(n.reason == first_reason && n.failed_stage == first_stage);
    if (result == 1) CHECK(n.failed_stage == PPP_LTE_FAILED && !n.reason);
    else CHECK(n.failed_stage != PPP_LTE_FAILED && n.reason);
    /* Loss time: the failing read's last_ms for a pin lost/changed after the IP
     * stage, 0 for any other outcome (success, IP-stage resets, ambiguity, never
     * pinned, later failures). */
    if (fault == ADDRESS_LOST_OPEN || fault == ADDRESS_CHANGE_CONNECT)
        CHECK(f.lost_address_ms && n.lost_ms == f.lost_address_ms && n.lost_ms > n.stable_ms &&
              n.stable_ms > n.pinned_ms);
    else CHECK(!n.lost_ms);
    if (fault == ADDRESS_MISSING || fault == ADDRESS_ERROR || fault == STOP_GATE)
        CHECK(!n.pinned && !n.pinned_ms && n.failed_stage == PPP_LTE_IP);
    if (fault == CONNECT_ERROR) CHECK(n.failed_stage == PPP_LTE_OPEN);
    if (fault == POLL_ERROR) CHECK(n.failed_stage == PPP_LTE_CONNECT);
    if (fault == SEND_ERROR) CHECK(n.failed_stage == PPP_LTE_SEND);
    if (fault == SHUTDOWN_ERROR) CHECK(n.failed_stage == PPP_LTE_SHUTDOWN);
    if (fault == RECEIVE_ERROR) CHECK(n.failed_stage == PPP_LTE_RECEIVE);
    CHECK(f.connects <= 1);
    /* Every descriptor that was opened is closed exactly once. */
    CHECK(f.route_closes == (fault == ROUTE_SOCKET ? 0u : f.route_sockets));
    CHECK(f.route_sockets <= (fault == ADDRESS_BLIP_ROUTE ? 2u : 1u));
    CHECK(f.tcp_closes == (f.tcp_sockets && fault != TCP_SOCKET ? 1u : 0u));
    CHECK(n.socket_fd == -1 && n.route_fd == -1);
    if (fault <= BOUND_WRONG && fault != NONE && fault != TRANSIENT && fault != ADDRESS_CHANGE) CHECK(!f.sends);
    if (fault >= ROUTE_SOCKET && fault <= BOUND_WRONG) CHECK(!f.connects);
    if (fault == GATE_DEADLINE_ROUTE) CHECK(!f.route_writes && !f.connects && !f.sends);
    if (fault == GATE_DEADLINE_CONNECT) CHECK(!f.connects && !f.sends);
    if (fault == GATE_DEADLINE_SEND) CHECK(!f.sends);
}
int main(void) {
    for (enum fault fault = NONE; fault <= ADDRESS_CHANGE_CONNECT; fault++) transaction(fault);
    const uint32_t denied[] = {0, 0x7f000001, 0xa9fe0001, 0xe0000001, 0xffffffff, 0xf0000001};
    for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        CHECK(ppp_lte_endpoint_allowed((struct in_addr){.s_addr = htonl(denied[i])}) == -1);
    printf("lte-network checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
