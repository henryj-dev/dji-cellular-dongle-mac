#define _DARWIN_C_SOURCE
/* Real once runner logic + real network state machine in one TU. Only the
 * runner/observer/discovery owners and the socket/route/address OS boundary
 * are fakes; session/runner contracts stay covered by ppp_usb_test. */
#include "discovery_internal.h"
#include "discovery.h"
#include "ppp_lte_network.h"
#include "ppp_runner.h"
#include "ppp_trial_preflight.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-once-integration line=%d\n", __LINE__); } } while (0)
enum scenario { ECHO, INTERFACE_CHANGE, SIGNAL_RECEIVE, ROUTE_INDEX, ADDRESS_TIMEOUT, ADDRESS_LOST,
    ADDRESS_BLIP };
static struct {
    enum scenario scenario;
    uint64_t now;
    unsigned slices, samples, restores, released, closed, ended, stopped_at;
    unsigned addresses, route_writes, route_reads, connects, sends, shutdowns, receives;
    unsigned route_closes, tcp_closes, receives_after_signal;
    int bound;
    size_t sent, received;
    unsigned char request[512]; size_t request_size;
    struct sockaddr_in local, peer;
    struct ifaddrs address;
    struct ppp_runner_status status;
    struct ppp_session_observation child;
} f;
static uid_t fake_uid(void *context) { (void)context; return 0; }
static int fake_home(void *context, uid_t uid, const char **home) { (void)context; (void)uid; *home = "/fixture"; return 0; }
static int fake_metadata(void *context, const char *path, struct stat *st) {
    (void)context; *st = (struct stat){.st_mode = !strcmp(path, "/usr/sbin/pppd") ? S_IFREG | 0755 : S_IFDIR | 0755};
    return 0;
}
static int fake_identity(void *context) { (void)context; return 0; }
static const char *fake_executable(const struct trial_backend *b) { (void)b; return "clear"; }
static int fake_preflight(void *context, struct trial_preflight *p) {
    (void)context;
    *p = (struct trial_preflight){.status = TRIAL_PREFLIGHT_COMPLETE, .metadata.clear = 1,
        .backend = {&f, fake_uid, fake_home, fake_metadata}};
    return 0;
}
/* Shared by once and network: both read the same monotonic fixture clock. */
static int fake_clock(clockid_t clock, struct timespec *ts) {
    CHECK(clock == CLOCK_MONOTONIC);
    f.now += 10; ts->tv_sec = (time_t)(f.now / 1000); ts->tv_nsec = (long)(f.now % 1000) * 1000000;
    return 0;
}
static int fake_sleep(const struct timespec *delay, struct timespec *remaining) { (void)delay; (void)remaining; return 0; }
static void fake_observer_begin(struct ppp_trial_observer *o) { memset(o, 0, sizeof(*o)); }
static void fake_observer_child(struct ppp_trial_observer *o, pid_t pid) { CHECK(pid == 789); o->identity = 1; }
static void fake_observer_sample(struct ppp_trial_observer *o) {
    CHECK(!f.child.reaped && !f.child.ownership_lost); f.samples++;
    o->current_valid = 1; o->attributed = 1;
}
static int fake_observer_current(const struct ppp_trial_observer *o, struct ppp_trial_interface *i) {
    if (!o->current_valid) return -1;
    *i = (struct ppp_trial_interface){.name = "ppp-fixture", .index = 7};
    if (f.scenario == INTERFACE_CHANGE && f.connects) i->index = 8;
    return 0;
}
static void fake_observer_finish(struct ppp_trial_observer *o) { (void)o; CHECK(f.released); }
static int fake_observer_result(const struct ppp_trial_observer *o) { (void)o; return 0; }
static int fake_observer_diagnostic(const struct ppp_trial_observer *o, char *b, size_t n) {
    (void)o; return snprintf(b, n, "attribution=fixture");
}
static void fake_observer_dispose(struct ppp_trial_observer *o) { (void)o; }
static int fake_session_start(const struct ppp_session_request *r, const struct ppp_metadata_backend *b,
                              struct ppp_session **s) {
    (void)r; (void)b; *s = (struct ppp_session *)&f; return 0;
}
static int fake_session_stop(struct ppp_session *s) { (void)s; CHECK(0); return -1; }
static int fake_session_step(struct ppp_session *s) { (void)s; CHECK(0); return -1; }
static int fake_session_release(struct ppp_session **s) { (void)s; CHECK(0); return -1; }
static int fake_runner_begin(struct ppp_session **s, struct ppp_runner **r) {
    *s = NULL; *r = (struct ppp_runner *)&f; return 0;
}
static int fake_runner_observe(struct ppp_runner *r, struct ppp_runner_status *s, struct ppp_session_observation *o) {
    (void)r; *s = f.status; *o = f.child; return 0;
}
static int fake_runner_slice(struct ppp_runner *r, const volatile sig_atomic_t *stop,
                             const uint32_t *wait, struct ppp_runner_status *s) {
    (void)r; (void)wait; f.slices++;
    if (f.slices == 1) {
        f.child = (struct ppp_session_observation){.pid = 789, .started = 1};
        f.status.session.state = PPP_SESSION_RUNNING;
    }
    if (!f.closed && (*stop || f.status.signal_number)) {
        f.status.stop_requested = 1;
        if (!f.ended) { f.ended = 1; f.stopped_at = f.slices; }
        if (f.slices - f.stopped_at >= 3) {
            f.child.term_signals = f.child.kill_signals = 1;
            f.child.reaped = f.child.stdout_eof = f.child.stderr_eof = f.child.peer_quiet = 1;
            f.status.session.local_done = 1; f.status.session.state = PPP_SESSION_CLOSED;
            f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR;
            f.status.session.supervisor_valid = 1;
            f.status.session.supervisor_error = PPP_ERROR_TIMEOUT;
            f.closed = 1;
        }
    }
    CHECK(f.slices < 100000);
    *s = f.status; return f.status.session.local_done;
}
static int fake_runner_restore(struct ppp_runner *r, struct ppp_runner_status *s) {
    (void)r; CHECK(f.closed); f.restores++; *s = f.status; return 0;
}
static int fake_runner_release(struct ppp_runner **r) {
    CHECK(f.closed && f.restores && f.tcp_closes <= 1 && f.route_closes <= 1);
    f.released++; *r = NULL; return 0;
}
static int fake_discovery(discovery_target_callback callback, void *context) {
    struct descriptor_config config = {0};
    IOUSBDeviceInterface *device = NULL;
    return callback(context, 1, &device, 42, &config);
}
static int fake_getifaddrs(struct ifaddrs **list) {
    f.addresses++;
    if (f.scenario == ADDRESS_TIMEOUT) { f.now += 1000; *list = NULL; return 0; }
    /* ADDRESS_LOST: the pinned IPv4 disappears after the one connect.
     * ADDRESS_BLIP: it is gone for reads 3-5, during the settle wait. */
    if ((f.scenario == ADDRESS_LOST && f.connects) ||
        (f.scenario == ADDRESS_BLIP && f.addresses >= 3 && f.addresses <= 5)) { *list = NULL; return 0; }
    f.local = (struct sockaddr_in){.sin_len = sizeof(f.local), .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(0x0a000001)};
    f.peer = f.local; f.peer.sin_addr.s_addr = htonl(0x0a000002);
    f.address = (struct ifaddrs){.ifa_name = "ppp-fixture", .ifa_flags = IFF_UP | IFF_POINTOPOINT,
        .ifa_addr = (struct sockaddr *)&f.local, .ifa_dstaddr = (struct sockaddr *)&f.peer};
    *list = &f.address; return 0;
}
static void fake_freeifaddrs(struct ifaddrs *list) { CHECK(!list || list == &f.address); }
static int fake_socket(int domain, int type, int protocol) {
    (void)type; (void)protocol; return domain == AF_ROUTE ? 40 : 41;
}
static int fake_close(int fd) {
    CHECK(fd == 40 || fd == 41);
    if (fd == 40) f.route_closes++; else f.tcp_closes++;
    return 0;
}
static int fake_fcntl(int fd, int command, ...) { (void)fd; (void)command; return 0; }
static int fake_setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    (void)fd; (void)size;
    if (level == IPPROTO_IP && option == IP_BOUND_IF) f.bound = *(const int *)value;
    return 0;
}
static int fake_getsockopt(int fd, int level, int option, void *value, socklen_t *size) {
    (void)fd; (void)option; (void)size;
    *(int *)value = level == IPPROTO_IP ? f.bound : 0; return 0;
}
static pid_t fake_getpid(void) { return 456; }
static ssize_t fake_write(int fd, const void *data, size_t size) {
    CHECK(fd == 40 && size <= sizeof(f.request)); f.route_writes++;
    memcpy(f.request, data, size); f.request_size = size; return (ssize_t)size;
}
static ssize_t fake_read(int fd, void *data, size_t size) {
    CHECK(fd == 40 && size >= f.request_size); f.route_reads++;
    struct rt_msghdr h; memcpy(&h, f.request, sizeof(h));
    if (f.scenario == ROUTE_INDEX) h.rtm_index++;
    memcpy(data, f.request, f.request_size); memcpy(data, &h, sizeof(h));
    return (ssize_t)f.request_size;
}
static int fake_connect(int fd, const struct sockaddr *address, socklen_t size) {
    CHECK(fd == 41 && size == sizeof(struct sockaddr_in) && address->sa_family == AF_INET && f.bound == 7);
    f.connects++; return 0;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout) { (void)fds; (void)count; (void)timeout; CHECK(0); return -1; }
