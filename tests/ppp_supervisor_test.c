#define _DARWIN_C_SOURCE
#include "../src/ppp_supervisor.h"
#include "../src/ppp_launch.h"
#include "../src/ppp_auth_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures;
static int pause_before_exec;
static void child_before_exec(void) {
    if (pause_before_exec) (void)kill(getpid(), SIGSTOP);
}
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL supervisor line=%d\n", __LINE__); failures++; } } while (0)
struct fake {
    unsigned char input[16384], output[16384];
    size_t length, offset, chunk, written;
    unsigned reads, writes, closes, releases;
    int read_failure, write_failure, partial, close_failure, poisoned;
    uint64_t now, delay, clock_values[8];
    size_t clock_length, clock_offset;
    struct ppp_supervisor *supervisor;
    unsigned begins, quiesces, quiet_polls;
    int quiet_hold, quiet_failure, quiesce_failure;
    int begin_failure;
    uint64_t begin_delay;
};
static struct offline_io peer_read(void *context, unsigned char *data, size_t capacity) {
    struct fake *f = context;
    f->reads++;
    f->now += f->delay;
    if (f->read_failure) {
        f->poisoned = 1;
        return (struct offline_io){(enum offline_io_code)f->read_failure, SIZE_MAX};
    }
    if (f->offset == f->length) return (struct offline_io){OFFLINE_IO_AGAIN, SIZE_MAX};
    size_t length = f->length - f->offset;
    if (length > capacity) length = capacity;
    if (f->chunk && length > f->chunk) length = f->chunk;
    memcpy(data, f->input + f->offset, length); f->offset += length;
    return (struct offline_io){OFFLINE_IO_OK, length};
}
static struct offline_io peer_write(void *context, const unsigned char *data, size_t length) {
    struct fake *f = context;
    f->writes++;
    f->now += f->delay;
    if (f->write_failure) return (struct offline_io){(enum offline_io_code)f->write_failure, SIZE_MAX};
    if (f->partial) return (struct offline_io){OFFLINE_IO_OK, length - 1};
    if (length > sizeof(f->output) - f->written) return (struct offline_io){OFFLINE_IO_ERROR, 0};
    memcpy(f->output + f->written, data, length); f->written += length;
    return (struct offline_io){OFFLINE_IO_OK, length};
}
static uint64_t peer_now(void *context) {
    struct fake *f = context;
    if (f->clock_offset < f->clock_length) f->now = f->clock_values[f->clock_offset++];
    return f->now;
}
static int peer_begin(void *context) {
    struct fake *f = context;
    struct ppp_supervisor *s = f->supervisor;
    f->begins++;
    CHECK(s && s->phase == PPP_CONNECT && s->ready && s->child.exec_done &&
          !s->child.reaped && !s->stdout_eof && !s->error && s->connect_attempted);
    CHECK(s->pending_tx.length && !s->stream.ready && !f->reads && !f->writes);
    if (s->observed_start) CHECK(s->output_observed && ppp_supervisor_ready(s) == -1);
    f->now += f->begin_delay;
    return f->begin_failure;
}
static int peer_quiesce(void *context) {
    struct fake *f = context; f->quiesces++; return f->quiesce_failure;
}
static struct ppp_quiet peer_quiet(void *context) {
    struct fake *f = context; f->quiet_polls++;
    return (struct ppp_quiet){!f->quiet_hold, f->quiet_failure};
}
static int peer_close(void *context) { struct fake *f = context; f->closes++; return f->close_failure; }
static void peer_release(void *context) { ((struct fake *)context)->releases++; }
static uint64_t wall_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void drive(struct ppp_supervisor *s) {
    if (ppp_supervisor_step(s) < 0) CHECK(0);
    struct pollfd fds[] = {{s->child.output_fd, POLLIN, 0}, {s->child.error_fd, POLLIN, 0},
                          {s->child.exec_fd, POLLIN, 0}};
    (void)poll(fds, 3, 1);
}
static void start(struct ppp_supervisor *s, struct fake *f, const char *path, char *mode, size_t capacity) {
    *f = (struct fake){};
    struct ppp_peer peer = {f, peer_read, peer_write, peer_now, peer_close, peer_release,
                            peer_quiesce, peer_quiet, NULL};
    struct ppp_budgets budgets = {100, 200, 300, 400};
    char *args[] = {(char *)path, mode, NULL}, *env[] = {"PPP_FIXTURE=explicit", NULL};
    CHECK(ppp_supervisor_start(s, &peer, &budgets, capacity, path, args, env, "/") == 0);
}
static void ready(struct ppp_supervisor *s) {
    uint64_t deadline = wall_now() + 3000;
    while ((!s->child.exec_done || s->pending_tx.length < 3) && wall_now() < deadline && !s->error)
        drive(s);
    size_t length;
    const unsigned char *span = offline_queue_peek(&s->pending_tx, &length);
    CHECK(s->child.exec_done && !s->child.exec_error && length >= 3 && !memcmp(span, "RDY", 3));
    CHECK(s->phase == PPP_START && !s->stream.ready && !s->peer_done);
    CHECK(ppp_supervisor_ready(s) == 0);
    CHECK(ppp_supervisor_ready(s) == -1);
}
static void closed(struct ppp_supervisor *s, struct fake *f) {
    uint64_t deadline = wall_now() + 3000;
    while (s->phase != PPP_CLOSED && wall_now() < deadline) drive(s);
    CHECK(s->phase == PPP_CLOSED && s->child.closed && s->child.reaped);
    CHECK(s->child.reap_count == 1 && s->child.fd_closes == 4);
    CHECK(f->closes == 1 && f->releases == 1 && s->stream.cleanup_done);
    int status;
    CHECK(waitpid(s->child.pid, &status, WNOHANG) == -1 && errno == ECHILD);
    CHECK(ppp_supervisor_step(s) == 1 && f->closes == 1 && f->releases == 1);
}
static void force_cleanup(struct ppp_supervisor *s, struct fake *f) {
    if (s->phase != PPP_CLOSED && s->phase != PPP_CLEANUP && !s->error) {
        f->now = s->deadline_ms;
        drive(s);
    }
    closed(s, f);
}
static void connect_peer(struct ppp_supervisor *s, struct fake *f) {
    memcpy(f->input, "CONNECT\r\n", 9); f->length = 9;
    uint64_t deadline = wall_now() + 3000;
    while (!s->connected && !s->error && wall_now() < deadline) drive(s);
    CHECK(s->connected && s->stream.ready && s->phase == PPP_SESSION);
    CHECK(s->deadline_ms == f->now + s->budgets.session_ms);
}
static int fd_count(void) {
    int count = 0;
    for (int i = 0; i < 1024; i++) if (fcntl(i, F_GETFD) >= 0) count++;
    return count;
}
static void transfer_test(const char *path, size_t capacity, size_t chunk) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "echo", capacity);
    ready(&s);
    CHECK(!f.reads && !f.writes && s.pending_tx.length == 3);
    f.chunk = chunk;
    memcpy(f.input, "CONNECT\r\n", 9);
    unsigned char raw[3072];
    for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)i;
    memcpy(f.input + 9, raw, sizeof(raw)); f.length = 9 + sizeof(raw);
    uint64_t deadline = wall_now() + 5000;
    int suffix_seen = 0, order = 1;
    while (f.written < 3 + sizeof(raw) && !s.error && wall_now() < deadline) {
        size_t retained = s.usb_length, offset = s.usb_offset;
        unsigned reads = f.reads;
        drive(&s);
        if (retained && offset < retained && f.reads != reads) order = 0;
        suffix_seen |= s.usb_length > s.usb_offset;
    }
    CHECK(!s.error && f.written == 3 + sizeof(raw) && order);
    CHECK(chunk <= capacity || suffix_seen);
    CHECK(!memcmp(f.output, "RDY", 3) && !memcmp(f.output + 3, raw, sizeof(raw)));
    CHECK(s.stream.rx.accepted == sizeof(raw) && s.stream.rx.delivered == sizeof(raw));
    CHECK(s.stream.tx.accepted == sizeof(raw) + 3 && s.stream.tx.delivered == sizeof(raw) + 3);
    /* The diagnostic copy sees exactly the accepted bytes: never the CONNECT text. */
    CHECK(s.negotiation.summary.bytes[PPP_NEGO_PEER] == s.stream.rx.accepted);
    CHECK(s.negotiation.summary.bytes[PPP_NEGO_LOCAL] == s.stream.tx.accepted);
    CHECK(ppp_supervisor_stop(&s) == 0);
    uint64_t drain_deadline = s.deadline_ms;
    unsigned signals = s.child.signals;
    CHECK(ppp_supervisor_stop(&s) == 0 && s.deadline_ms == drain_deadline && s.child.signals == signals);
    CHECK(ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    CHECK(s.error == PPP_ERROR_NONE && !s.stream.recovery_unknown);
    char diagnostic[1024];
    CHECK(ppp_supervisor_diagnostic(&s, diagnostic, sizeof(diagnostic)) > 0);
    CHECK(strstr(diagnostic, "recovery=unproven") && strstr(diagnostic, "cancellation=0") &&
          strstr(diagnostic, "core-error=none") && strstr(diagnostic, "descendants=unproven") &&
          !strstr(diagnostic, "RDY"));
}
static size_t hdlc_frame(unsigned char *out, const unsigned char *data, size_t length) {
    unsigned char body[32];
    memcpy(body, data, length);
    uint16_t fcs = (uint16_t)~ppp_auth_fcs(0xffff, data, length);
    body[length] = (unsigned char)(fcs & 0xff); body[length + 1] = (unsigned char)(fcs >> 8);
    size_t n = 0;
    out[n++] = 0x7e;
    for (size_t i = 0; i < length + 2; i++) {
        if (body[i] < 0x20 || body[i] == 0x7d || body[i] == 0x7e) {
            out[n++] = 0x7d; out[n++] = body[i] ^ 0x20;
        } else out[n++] = body[i];
    }
    out[n++] = 0x7e;
    return n;
}
/* The echo fixture returns every peer frame, so each direction sees the same
 * LCP Configure-Ack and IPCP Configure-Nak. The relay output stays byte-exact. */
