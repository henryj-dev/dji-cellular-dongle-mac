#define _DARWIN_C_SOURCE
#include "ppp_lte_network.h"
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int ppp_lte_endpoint_allowed(struct in_addr address) {
    uint32_t host = ntohl(address.s_addr);
    return host && host != 0xffffffffu && (host >> 24) != 127 &&
        (host & 0xffff0000u) != 0xa9fe0000u && (host >> 28) < 0xe ? 0 : -1;
}
static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || (uint64_t)ts.tv_sec > (UINT64_MAX - 999) / 1000)
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void close_once(int *fd, unsigned *count) {
    if (*fd < 0) return;
    (void)close(*fd); /* No retry: the descriptor may already be released. */
    (*count)++; *fd = -1;
}
static int fail(struct ppp_lte_network *n, const char *reason) {
    close_once(&n->socket_fd, &n->socket_closes);
    close_once(&n->route_fd, &n->route_closes);
    /* Keep the first cause; wrapper gates only add context when none exists. */
    if (!n->reason) { n->reason = reason; n->failed_stage = n->stage; }
    n->stage = PPP_LTE_FAILED;
    return -1;
}
static int deadline(struct ppp_lte_network *n) {
    uint64_t now = now_ms();
    if (now == UINT64_MAX || now < n->last_ms || now >= n->overall_deadline) return -1;
    n->last_ms = now;
    if (n->stage == PPP_LTE_IP && now >= n->ip_deadline) return -1;
    if (n->stage >= PPP_LTE_OPEN && now >= n->app_deadline) return -1;
    return 0;
}
static int eligible(struct ppp_lte_network *n) {
    struct ppp_trial_interface current = {0};
    if (n->gate(n->context, &current) || !memchr(current.name, 0, sizeof(current.name)) ||
        strcmp(current.name, n->interface.name) || current.index != n->interface.index ||
        !current.index || current.index > USHRT_MAX) return fail(n, "interface-changed");
    return 0;
}
/* gate can spend time in CF/proc callbacks; check the clock AFTER it too. */
static int checked(struct ppp_lte_network *n) {
    if (deadline(n) || eligible(n) || deadline(n)) return fail(n, "readiness-gate");
    return 0;
}
static int addresses(struct ppp_lte_network *n) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return fail(n, "address-error");
    unsigned found = 0, walked = 0;
    struct in_addr local = {0}, peer = {0};
    for (struct ifaddrs *item = list; item; item = item->ifa_next) {
        if (++walked > 1024) { found = 2; break; }
        if (!item->ifa_addr || !item->ifa_name || strcmp(item->ifa_name, n->interface.name) ||
            item->ifa_addr->sa_family != AF_INET) continue;
        if (!item->ifa_dstaddr || item->ifa_addr->sa_len != sizeof(struct sockaddr_in) ||
            item->ifa_dstaddr->sa_len != sizeof(struct sockaddr_in) ||
            item->ifa_dstaddr->sa_family != AF_INET ||
            (item->ifa_flags & (IFF_POINTOPOINT | IFF_UP)) != (IFF_POINTOPOINT | IFF_UP)) {
            found = 2; break;
        }
        local = ((struct sockaddr_in *)item->ifa_addr)->sin_addr;
        peer = ((struct sockaddr_in *)item->ifa_dstaddr)->sin_addr;
        if (ppp_lte_endpoint_allowed(local) || ppp_lte_endpoint_allowed(peer) ||
            local.s_addr == n->endpoint.s_addr || local.s_addr == peer.s_addr) { found = 2; break; }
        found++;
    }
    freeifaddrs(list);
    if (found > 1) return fail(n, "address-ambiguous");
    int moved = n->pinned && (!found || local.s_addr != n->local.s_addr || peer.s_addr != n->peer.s_addr);
    /* Before the TCP socket exists a vanished or replaced pair restarts the
     * settle wait; the IP deadline still bounds it. The route socket is closed
     * too: it is not read during the new wait, so kernel broadcasts would fill
     * its buffer and a later RTM_GET reply could be dropped. The next settle
     * opens a fresh socket and query (new sequence). */
    if (moved && n->stage == PPP_LTE_IP) {
        n->pinned = 0; n->settle_start_ms = 0; n->stable_ms = 0; n->route_pending = 0;
        close_once(&n->route_fd, &n->route_closes);
        if (n->address_resets != UINT_MAX) n->address_resets++;
        return 1;
    }
    if (!found && !n->pinned) return 1;
    /* Diagnostic only, recorded with the first reason; never a decision input. */
    if (moved) {
        if (!n->reason) n->lost_ms = n->last_ms;
        return fail(n, found ? "address-changed" : "address-missing");
    }
    if (!n->pinned) {
        if (!n->pinned_ms) n->pinned_ms = n->last_ms;
        n->settle_start_ms = n->last_ms;
    }
    n->local = local; n->peer = peer; n->pinned = 1;
    return 0;
}
static int route_request(struct ppp_lte_network *n) {
    if (n->route_pending) return 0;
    if (n->sequence == INT_MAX) return fail(n, "route-sequence");
    struct { struct rt_msghdr header; struct sockaddr_in destination; } message = {0};
    message.header.rtm_msglen = sizeof(message); message.header.rtm_version = RTM_VERSION;
    message.header.rtm_type = RTM_GET; message.header.rtm_index = (u_short)n->interface.index;
    message.header.rtm_flags = RTF_UP | RTF_HOST | RTF_IFSCOPE; message.header.rtm_addrs = RTA_DST;
    message.header.rtm_pid = getpid(); message.header.rtm_seq = ++n->sequence;
    message.destination.sin_len = sizeof(message.destination);
    message.destination.sin_family = AF_INET; message.destination.sin_addr = n->endpoint;
    if (checked(n)) return fail(n, "route-gate");
    ssize_t count = write(n->route_fd, &message, sizeof(message));
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    if (count != (ssize_t)sizeof(message)) return fail(n, "route-query-error");
    n->route_pending = 1; n->route_sent = 1; n->route_pid = message.header.rtm_pid;
    return 0;
}
static int route_response(struct ppp_lte_network *n) {
    unsigned char buffer[512];
    ssize_t count;
    struct rt_msghdr header;
    /* AF_ROUTE also delivers other processes' and kernel broadcasts (NEWADDR,
     * NEWMADDR, IFINFO, ...), common right after PPP comes up and often shorter
     * than rt_msghdr. Only the shared msglen/version/type prefix is required
     * before skipping them; the IP/app deadlines bound the wait. Only our own
     * reply can fail. */
    for (unsigned skipped = 0;; skipped++) {
        if (skipped == 8) return 0;
        count = read(n->route_fd, buffer, sizeof(buffer));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (count < 4) return fail(n, "route-response-error");
        if (buffer[3] != RTM_GET) continue;
        if (count < (ssize_t)sizeof(header)) return fail(n, "route-response-error");
        memcpy(&header, buffer, sizeof(header));
        if (header.rtm_pid == n->route_pid && header.rtm_seq == n->sequence) break;
    }
    if ((size_t)count == sizeof(buffer)) return fail(n, "route-response-error");
    if (header.rtm_msglen != count || header.rtm_version != RTM_VERSION ||
        header.rtm_index != n->interface.index || header.rtm_errno ||
        (header.rtm_addrs & ~((1 << RTAX_MAX) - 1)) ||
        (header.rtm_flags & (RTF_UP | RTF_REJECT | RTF_BLACKHOLE)) != RTF_UP)
        return fail(n, "route-invalid");
    size_t offset = sizeof(header);
    struct in_addr destination = {0}, mask = {0};
    int have_destination = 0, have_mask = 0;
    for (unsigned bit = 0; bit < RTAX_MAX; bit++) {
        if (!(header.rtm_addrs & (1 << bit))) continue;
        if (offset >= (size_t)count || offset % sizeof(uint32_t)) return fail(n, "route-alignment");
        size_t length = buffer[offset];
        size_t rounded = length ? (length + 3u) & ~3u : sizeof(uint32_t);
        if (rounded > (size_t)count - offset || length > (size_t)count - offset)
            return fail(n, "route-bounds");
        if (bit == RTAX_NETMASK) {
            /* Darwin trims trailing zero mask bytes, including a zero-length
             * default mask. Missing address bytes are zero, never read past sa_len. */
            if (length > sizeof(struct sockaddr_in) || (length == 1) ||
                (length >= 2 && buffer[offset + 1] != AF_UNSPEC && buffer[offset + 1] != AF_INET))
                return fail(n, "route-mask");
            unsigned char bytes[4] = {0};
            for (size_t i = 0; i < sizeof(bytes); i++)
                if (4 + i < length) bytes[i] = buffer[offset + 4 + i];
            memcpy(&mask.s_addr, bytes, sizeof(bytes));
            have_mask = 1;
        } else {
            if (length < sizeof(struct sockaddr)) return fail(n, "route-bounds");
            if (bit == RTAX_DST) {
                struct sockaddr_in address;
                if (length != sizeof(address)) return fail(n, "route-destination");
                memcpy(&address, buffer + offset, sizeof(address));
                if (address.sin_family != AF_INET) return fail(n, "route-destination");
                destination = address.sin_addr; have_destination = 1;
            }
        }
        offset += rounded;
    }
    if (!have_destination || offset != (size_t)count) return fail(n, "route-incomplete");
    if (header.rtm_flags & RTF_HOST) {
        if (destination.s_addr != n->endpoint.s_addr) return fail(n, "route-destination");
    } else {
        uint32_t bits = ntohl(mask.s_addr), inverse = ~bits;
        if (!have_mask || (inverse & (inverse + 1u)) ||
            (n->endpoint.s_addr & mask.s_addr) != (destination.s_addr & mask.s_addr))
            return fail(n, "route-prefix");
    }
    n->route_pending = 0;
    return 1;
}
static int socket_prepare(struct ppp_lte_network *n) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fail(n, "socket-error");
    n->socket_fd = fd;
    int value = 1, bound = (int)n->interface.index, read_back = 0;
    socklen_t length = sizeof(read_back);
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) ||
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value)) ||
        setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &bound, sizeof(bound)) ||
        getsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &read_back, &length) ||
        length != sizeof(read_back) || read_back != bound) return fail(n, "socket-option-error");
    return 0;
}
static int connect_once(struct ppp_lte_network *n) {
    if (n->connected_once) return fail(n, "connect-repeat");
    struct sockaddr_in destination = {.sin_len = sizeof(destination), .sin_family = AF_INET,
        .sin_port = htons(n->port), .sin_addr = n->endpoint};
    if (checked(n)) return fail(n, "connect-gate");
    n->connected_once = 1;
    int result = connect(n->socket_fd, (struct sockaddr *)&destination, sizeof(destination));
    if (!result) { n->stage = PPP_LTE_CONNECTED; return 0; }
    if (errno == EINPROGRESS || errno == EINTR || errno == EAGAIN) {
        n->stage = PPP_LTE_CONNECT; return 0;
    }
    return fail(n, "connect-error");
}
static int connect_completion(struct ppp_lte_network *n) {
    struct pollfd descriptor = {.fd = n->socket_fd, .events = POLLOUT};
    int ready = poll(&descriptor, 1, 0);
    if (!ready || (ready < 0 && errno == EINTR)) return 0;
    if (ready < 0 || (descriptor.revents & POLLNVAL)) return fail(n, "connect-poll-error");
    int error = 0; socklen_t length = sizeof(error);
    if (getsockopt(n->socket_fd, SOL_SOCKET, SO_ERROR, &error, &length) ||
        length != sizeof(error) || error) return fail(n, "connect-completion-error");
    if (!(descriptor.revents & POLLOUT) || (descriptor.revents & (POLLERR | POLLHUP)))
        return fail(n, "connect-poll-error");
    n->stage = PPP_LTE_CONNECTED; return 0;
}
int ppp_lte_network_init(struct ppp_lte_network *n, struct in_addr endpoint,
                         uint16_t port, uint64_t start_ms,
                         int (*gate)(void *, struct ppp_trial_interface *), void *context) {
    if (!n || !port || !gate || ppp_lte_endpoint_allowed(endpoint) || start_ms > UINT64_MAX - 60000)
        return -1;
    memset(n, 0, sizeof(*n));
    n->endpoint = endpoint; n->port = port; n->gate = gate; n->context = context;
    n->socket_fd = n->route_fd = -1;
    n->overall_deadline = start_ms + 60000; n->ip_deadline = start_ms + 30000;
    n->last_ms = start_ms; n->failed_stage = PPP_LTE_FAILED;
    if (gate(context, &n->interface) || !n->interface.index || n->interface.index > USHRT_MAX ||
        !n->interface.name[0] || !memchr(n->interface.name, 0, sizeof(n->interface.name)))
        return fail(n, "interface-unproven");
    return 0;
}
int ppp_lte_network_step(struct ppp_lte_network *n) {
    if (!n || n->stage == PPP_LTE_DONE || n->stage == PPP_LTE_FAILED) return -1;
    if (checked(n)) return fail(n, n->reason ? n->reason : "deadline");
    int address = addresses(n);
    if (address) return address < 0 ? -1 : 0;
    if (checked(n)) return fail(n, n->reason ? n->reason : "deadline");
    /* No route socket, query or TCP socket until one pair has held for the
     * whole settle window. last_ms never decreases, so this cannot underflow. */
    if (n->stage == PPP_LTE_IP) {
        if (n->last_ms - n->settle_start_ms < PPP_LTE_ADDRESS_SETTLE_MS) return 0;
        if (!n->stable_ms) n->stable_ms = n->last_ms;
    }
    if (n->route_fd < 0) {
        n->route_fd = socket(AF_ROUTE, SOCK_RAW, 0);
        if (n->route_fd < 0) return fail(n, "route-socket-error");
        int flags = fcntl(n->route_fd, F_GETFL);
        if (flags < 0 || fcntl(n->route_fd, F_SETFL, flags | O_NONBLOCK))
            return fail(n, "route-option-error");
        return 0;
    }
    /* A fresh RTM_GET precedes every socket stage, including each partial write.
     * No route cache is treated as continuing proof. */
    if (!n->route_pending) return route_request(n);
    int matched = route_response(n);
    if (matched <= 0) return matched;
    if (checked(n)) return fail(n, n->reason ? n->reason : "deadline");
    if (n->stage == PPP_LTE_IP) {
        n->app_deadline = n->last_ms + 10000;
        if (n->app_deadline > n->overall_deadline) n->app_deadline = n->overall_deadline;
        if (socket_prepare(n)) return -1;
        n->stage = PPP_LTE_OPEN; return 0;
    }
    if (n->stage == PPP_LTE_OPEN) return connect_once(n);
    if (n->stage == PPP_LTE_CONNECT) return connect_completion(n);
    if (n->stage == PPP_LTE_CONNECTED) { n->stage = PPP_LTE_SEND; return 0; }
    if (n->stage == PPP_LTE_SEND) {
        size_t total = sizeof(PPP_LTE_PROBE) - 1;
        ssize_t count = send(n->socket_fd, &PPP_LTE_PROBE[n->sent], total - n->sent, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (count <= 0 || (size_t)count > total - n->sent) return fail(n, "send-error");
        n->sent += (size_t)count;
        if (n->sent == total) n->stage = PPP_LTE_SHUTDOWN;
        return 0;
    }
    if (n->stage == PPP_LTE_SHUTDOWN) {
        if (shutdown(n->socket_fd, SHUT_WR)) return fail(n, "shutdown-error");
        n->write_closed = 1; n->stage = PPP_LTE_RECEIVE; return 0;
    }
    if (n->stage == PPP_LTE_RECEIVE) {
        unsigned char byte;
        ssize_t count = recv(n->socket_fd, &byte, 1, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (count < 0) return fail(n, "receive-error");
        if (!count) {
            if (n->received != sizeof(PPP_LTE_PROBE) - 1) return fail(n, "response-mismatch");
            close_once(&n->socket_fd, &n->socket_closes);
            close_once(&n->route_fd, &n->route_closes);
            n->stage = PPP_LTE_DONE; return 1;
        }
        if (count != 1 || n->received >= sizeof(PPP_LTE_PROBE) - 1 ||
            byte != (unsigned char)PPP_LTE_PROBE[n->received]) return fail(n, "response-mismatch");
        n->received++;
    }
    return 0;
}
void ppp_lte_network_abort(struct ppp_lte_network *n, const char *reason) {
    if (!n || n->stage == PPP_LTE_DONE) return;
    (void)fail(n, reason ? reason : "aborted");
}