static ssize_t fake_send(int fd, const void *data, size_t size, int flags) {
    CHECK(fd == 41 && !flags && !memcmp(data, &PPP_LTE_PROBE[f.sent], size)); f.sends++;
    f.sent += size; return (ssize_t)size;
}
static int fake_shutdown(int fd, int how) { CHECK(fd == 41 && how == SHUT_WR); f.shutdowns++; return 0; }
static ssize_t fake_recv(int fd, void *data, size_t size, int flags) {
    CHECK(fd == 41 && size == 1 && !flags && f.shutdowns == 1); f.receives++;
    if (f.status.signal_number) f.receives_after_signal++;
    if (f.received == sizeof(PPP_LTE_PROBE) - 1) return 0;
    *(char *)data = PPP_LTE_PROBE[f.received++];
    /* The signal lands after the first echoed byte. */
    if (f.scenario == SIGNAL_RECEIVE && f.received == 1) f.status.signal_number = SIGINT;
    return 1;
}
int trial_preflight(void *context, struct trial_preflight *p) { return fake_preflight(context, p); }
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
#define trial_identity fake_identity
#define trial_metadata fake_metadata
#define trial_executable fake_executable
#define trial_preflight(...) fake_preflight(__VA_ARGS__)
#define nanosleep fake_sleep
#define ppp_trial_observer_begin fake_observer_begin
#define ppp_trial_observer_child fake_observer_child
#define ppp_trial_observer_sample fake_observer_sample
#define ppp_trial_observer_current fake_observer_current
#define ppp_trial_observer_finish fake_observer_finish
#define ppp_trial_observer_result fake_observer_result
#define ppp_trial_observer_diagnostic fake_observer_diagnostic
#define ppp_trial_observer_dispose fake_observer_dispose
#define ppp_session_start fake_session_start
#define ppp_session_stop fake_session_stop
#define ppp_session_step fake_session_step
#define ppp_session_release fake_session_release
#define ppp_runner_begin fake_runner_begin
#define ppp_runner_slice fake_runner_slice
#define ppp_runner_observe fake_runner_observe
#define ppp_runner_restore fake_runner_restore
#define ppp_runner_release fake_runner_release
#define discovery_with_one_target fake_discovery
#define main once_main
#include "../src/ppp_lte_once.c"
#undef main