static void negotiation_flow_test(const char *path) {
    static const unsigned char ack[] = {0xff, 0x03, 0xc0, 0x21, 0x02, 0x01, 0x00, 0x04};
    static const unsigned char nak[] = {0xff, 0x03, 0x80, 0x21, 0x03, 0x01, 0x00, 0x04};
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "echo", 64);
    ready(&s);
    unsigned char frames[96];
    size_t length = hdlc_frame(frames, ack, sizeof(ack));
    length += hdlc_frame(frames + length, nak, sizeof(nak));
    memcpy(f.input, "CONNECT\r\n", 9);
    memcpy(f.input + 9, frames, length); f.length = 9 + length;
    f.chunk = 5;
    uint64_t deadline = wall_now() + 5000;
    while (f.written < 3 + length && !s.error && wall_now() < deadline) drive(&s);
    CHECK(!s.error && f.written == 3 + length && !memcmp(f.output + 3, frames, length));
    const struct ppp_nego_summary *n = &s.negotiation.summary;
    CHECK(n->bytes[PPP_NEGO_PEER] == length && n->bytes[PPP_NEGO_LOCAL] == 3 + length);
    CHECK(n->frames[PPP_NEGO_PEER] == 2 && n->frames[PPP_NEGO_LOCAL] == 2);
    CHECK(!n->fcs_errors[PPP_NEGO_PEER] && !n->fcs_errors[PPP_NEGO_LOCAL]);
    CHECK(n->codes[PPP_NEGO_PEER][PPP_NEGO_LCP][2] == 1 && n->codes[PPP_NEGO_LOCAL][PPP_NEGO_LCP][2] == 1);
    CHECK(n->lcp == PPP_NEGO_LCP_OPENED && n->ipcp == PPP_NEGO_NCP_NAK);
    CHECK(n->auth == PPP_NEGO_AUTH_UNKNOWN && n->auth_result == PPP_NEGO_RESULT_NONE && !n->terminate);
    CHECK(ppp_supervisor_stop(&s) == 0);
    CHECK(ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    CHECK(s.error == PPP_ERROR_NONE);
}
static void gate_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "echo", 3);
    memcpy(f.input, "CONNECT\r\n", 9); f.length = 9;
    ready(&s);
    for (unsigned i = 0; i < 8; i++) {
        drive(&s);
        if (s.connected) break;
    }
    CHECK(s.connected && f.offset == 9 && f.written <= 3);
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    start(&s, &f, path, "echo", 3);
    uint64_t deadline = wall_now() + 3000;
    while (s.pending_tx.length < 3 && wall_now() < deadline) drive(&s);
    for (unsigned i = 0; i < 8; i++) drive(&s);
    CHECK(s.pending_tx.length == 3 && !f.reads && !f.writes && s.phase == PPP_START);
    f.now = s.deadline_ms;
    drive(&s);
    closed(&s, &f);
    CHECK(s.error == PPP_ERROR_TIMEOUT);
}
static void usb_failures_test(const char *path) {
    const int codes[] = {OFFLINE_IO_INTR, OFFLINE_IO_EOF, OFFLINE_IO_ERROR, 99};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        struct ppp_supervisor s;
        struct fake f;
        start(&s, &f, path, "echo", 8); ready(&s);
        f.read_failure = codes[i];
        drive(&s);
        unsigned reads = f.reads;
        closed(&s, &f);
        CHECK(s.error == PPP_ERROR_USB && f.poisoned && f.reads == reads && !s.stream.rx.accepted);
    }
    const int writes[] = {OFFLINE_IO_INTR, OFFLINE_IO_AGAIN, OFFLINE_IO_EOF, OFFLINE_IO_ERROR};
    for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]) + 1; i++) {
        struct ppp_supervisor s;
        struct fake f;
        start(&s, &f, path, "echo", 8); ready(&s);
        if (i == sizeof(writes) / sizeof(writes[0])) f.partial = 1;
        else f.write_failure = writes[i];
        memcpy(f.input, "CONNECT\r\n", 9); f.length = 9;
        drive(&s);
        CHECK(s.connected && s.stream.ready);
        unsigned count = f.writes;
        closed(&s, &f);
        CHECK(s.error == PPP_ERROR_USB && count == 1 && f.writes == count && !s.stream.tx.delivered);
    }
}
static void lifecycle_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "exchange", 8); ready(&s); connect_peer(&s, &f);
    CHECK(ppp_supervisor_stop(&s) == 0);
    /* Still forward opaque raw exchange after cancellation; no application source. */
    CHECK(ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    const unsigned char expected[] = {'R', 'D', 'Y', 0x7e, 0, 0xff};
    CHECK(!s.error && f.written == sizeof(expected) && !memcmp(f.output, expected, sizeof(expected)));
    start(&s, &f, path, "afterinput7", 8); ready(&s);
    memcpy(f.input, "CONNECT\r\nZ", 10); f.length = 10;
    uint64_t failure_deadline = wall_now() + 3000;
    while (s.phase != PPP_DRAIN && !s.error && wall_now() < failure_deadline) drive(&s);
    CHECK(s.phase == PPP_DRAIN && ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    CHECK(s.error == PPP_ERROR_CHILD && f.written == 4 && !memcmp(f.output, "RDYZ", 4));
    start(&s, &f, path, "nooutput", 8); ready(&s);
    memcpy(f.input, "CONNECT\r\n", 9); f.length = 9; drive(&s);
    CHECK(s.connected && s.stream.ready);
    failure_deadline = wall_now() + 3000;
    while (s.phase != PPP_DRAIN && !s.error && wall_now() < failure_deadline) drive(&s);
    CHECK(s.phase == PPP_DRAIN && s.stdout_eof && !s.peer_done);
    /* EOF is not peer completion; lack of that event must expire. */
    f.now = s.deadline_ms; drive(&s); closed(&s, &f);
    CHECK(s.error == PPP_ERROR_TIMEOUT);
    start(&s, &f, path, "rxaftereof", 8); ready(&s);
    memcpy(f.input, "CONNECT\r\n", 9); f.length = 9; drive(&s);
    failure_deadline = wall_now() + 3000;
    while (s.phase != PPP_DRAIN && !s.error && wall_now() < failure_deadline) drive(&s);
    CHECK(s.phase == PPP_DRAIN && s.stdout_eof && !s.peer_done);
    f.input[f.length++] = 'Z';
    drive(&s);
    CHECK(s.stream.rx.delivered == 1 && !s.error);
    CHECK(ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f); CHECK(!s.error);
    start(&s, &f, path, "diagnostic", 8); ready(&s); connect_peer(&s, &f);
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    CHECK(!s.error && s.child.stderr_bytes == 131072);
    start(&s, &f, path, "exit0", 8);
    /* Exit before explicit readiness is not ready, and cannot hide stdout. */
    force_cleanup(&s, &f);
    CHECK(s.error != PPP_ERROR_NONE);
    start(&s, &f, path, "ignore", 8); ready(&s); connect_peer(&s, &f);
    CHECK(ppp_supervisor_stop(&s) == 0);
    f.now = s.deadline_ms; drive(&s); closed(&s, &f);
    CHECK(s.error == PPP_ERROR_TIMEOUT && s.kill_sent);
    start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
    f.close_failure = 1;
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f); CHECK(s.error == PPP_ERROR_CLOSE);
    start(&s, &f, path, "noinput", 8); ready(&s);
    memcpy(f.input, "CONNECT\r\nprivate-child-marker", 29); f.length = 29;
    drive(&s); force_cleanup(&s, &f);
    CHECK(s.error != PPP_ERROR_NONE && !s.stream.rx.delivered);
    char diagnostic[512];
    CHECK(ppp_supervisor_diagnostic(&s, diagnostic, sizeof(diagnostic)) > 0 &&
          !strstr(diagnostic, "private-child-marker"));
}
static void deadlines_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "echo", 8); ready(&s); drive(&s);
    CHECK(s.phase == PPP_CONNECT && s.deadline_ms == 200);
    f.now = 200; drive(&s); closed(&s, &f); CHECK(s.error == PPP_ERROR_TIMEOUT);
    start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
    f.now = s.deadline_ms; drive(&s); closed(&s, &f); CHECK(s.error == PPP_ERROR_TIMEOUT);
    start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
    f.now = 10; drive(&s); f.now = 9; drive(&s); closed(&s, &f); CHECK(s.error == PPP_ERROR_CLOCK);
    start(&s, &f, path, "echo", 8); ready(&s);
    memcpy(f.input, "CONNECT\r\n", 9); f.length = 9; f.delay = 201;
    drive(&s); closed(&s, &f);
    CHECK(s.error == PPP_ERROR_TIMEOUT && !f.writes && !s.connected);
    start(&s, &f, path, "echo", 8); ready(&s); drive(&s);
    f.delay = 201; drive(&s); closed(&s, &f);
    CHECK(s.error == PPP_ERROR_TIMEOUT && !f.writes); /* AGAIN callback also expires. */
    start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
    f.now = UINT64_MAX; drive(&s); closed(&s, &f); CHECK(s.error == PPP_ERROR_CLOCK);
}
static void boundary_test(const char *path) {
    for (unsigned i = 0; i < 4; i++) {
        struct ppp_supervisor s;
        struct fake f;
        start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
        const unsigned char byte = 'Z';
        /* White-box invariant setup via queue/child APIs, not a caller example.
         * Keep real echoed stdout unread while the core clock boundary fails. */
        struct offline_io result = ppp_child_write(&s.child, &byte, 1);
        CHECK(result.code == OFFLINE_IO_OK && result.count == 1);
        int observed = fcntl(s.child.output_fd, F_DUPFD_CLOEXEC, 3), before = 0, after = 0;
        CHECK(observed >= 0);
        uint64_t deadline = wall_now() + 3000;
        while (!before && wall_now() < deadline) {
            struct pollfd fd = {observed, POLLIN, 0};
            (void)poll(&fd, 1, 1);
            if (ioctl(observed, FIONREAD, &before)) break;
        }
        CHECK(before == 1);
        if (i != 3) CHECK(offline_queue_enqueue(&s.pending_tx, &byte, 1) == OFFLINE_OK);
        CHECK(ppp_supervisor_stop(&s) == 0 && s.deadline_ms == 400);
        CHECK(ppp_supervisor_peer_done(&s) == 0);
        uint64_t accepted = s.pending_tx.accepted, delivered = s.pending_tx.delivered;
        unsigned reads = f.reads, writes = f.writes;
        f.clock_values[0] = i == 1 ? 10 : 399;
        f.clock_values[1] = i == 1 ? 9 : i == 2 ? UINT64_MAX : 400;
        f.clock_length = 2;
        CHECK(ppp_supervisor_step(&s) >= 0);
        CHECK(s.error == (i == 1 || i == 2 ? PPP_ERROR_CLOCK : PPP_ERROR_TIMEOUT));
        CHECK(s.stream.recovery_unknown && s.phase >= PPP_CLEANUP);
        CHECK(f.clock_offset == 2 && f.reads == reads && f.writes == writes);
        CHECK(s.pending_tx.delivered == delivered && s.pending_tx.length == 1);
        CHECK(s.pending_tx.accepted == accepted + (i == 3 ? 1 : 0));
        CHECK(!ioctl(observed, FIONREAD, &after) && after == 0);
        CHECK(s.child.stdout_discarded == (i == 3 ? 0u : 1u));
        CHECK(close(observed) == 0);
        f.clock_length = 0; f.now = 400;
        closed(&s, &f);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct ppp_supervisor s;
        struct fake f;
        start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
        unsigned signals = s.child.signals, reads = f.reads, writes = f.writes;
        f.clock_values[0] = 10;
        f.clock_values[1] = i ? UINT64_MAX : 9;
        f.clock_values[2] = 10;
        f.clock_length = 3;
        CHECK(ppp_supervisor_stop(&s) == -1 && s.error == PPP_ERROR_CLOCK);
        CHECK(s.child.signals == signals && f.reads == reads && f.writes == writes);
        CHECK(s.last_ms == 10 && f.clock_offset == 2);
        closed(&s, &f);
        CHECK(f.reads == reads && f.writes == writes);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct ppp_supervisor s;
        struct fake f = {.now = 1};
        struct ppp_peer peer = {&f, peer_read, peer_write, peer_now, peer_close, peer_release, NULL, NULL, NULL};
        struct ppp_budgets budgets = {1, i ? 200 : UINT64_MAX, i ? UINT64_MAX : 300, 400};
        char *args[] = {(char *)path, "echo", NULL}, *env[] = {"PPP_FIXTURE=explicit", NULL};
        CHECK(ppp_supervisor_start(&s, &peer, &budgets, 8, path, args, env, "/") == 0);
        ready(&s);
        memcpy(f.input, "CONNECT\r\n", 9); f.length = 9;
        CHECK(ppp_supervisor_step(&s) >= 0 && s.error == PPP_ERROR_CLOCK);
        CHECK(f.reads == i && !f.writes && !s.stream.ready);
        closed(&s, &f);
    }
}
static int quiesce_error(void *context) { (void)context; return -1; }
static struct ppp_quiet quiet_error(void *context) { (void)context; return (struct ppp_quiet){1, 1}; }
static void hooks_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f = {0};
    struct ppp_peer peer = {&f, peer_read, peer_write, peer_now, peer_close, peer_release, quiesce_error, NULL, NULL};
    struct ppp_budgets budgets = {100, 200, 300, 400};
    char *args[] = {(char *)path, "echo", NULL}, *env[] = {"PPP_FIXTURE=explicit", NULL};
    CHECK(ppp_supervisor_start(&s, &peer, &budgets, 8, path, args, env, "/") == -1);
    peer.quiesce = NULL; peer.poll_quiet = quiet_error;
    CHECK(ppp_supervisor_start(&s, &peer, &budgets, 8, path, args, env, "/") == -1);
    CHECK(!f.closes && !f.releases);
    peer.quiesce = quiesce_error;
    CHECK(ppp_supervisor_start(&s, &peer, &budgets, 8, path, args, env, "/") == 0);
    ready(&s); connect_peer(&s, &f);
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
    CHECK(s.error == PPP_ERROR_USB && s.peer_quiet && s.quiesce_started && s.stream.recovery_unknown);
}
static void exec_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, "/nonexistent-ppp-child-fixture", "echo", 8);
    uint64_t deadline = wall_now() + 3000;
    while (!s.error && wall_now() < deadline) drive(&s);
    closed(&s, &f); CHECK(s.error == PPP_ERROR_CHILD && s.child.exec_error == ENOENT);
    start(&s, &f, path, "echo", 8);
    CHECK(ppp_supervisor_stop(&s) == 0);
    closed(&s, &f);
    CHECK(!s.error && s.cancel_requested && s.cancel_core_expected && s.stream.recovery_unknown);
}
static void observed_start(struct ppp_supervisor *s, struct fake *f, const char *path,
                           char *mode, size_t capacity) {
    *f = (struct fake){.supervisor = s};
    struct ppp_peer peer = {f, peer_read, peer_write, peer_now, peer_close, peer_release, NULL, NULL, peer_begin};
    struct ppp_budgets budgets = {100, 200, 300, 400};
    char *args[] = {(char *)path, mode, NULL}, *env[] = {"PPP_FIXTURE=explicit", NULL};
    CHECK(ppp_supervisor_start_observed(s, &peer, &budgets, capacity, path, args, env, "/") == 0);
    CHECK(ppp_supervisor_ready(s) == -1 && !s->ready);
}
static void observed_test(const char *path) {
    const unsigned char first[] = {0, 0xff, 0x7e, 0x7d, 3, 1};
    for (unsigned split = 0; split < 2; split++) {
        struct ppp_supervisor s;
        struct fake f;
        observed_start(&s, &f, path, split ? "binarysplit" : "binary", split ? 1 : 3);
        memcpy(f.input, "CONNECT\r\n", 9);
        unsigned char raw[64]; for (unsigned i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)i;
        memcpy(f.input + 9, raw, sizeof(raw)); f.length = 9 + sizeof(raw);
        uint64_t deadline = wall_now() + 3000;
        while (f.written < sizeof(first) + sizeof(raw) && !s.error && wall_now() < deadline) drive(&s);
        CHECK(!s.error && f.begins == 1 && s.connected && s.output_observed);
        CHECK(f.written == sizeof(first) + sizeof(raw) && !memcmp(f.output, first, sizeof(first)) &&
              !memcmp(f.output + sizeof(first), raw, sizeof(raw)));
        CHECK(ppp_supervisor_ready(&s) == -1 && s.stream.rx.delivered == sizeof(raw));
        CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
        closed(&s, &f); CHECK(!s.error && f.begins == 1);
    }
    for (unsigned i = 0; i < 6; i++) {
        struct ppp_supervisor s;
        struct fake f;
        observed_start(&s, &f, i == 2 ? "/nonexistent-ppp-child-fixture" : path,
                       i == 0 ? "silent" : i == 1 ? "stderronly" : "binary", 8);
        if (i == 3) f.begin_failure = -1;
        if (i == 4) f.begin_delay = 200;
        if (i == 5) f.begin_delay = UINT64_MAX;
        uint64_t deadline = wall_now() + 3000;
        if (i < 2) {
            /* Exec and stderr never make stdout readiness. */
            while ((!s.child.exec_done || (i == 1 && !s.child.stderr_bytes)) && wall_now() < deadline) drive(&s);
            CHECK(!s.ready && !s.output_observed && !f.begins && !f.reads && !f.writes);
            f.now = s.deadline_ms;
        }
        while (!s.error && wall_now() < deadline) drive(&s);
        closed(&s, &f);
        CHECK(f.begins == (i >= 3 ? 1u : 0u) && !f.reads && !f.writes && !s.stream.rx.accepted);
        CHECK(s.error == (i == 2 ? PPP_ERROR_CHILD : i == 3 ? PPP_ERROR_USB :
                         i == 5 ? PPP_ERROR_CLOCK : PPP_ERROR_TIMEOUT));
    }
    /* Stored first output must not authorize a known dead/EOF child. Use child
     * APIs to isolate the boundary without racing the actual fixture exit. */
    for (unsigned i = 0; i < 2; i++) {
        struct ppp_supervisor s;
        struct fake f;
        observed_start(&s, &f, path, i ? "binaryeof" : "binaryexit", 8);
        uint64_t deadline = wall_now() + 3000;
        unsigned char bytes[8]; struct offline_io r = {OFFLINE_IO_AGAIN, 0};
        while (r.code == OFFLINE_IO_AGAIN && wall_now() < deadline) {
            (void)ppp_child_poll(&s.child);
            r = ppp_child_read(&s.child, bytes, sizeof(bytes));
            struct pollfd fd = {s.child.output_fd, POLLIN, 0}; (void)poll(&fd, 1, 1);
        }
        CHECK(r.code == OFFLINE_IO_OK && r.count == sizeof(first));
        CHECK(offline_queue_enqueue(&s.pending_tx, bytes, r.count) == OFFLINE_OK);
        s.output_observed = 1;
        if (i) {
            do {
                r = ppp_child_read(&s.child, bytes, sizeof(bytes));
                struct pollfd fd = {s.child.output_fd, POLLIN, 0}; (void)poll(&fd, 1, 1);
            } while (r.code == OFFLINE_IO_AGAIN && wall_now() < deadline);
            CHECK(r.code == OFFLINE_IO_EOF); s.stdout_eof = 1;
        } else {
            while (!s.child.reaped && wall_now() < deadline) {
                (void)ppp_child_poll(&s.child);
                struct pollfd fd = {s.child.error_fd, POLLIN, 0}; (void)poll(&fd, 1, 1);
            }
            CHECK(s.child.reaped);
        }
        drive(&s); closed(&s, &f);
        CHECK(s.error == PPP_ERROR_CHILD && !f.begins && !f.reads && !f.writes);
        CHECK(s.pending_tx.length == sizeof(first));
    }
    /* A clock failure at the transition is checked before any control request. */
    for (unsigned i = 0; i < 3; i++) {
        struct ppp_supervisor s; struct fake f;
        observed_start(&s, &f, path, "binary", 8);
        uint64_t deadline = wall_now() + 3000;
        int available = 0;
        while ((!s.child.exec_done || !available) && wall_now() < deadline) {
            (void)ppp_child_poll(&s.child);
            CHECK(ioctl(s.child.output_fd, FIONREAD, &available) == 0);
            struct pollfd fd = {s.child.output_fd, POLLIN, 0}; (void)poll(&fd, 1, 1);
        }
        f.clock_values[0] = 1;
        f.clock_values[1] = i == 0 ? 100 : i == 1 ? 0 : UINT64_MAX;
        f.clock_length = 2;
        drive(&s); closed(&s, &f);
        CHECK(s.output_observed && s.pending_tx.length == sizeof(first) && !f.begins && !f.reads && !f.writes);
        CHECK(s.error == (i == 0 ? PPP_ERROR_TIMEOUT : PPP_ERROR_CLOCK));
    }
    struct ppp_supervisor s; struct fake f = {0};
    struct ppp_peer peer = {&f, peer_read, peer_write, peer_now, peer_close, peer_release, NULL, NULL, NULL};
    struct ppp_budgets budgets = {100, 200, 300, 400};
    char *args[] = {(char *)path, "binary", NULL}, *env[] = {NULL};
    CHECK(ppp_supervisor_start_observed(&s, &peer, &budgets, 8, path, args, env, "/") == -1);
    CHECK(!f.closes && !f.releases);
    /* Optional hook also works with explicit injected readiness. */
    peer.begin_connect = peer_begin; f.supervisor = &s;
    CHECK(ppp_supervisor_start(&s, &peer, &budgets, 8, path, args, env, "/") == 0);
    uint64_t deadline = wall_now() + 3000;
    while (!s.pending_tx.length && !s.error && wall_now() < deadline) drive(&s);
    CHECK(!f.begins && ppp_supervisor_ready(&s) == 0);
    connect_peer(&s, &f); CHECK(f.begins == 1);
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    closed(&s, &f);
}
static void cleanup_barriers_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "holder-stderr", 8); ready(&s);
    memcpy(f.input, "CONNECT\r\n", 9); f.length = 9;
    uint64_t connect_deadline = wall_now() + 3000;
    while (!s.connected && !s.error && wall_now() < connect_deadline) drive(&s);
    CHECK(s.connected && s.stream.ready);
    /* The main stays alive to waitpid its holder. Closing stdout alone must not
     * close the supervisor or certify stderr EOF. */
    uint64_t deadline = wall_now() + 3000;
    while (!s.stdout_eof && !s.error && wall_now() < deadline) drive(&s);
    CHECK(s.phase == PPP_DRAIN && s.stdout_eof && !s.child.stderr_eof);
    CHECK(ppp_supervisor_peer_done(&s) == 0);
    drive(&s);
    CHECK(s.phase == PPP_CLEANUP && !s.error && !s.stream.drain_done);
    CHECK(s.child.input_fd < 0 && f.quiesces == 1 && s.peer_quiet);
    uint64_t drain_deadline = s.deadline_ms;
    closed(&s, &f);
    CHECK(!s.error && s.child.stderr_eof && !s.child.transport_abandoned);
    CHECK(s.deadline_ms == drain_deadline && !s.error_cleanup_started);

    /* Normal cleanup has its original deadline, even while quiet is pending. */
    start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
    f.quiet_hold = 1;
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    deadline = wall_now() + 3000;
    while ((!s.child.reaped || s.phase != PPP_CLEANUP) && wall_now() < deadline) drive(&s);
    CHECK(s.phase == PPP_CLEANUP && !s.error && !s.stream.drain_done && !f.closes);
    drain_deadline = s.deadline_ms;
    f.now = drain_deadline;
    drive(&s);
    CHECK(s.error == PPP_ERROR_TIMEOUT && s.error_cleanup_started && !s.stream.drain_done);
    uint64_t transport_deadline = s.transport_deadline_ms;
    f.now = transport_deadline + 1;
    for (unsigned i = 0; i < 4; i++) drive(&s);
    CHECK(s.phase == PPP_CLEANUP && !s.peer_quiet && !f.closes && !f.releases);
    CHECK(s.transport_deadline_ms == transport_deadline && s.deadline_ms == drain_deadline);
    CHECK(f.quiesces == 1 && ppp_supervisor_stop(&s) == -1);
    f.quiet_hold = 0; closed(&s, &f);
    CHECK(s.error == PPP_ERROR_TIMEOUT && f.quiesces == 1);

    /* A failure first reported by poll_quiet after normal cleanup also gets
     * exactly one error entry; drain_complete must not have locked ERROR out. */
    start(&s, &f, path, "echo", 8); ready(&s); connect_peer(&s, &f);
    f.quiet_hold = 1;
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    deadline = wall_now() + 3000;
    while (s.phase != PPP_CLEANUP && wall_now() < deadline) drive(&s);
    CHECK(!s.error && !s.error_cleanup_started && !s.stream.drain_done);
    f.quiet_failure = 1;
    drive(&s);
    CHECK(s.error == PPP_ERROR_USB && s.error_cleanup_started && s.stream.recovery_unknown);
    transport_deadline = s.transport_deadline_ms;
    for (unsigned i = 0; i < 4; i++) drive(&s);
    CHECK(s.transport_deadline_ms == transport_deadline && f.quiesces == 1 && !f.closes);
    f.quiet_hold = 0; closed(&s, &f);
    CHECK(s.error == PPP_ERROR_USB && !s.stream.drain_done);
}
static void main_first_abandon_test(const char *path, int cancel) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "holder-exit", 8); ready(&s);
    if (!cancel) connect_peer(&s, &f);
    /* RDY is relayed only after the holder's internal H handshake proves stdio
     * ownership/watchdog. X exits only the main; holder waits for stdin EOF and
     * THEN holds pipes for 2000ms. Its independent watchdog is 8s. */
    const unsigned char exit_main = 'X';
    struct offline_io io = ppp_child_write(&s.child, &exit_main, 1);
    CHECK(io.code == OFFLINE_IO_OK && io.count == 1);
    uint64_t deadline = wall_now() + 1000;
    while (!s.child.reaped && wall_now() < deadline) {
        (void)ppp_child_poll_status(&s.child);
        (void)poll(NULL, 0, 1);
    }
    CHECK(s.child.reaped && !s.child.ownership_lost && s.child.reap_count == 1);
    CHECK(WIFEXITED(s.child.wait_status) && !WEXITSTATUS(s.child.wait_status));
    unsigned char bytes[32];
    CHECK(ppp_child_read(&s.child, bytes, sizeof(bytes)).code == OFFLINE_IO_AGAIN);
    if (!cancel) drive(&s);
    CHECK(!s.error && s.phase == (cancel ? PPP_START : PPP_SESSION) &&
          !s.child.stdout_eof && !s.child.stderr_eof);
    /* Separate read-fd ownership permits a later REAL EOF observation. It is
     * never used to turn abandonment into EOF in the transport object. */
    int observed_out = fcntl(s.child.output_fd, F_DUPFD_CLOEXEC, 3);
    int observed_err = fcntl(s.child.error_fd, F_DUPFD_CLOEXEC, 3);
    CHECK(observed_out >= 0 && observed_err >= 0);
    uint64_t eof_start = wall_now();
    if (cancel) {
        CHECK(ppp_supervisor_stop(&s) == 0 && !s.term_sent);
        drive(&s); /* Cancellation closes stdin but never synthesizes pipe EOF. */
        CHECK(s.phase == PPP_CANCEL && !s.error && !s.peer_done && !s.stream.drain_done);
    }
    f.now = s.deadline_ms;
    drive(&s); /* Error or cancellation closes stdin; neither is pipe EOF. */
    CHECK(s.error == PPP_ERROR_TIMEOUT && s.error_cleanup_started && s.phase == PPP_CLEANUP);
    uint64_t transport_deadline = s.transport_deadline_ms;
    f.now = transport_deadline - 1; drive(&s);
    CHECK(s.phase == PPP_CLEANUP && !s.child.closed && !s.child.transport_abandoned);
    CHECK(!s.child.stdout_eof && !s.child.stderr_eof && !s.stream.drain_done);
    f.now = transport_deadline; drive(&s);
    CHECK(wall_now() - eof_start < 2000); /* Watchdog/hold expiry cannot pass this case. */
    CHECK(s.phase == PPP_CLOSED && s.child.transport_abandoned && s.child.closed);
    CHECK(!s.child.stdout_eof && !s.child.stderr_eof && !s.kill_sent);
    CHECK(ppp_child_read(&s.child, bytes, sizeof(bytes)).code == OFFLINE_IO_ERROR);
    CHECK(f.quiesces == 1 && f.closes == 1 && f.releases == 1);
    CHECK(ppp_supervisor_step(&s) == 1 && s.transport_deadline_ms == transport_deadline);
    int out_eof = 0, err_eof = 0;
    deadline = eof_start + 5000; /* Below the 8s watchdog, above the 2s hold. */
    while ((!out_eof || !err_eof) && wall_now() < deadline) {
        ssize_t n;
        if (!out_eof && observed_out >= 0) {
            n = read(observed_out, bytes, sizeof(bytes));
            if (!n) out_eof = 1;
            else if (n > 0 || (errno != EAGAIN && errno != EINTR)) break;
        }
        if (!err_eof && observed_err >= 0) {
            n = read(observed_err, bytes, sizeof(bytes));
            if (!n) err_eof = 1;
            else if (n > 0 || (errno != EAGAIN && errno != EINTR)) break;
        }
        (void)poll(NULL, 0, 1);
    }
    CHECK(out_eof && err_eof && wall_now() - eof_start >= 2000);
    CHECK(!s.child.stdout_eof && !s.child.stderr_eof && s.child.transport_abandoned);
    if (observed_out >= 0) CHECK(close(observed_out) == 0);
    if (observed_err >= 0) CHECK(close(observed_err) == 0);
    ppp_child_end_input(&s.child); /* Every failure path closes control input. */
    if (s.phase != PPP_CLOSED) force_cleanup(&s, &f);
    /* We waited for the MAIN only. Holder wait status belongs to the system
     * reaper, and pipe EOF does not establish every descendant's death. */
}
static void shared_stdout_budget_test(const char *path, size_t capacity) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "burst", capacity);
    uint64_t deadline = wall_now() + 3000;
    int available = 0;
    /* Stage enough real stdout without stepping, so a normal read followed by
     * a failing peer read has bytes available for a second cleanup read. */
    while ((!s.child.exec_done || available < 8192) && wall_now() < deadline) {
        (void)ppp_child_poll_status(&s.child);
        CHECK(ioctl(s.child.output_fd, FIONREAD, &available) == 0);
        (void)poll(NULL, 0, 1);
    }
    CHECK(s.child.exec_done && available >= 8192);
    CHECK(ppp_supervisor_ready(&s) == 0);
    f.read_failure = OFFLINE_IO_ERROR;
    uint64_t accepted = s.pending_tx.accepted, discarded = s.child.stdout_discarded;
    CHECK(ppp_supervisor_step(&s) >= 0);
    uint64_t normal = s.pending_tx.accepted - accepted;
    uint64_t cleanup = s.child.stdout_discarded - discarded;
    CHECK(s.error == PPP_ERROR_USB && s.error_cleanup_started && f.reads == 1 && !f.writes);
    CHECK(normal == capacity && cleanup == PPP_SUPERVISOR_BUFFER - capacity);
    CHECK(normal + cleanup == PPP_SUPERVISOR_BUFFER && !s.stdout_remaining);
    CHECK(s.pending_tx.length == capacity && !s.pending_tx.delivered);
    closed(&s, &f);
    size_t remaining = s.stdout_remaining;
    CHECK(ppp_supervisor_step(&s) == 1 && s.stdout_remaining == remaining);
    CHECK(ppp_supervisor_step(NULL) == -1);
    struct ppp_supervisor invalid = {0};
    CHECK(ppp_supervisor_step(&invalid) == -1 && !invalid.stdout_remaining);
}
static void cleanup_budget_test(const char *path) {
    struct ppp_supervisor s;
    struct fake f;
    start(&s, &f, path, "holder-burst", 8); ready(&s);
    f.now = s.deadline_ms;
    uint64_t deadline = wall_now() + 3000;
    while (s.phase != PPP_CLOSED && wall_now() < deadline) {
        uint64_t out = s.child.stdout_discarded, err = s.child.stderr_bytes;
        drive(&s);
        CHECK(s.child.stdout_discarded - out <= 4096 && s.child.stderr_bytes - err <= 4096);
    }
    CHECK(s.phase == PPP_CLOSED && s.error == PPP_ERROR_TIMEOUT);
    CHECK(f.quiesces == 1 && f.closes == 1 && f.releases == 1);
}
static void preconnect_cancel_test(const char *path) {
    int baseline = fd_count();
    const char *const modes[] = {"echo", "term-default", "term-exit7", "term-other", "ignore"};
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
        for (unsigned connect = 0; connect < 2; connect++) {
            struct ppp_supervisor s; struct fake f;
            start(&s, &f, path, (char *)modes[mode], 8); ready(&s);
            if (connect) {
                memcpy(f.input, "CON", 3); f.length = 3; drive(&s);
                CHECK(s.phase == PPP_CONNECT && s.stream.control_bytes == 3 && !s.connected);
            }
            unsigned reads = f.reads, writes = f.writes, begins = f.begins;
            CHECK(ppp_supervisor_stop(&s) == 0 && s.phase == PPP_CANCEL && !s.error);
            CHECK(s.cancel_requested && s.cancel_core_expected && s.stream.recovery_unknown &&
                  s.stream.error == OFFLINE_ERROR_CONTROL && !s.stream.ready);
            CHECK(s.term_sent && s.child.term_signals == 1 && !s.child.kill_signals);
            uint64_t deadline = s.deadline_ms;
            CHECK(ppp_supervisor_stop(&s) == 0 && s.deadline_ms == deadline && s.child.term_signals == 1);
            CHECK(ppp_supervisor_ready(&s) == -1 && ppp_supervisor_peer_done(&s) == -1);
            if (mode == 4) {
                f.now = deadline - 1; drive(&s);
                CHECK(s.phase == PPP_CANCEL && !s.error && !s.kill_sent && !f.closes);
                f.now = deadline; drive(&s);
                CHECK(s.error == PPP_ERROR_TIMEOUT && s.kill_sent);
            }
            closed(&s, &f);
            CHECK(s.error == (mode == 2 || mode == 3 ? PPP_ERROR_CHILD :
                             mode == 4 ? PPP_ERROR_TIMEOUT : PPP_ERROR_NONE));
            CHECK(f.reads == reads && f.writes == writes && f.begins == begins);
            CHECK(!s.peer_done && !s.stream.drain_done && f.quiesces == 1);
            CHECK(s.child.term_signals == 1 && s.child.kill_signals <= 1);
            CHECK(!s.child.transport_abandoned && s.child.stdout_eof && s.child.stderr_eof);
            char diagnostic[1024];
            CHECK(ppp_supervisor_diagnostic(&s, diagnostic, sizeof(diagnostic)) > 0 &&
                  strstr(diagnostic, "cancellation=1") && strstr(diagnostic, "core-error=control") &&
                  strstr(diagnostic, "recovery=unknown") && !strstr(diagnostic, "RDY"));
        }
    }
    /* Observed-start cancellation must not call the one-shot begin hook. */
    {
        struct ppp_supervisor s; struct fake f;
        observed_start(&s, &f, path, "binary", 8);
        CHECK(ppp_supervisor_stop(&s) == 0);
        closed(&s, &f);
        CHECK(!s.error && !f.begins && !f.reads && !f.writes && !s.ready);
    }
    for (unsigned failure = 0; failure < 4; failure++) {
        struct ppp_supervisor s; struct fake f;
        start(&s, &f, path, "echo", 8); ready(&s);
        f.quiet_hold = 1;
        if (failure == 1) f.quiesce_failure = -1;
        if (failure == 2) f.quiet_failure = 1;
        if (failure == 3) f.close_failure = -1;
        CHECK(ppp_supervisor_stop(&s) == 0);
        uint64_t wall_deadline = wall_now() + 3000;
        while (!s.child.reaped && wall_now() < wall_deadline) drive(&s);
        CHECK(s.child.reaped && !s.peer_quiet && !f.closes && !f.releases && !s.child.closed);
        CHECK(f.quiesces == 1 && !s.stream.drain_done);
        if (!failure) {
            f.now = s.deadline_ms; drive(&s);
            CHECK(s.error == PPP_ERROR_TIMEOUT);
            f.now = s.transport_deadline_ms;
            for (unsigned i = 0; i < 3; i++) drive(&s);
            CHECK(!f.closes && !f.releases && !s.child.closed);
        }
        f.quiet_hold = 0; closed(&s, &f);
        CHECK(s.error == (failure == 0 ? PPP_ERROR_TIMEOUT : failure == 3 ? PPP_ERROR_CLOSE : PPP_ERROR_USB));
        CHECK(f.quiesces == 1 && !s.stream.drain_done);
    }
    for (unsigned clock = 0; clock < 2; clock++) {
        struct ppp_supervisor s; struct fake f;
        start(&s, &f, path, "echo", 8); ready(&s);
        f.quiet_hold = 1;
        CHECK(ppp_supervisor_stop(&s) == 0);
        f.now = clock ? UINT64_MAX : 1; drive(&s);
        if (!clock) { f.now = 0; drive(&s); }
        f.quiet_hold = 0; closed(&s, &f);
        CHECK(s.error == PPP_ERROR_CLOCK && s.child.term_signals == 1);
    }
    {
        struct ppp_supervisor overflow; struct fake f = {.now = UINT64_MAX - 200};
        struct ppp_peer peer = {&f, peer_read, peer_write, peer_now, peer_close, peer_release,
                                peer_quiesce, peer_quiet, NULL};
        struct ppp_budgets budgets = {100, 100, 100, 400};
        char *args[] = {(char *)path, "echo", NULL}, *env[] = {NULL};
        CHECK(ppp_supervisor_start(&overflow, &peer, &budgets, 8, path, args, env, "/") == 0);
        CHECK(ppp_supervisor_stop(&overflow) == -1 && overflow.error == PPP_ERROR_CLOCK);
        CHECK(!overflow.child.term_signals);
        closed(&overflow, &f);
    }
    /* A test-local atfork stop proves TERM cancellation precedes execve. */
    {
        struct ppp_supervisor s; struct fake f;
        pause_before_exec = 1;
        start(&s, &f, path, "echo", 8);
        pause_before_exec = 0;
        int status = 0; pid_t result;
        uint64_t deadline = wall_now() + 3000;
        for (;;) {
            result = waitpid(s.child.pid, &status, WUNTRACED | WNOHANG);
            if (result == s.child.pid || (result < 0 && errno != EINTR) || wall_now() >= deadline) break;
            (void)poll(NULL, 0, 1);
        }
        int stopped = result == s.child.pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP;
        CHECK(stopped);
        /* An unexpected exit may have been reaped above. Refresh ownership
         * before any signal, and resume an owned child even on failed setup. */
        (void)ppp_child_poll_status(&s.child);
        if (stopped && !s.child.reaped && !s.child.ownership_lost) {
            CHECK(!s.child.exec_done);
            int stop_result = ppp_supervisor_stop(&s);
            CHECK(stop_result == 0 && s.term_sent);
            (void)ppp_child_poll_status(&s.child);
            int continue_result = 0;
            if (!s.child.reaped && !s.child.ownership_lost) {
                continue_result = kill(s.child.pid, SIGCONT);
                CHECK(continue_result == 0);
            }
            if (stop_result || continue_result) force_cleanup(&s, &f);
            else closed(&s, &f);
            CHECK(!s.error && !s.child.exec_error && s.child.exec_done &&
                  WIFSIGNALED(s.child.wait_status) && WTERMSIG(s.child.wait_status) == SIGTERM);
            CHECK(!s.pending_tx.accepted && !f.reads && !f.writes && !f.begins);
        } else {
            if (!s.child.reaped && !s.child.ownership_lost) {
                (void)ppp_supervisor_stop(&s);
                (void)ppp_child_poll_status(&s.child);
                if (!s.child.reaped && !s.child.ownership_lost) CHECK(kill(s.child.pid, SIGCONT) == 0);
            }
            force_cleanup(&s, &f);
        }
        if (s.phase != PPP_CLOSED) exit(EXIT_FAILURE); /* Do not outlive the local peer context. */
    }
    /* Cancellation output discard shares the 4096-byte step budget. */
    {
        struct ppp_supervisor s; struct fake f;
        start(&s, &f, path, "burst", 8);
        int available = 0; uint64_t deadline = wall_now() + 3000;
        while (available < 8192 && wall_now() < deadline) {
            (void)ppp_child_poll_status(&s.child);
            CHECK(ioctl(s.child.output_fd, FIONREAD, &available) == 0);
            (void)poll(NULL, 0, 1);
        }
        CHECK(available >= 8192 && ppp_supervisor_stop(&s) == 0);
        do {
            uint64_t out = s.child.stdout_discarded, err = s.child.stderr_bytes;
            drive(&s);
            CHECK(s.child.stdout_discarded - out <= 4096 && s.child.stderr_bytes - err <= 4096);
        } while (s.phase != PPP_CLOSED && wall_now() < deadline);
        closed(&s, &f);
        CHECK(!s.error && !f.reads && !f.writes && !s.pending_tx.accepted && s.child.stdout_discarded);
    }
    /* waitpid ownership loss is an error, and never authorizes another signal. */
    struct ppp_supervisor s; struct fake f;
    start(&s, &f, path, "echo", 8); ready(&s);
    CHECK(ppp_supervisor_stop(&s) == 0);
    int status = 0; pid_t result;
    uint64_t wall_deadline = wall_now() + 3000;
    for (;;) {
        result = waitpid(s.child.pid, &status, WNOHANG);
        if (result == s.child.pid || (result < 0 && errno != EINTR) || wall_now() >= wall_deadline) break;
        (void)poll(NULL, 0, 1);
    }
    int externally_reaped = result == s.child.pid;
    CHECK(externally_reaped);
    if (externally_reaped) {
        wall_deadline = wall_now() + 3000;
        while (s.phase != PPP_CLOSED && wall_now() < wall_deadline) drive(&s);
        CHECK(s.phase == PPP_CLOSED && s.error == PPP_ERROR_CHILD && s.child.ownership_lost &&
              !s.child.reaped && !s.child.reap_count && s.child.term_signals == 1 && !s.child.kill_signals);
        CHECK(f.closes == 1 && f.releases == 1 && s.child.fd_closes == 4);
    } else {
        /* Only the supervisor may reap/kill after a failed external observation.
         * Its first status poll detects ECHILD before the timeout fallback. */
        (void)ppp_child_poll_status(&s.child);
        force_cleanup(&s, &f);
    }
    if (s.phase != PPP_CLOSED) exit(EXIT_FAILURE); /* Do not outlive the local peer context. */
    /* An externally delivered TERM lacks the supervisor's actual-send evidence. */
    start(&s, &f, path, "term-default", 8); ready(&s);
    CHECK(kill(s.child.pid, SIGTERM) == 0);
    wall_deadline = wall_now() + 3000;
    while (!s.child.reaped && wall_now() < wall_deadline) {
        (void)ppp_child_poll_status(&s.child); (void)poll(NULL, 0, 1);
    }
    CHECK(s.child.reaped && !s.child.term_signals);
    CHECK(ppp_supervisor_stop(&s) == 0 && !s.term_sent);
    closed(&s, &f); CHECK(s.error == PPP_ERROR_CHILD && !s.child.signals);
    /* A completed failed exec cannot become an expected local cancellation. */
    start(&s, &f, "/nonexistent-ppp-child-fixture", "echo", 8);
    wall_deadline = wall_now() + 3000;
    while (!s.child.exec_done && wall_now() < wall_deadline) {
        (void)ppp_child_poll_status(&s.child); (void)poll(NULL, 0, 1);
    }
    CHECK(s.child.exec_error == ENOENT);
    CHECK(ppp_supervisor_stop(&s) == 0);
    closed(&s, &f); CHECK(s.error == PPP_ERROR_CHILD && s.child.exec_error == ENOENT);
    CHECK(fd_count() == baseline);
}
static void fixed_start_test(void) {
    struct ppp_supervisor s = {0};
    struct fake f = {0};
    struct ppp_budgets budgets = {100, 200, 300, 400};

    /* No begin_connect: must reject without creating a child or taking ownership. */
    struct ppp_peer peer_no_begin = {&f, peer_read, peer_write, peer_now,
                                     peer_close, peer_release, NULL, NULL, NULL};
    CHECK(ppp_supervisor_start_fixed(&s, &peer_no_begin, &budgets, 8) == -1);
    CHECK(!s.child.started);
    CHECK(!f.closes && !f.releases);

    /* NULL peer: must reject. */
    CHECK(ppp_supervisor_start_fixed(&s, NULL, &budgets, 8) == -1);
    CHECK(!s.child.started);

    /* With begin_connect: verify the fixed policy is path /usr/sbin/pppd,
     * cwd /, envp {NULL}, argv as ppp_launch_prepare() specifies.
     * We call ppp_supervisor_start_fixed and check the prepared policy
     * independently — no exec of /usr/sbin/pppd. */
    struct ppp_launch_policy policy;
    CHECK(ppp_launch_prepare(&policy) == 0);
    CHECK(!strcmp(policy.path, "/usr/sbin/pppd") && !strcmp(policy.cwd, "/") && !policy.envp[0]);
    const char *const expected_argv[] = {
        "/usr/sbin/pppd", "notty", "nodetach", "nopersist", "noauth",
        "noipdefault", "nodefaultroute", "noipv6", "nolog", "refuse-pap", "refuse-chap",
        "refuse-mschap", "refuse-mschap-v2", "refuse-eap", "noacsp", NULL};
    for (unsigned i = 0; i < 15; i++) CHECK(!strcmp(policy.argv[i], expected_argv[i]));
    CHECK(!policy.argv[15] && sizeof(policy.argv) / sizeof(policy.argv[0]) == 16);

    /* ppp_supervisor_start_fixed has no path parameter: calling it with
     * begin_connect set and verifying it fails closed when begin_connect is NULL
     * proves the no-caller-argv contract (the function signature itself has none). */
    f = (struct fake){0};
    struct ppp_peer peer_with_begin = {&f, peer_read, peer_write, peer_now,
                                       peer_close, peer_release, NULL, NULL, peer_begin};
    f.supervisor = NULL; /* begin_connect won't be reached in these checks */
    CHECK(ppp_supervisor_start_fixed(&s, &peer_with_begin, &budgets, 0) == -1);
    CHECK(!s.child.started && !f.closes && !f.releases);
    CHECK(ppp_supervisor_start_fixed(&s, &peer_with_begin, NULL, 8) == -1);
    CHECK(!s.child.started && !f.closes && !f.releases);

    /* Connection-keeping start: the same fail-closed contract, never spawning here. */
    CHECK(ppp_supervisor_start_up(&s, &peer_no_begin, &budgets, 8) == -1);
    CHECK(ppp_supervisor_start_up(&s, NULL, &budgets, 8) == -1);
    CHECK(ppp_supervisor_start_up(&s, &peer_with_begin, &budgets, 0) == -1);
    CHECK(ppp_supervisor_start_up(&s, &peer_with_begin, NULL, 8) == -1);
    CHECK(ppp_supervisor_start_up(&s, &peer_with_begin, &budgets, PPP_SUPERVISOR_BUFFER + 1) == -1);
    CHECK(!s.child.started && !f.closes && !f.releases);
    struct ppp_budgets zero_session = {100, 200, 0, 400};
    CHECK(ppp_supervisor_start_up(&s, &peer_with_begin, &zero_session, 8) == -1);
    CHECK(!s.child.started && !f.closes && !f.releases);
}
int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') return 2;
    int baseline = fd_count();
    CHECK(pthread_atfork(NULL, NULL, child_before_exec) == 0);
    transfer_test(argv[1], 3, 4096); transfer_test(argv[1], 17, 31);
    transfer_test(argv[1], 127, 1); negotiation_flow_test(argv[1]);
    gate_test(argv[1]); usb_failures_test(argv[1]);
    lifecycle_test(argv[1]); deadlines_test(argv[1]); boundary_test(argv[1]); exec_test(argv[1]); hooks_test(argv[1]);
    observed_test(argv[1]); cleanup_barriers_test(argv[1]);
    main_first_abandon_test(argv[1], 0); main_first_abandon_test(argv[1], 1);
    cleanup_budget_test(argv[1]);
    shared_stdout_budget_test(argv[1], 1024); shared_stdout_budget_test(argv[1], 4096);
    fixed_start_test();
    preconnect_cancel_test(argv[1]);
    CHECK(fd_count() == baseline);
    printf("ppp_supervisor real child/fake USB checks=%u failures=%u (explicit fake readiness, no device/network/pppd)\n",
           checks, failures);
    return failures ? 1 : 0;
}