static const char *expected_text(enum scenario s) {
    switch (s) {
    case ECHO: return "app-echo=exact-eof reason=echo-observed payload-sent=14 payload-received=14"
        " local=closed session-error=supervisor-timeout cleanup=none clock=ok";
    case INTERFACE_CHANGE: return "app-echo=unproven reason=interface-changed payload-sent=0 payload-received=0";
    case SIGNAL_RECEIVE: return "app-echo=unproven reason=child-or-clock-unproven payload-sent=14 payload-received=1";
    case ROUTE_INDEX: return "app-echo=unproven reason=route-invalid payload-sent=0";
    case ADDRESS_TIMEOUT: return "app-echo=unproven reason=readiness-gate payload-sent=0";
    case ADDRESS_LOST: return "app-echo=unproven reason=address-missing payload-sent=0";
    case ADDRESS_BLIP: return "app-echo=exact-eof reason=echo-observed payload-sent=14 payload-received=14";
    }
    return NULL;
}
int main(void) {
    char *argv[] = {"once", "--lte-once", "--endpoint", "192.0.2.1", "--port", "1234"};
    for (enum scenario s = ECHO; s <= ADDRESS_BLIP; s++) {
        memset(&f, 0, sizeof(f)); f.scenario = s; f.now = 1000;
        FILE *output = tmpfile(); CHECK(output != NULL); if (!output) continue;
        CHECK(lte_once(6, argv, output, NULL) == 3);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        char text[2048] = {0}; (void)fread(text, 1, sizeof(text) - 1, output);
        CHECK(fclose(output) == 0);
        if (!strstr(text, expected_text(s))) { failures++; fprintf(stderr, "FAIL lte-once-integration scenario=%d text=%s\n", (int)s, text); }
        checks++;
        CHECK(!strstr(text, "192.0.2.1") && !strstr(text, "10.0.0.") && !strstr(text, "1234") && !strstr(text, "/fixture"));
        /* One connect at most, every opened socket closed exactly once, cleanup completed. */
        CHECK(f.connects <= 1 && f.released == 1 && f.restores == 1);
        CHECK(f.tcp_closes == (f.connects || s == ECHO || s == SIGNAL_RECEIVE || s == INTERFACE_CHANGE ||
            s == ADDRESS_BLIP ? 1u : 0u));
        CHECK(f.route_closes == (s == ADDRESS_TIMEOUT ? 0u : 1u));
        if (s == ECHO || s == ADDRESS_BLIP)
            CHECK(f.connects == 1 && f.shutdowns == 1 && f.sent == 14 && f.received == 14);
        if (s == INTERFACE_CHANGE) CHECK(f.connects == 1 && !f.sends && !f.shutdowns);
        if (s == SIGNAL_RECEIVE) CHECK(f.sent == 14 && f.received == 1 && !f.receives_after_signal);
        /* A non-zero failed_stage reaches the once output, not just the default ip. */
        if (s == SIGNAL_RECEIVE) CHECK(strstr(text, " network-stage=receive ") != NULL);
        if (s == ROUTE_INDEX) CHECK(f.route_writes == 1 && f.route_reads == 1 && !f.connects);
        if (s == ADDRESS_TIMEOUT) CHECK(!f.route_writes && !f.connects && f.addresses >= 25);
        /* Real network module: pin time is a number once pinned, the stage is the
         * first failing one (or done), and an address never pinned prints none. */
        const char *ready = strstr(text, " ip-ready-ms=");
        CHECK(ready && (s == ADDRESS_TIMEOUT ? !strncmp(ready + 13, "none ", 5) :
            ready[13] >= '0' && ready[13] <= '9'));
        if (s == ECHO || s == ADDRESS_BLIP) CHECK(strstr(text, " network-stage=done ") != NULL);
        /* Settle: a number at least 3000 after the pin once the pair held (the
         * reset pushes it past the read-6 re-pin), none if never pinned. */
        char *end = NULL;
        unsigned long long pin = ready ? strtoull(ready + 13, &end, 10) : 0;
        const char *settled = strstr(text, " ip-stable-ms=");
        if (s == ADDRESS_TIMEOUT) CHECK(settled && !strncmp(settled + 14, "none ip-resets=0 ", 17));
        else {
            char *after = NULL;
            unsigned long long stable = settled ? strtoull(settled + 14, &after, 10) : 0;
            CHECK(end && *end == ' ' && settled && after && settled[14] >= '0' && settled[14] <= '9' &&
                  stable >= pin + PPP_LTE_ADDRESS_SETTLE_MS);
            CHECK(after && !strncmp(after, s == ADDRESS_BLIP ? " ip-resets=1 " : " ip-resets=0 ", 13));
        }
        if (s == ROUTE_INDEX || s == ADDRESS_TIMEOUT) CHECK(strstr(text, " network-stage=ip ") != NULL);
        /* Loss time is a number only when a pinned address went missing; the
         * child negotiation is zeroed here, so the trace is fresh but empty. */
        const char *lost = strstr(text, " ip-lost-ms=");
        CHECK(lost && (s == ADDRESS_LOST ? lost[12] >= '0' && lost[12] <= '9' : !strncmp(lost + 12, "none ", 5)));
        if (s == ADDRESS_LOST) CHECK(f.connects == 1 && !f.sends && f.addresses >= 2);
        CHECK(strstr(text, " ppp-trace=none ") != NULL && strchr(text, '\n') != NULL);
        CHECK(!strstr(text, "0x"));
    }
    printf("lte-once-integration checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
