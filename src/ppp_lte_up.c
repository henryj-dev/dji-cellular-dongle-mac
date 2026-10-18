#define _DARWIN_C_SOURCE
#include "discovery_internal.h"
#include "discovery.h"
#include "ppp_lte_link.h"
#include "ppp_lte_network.h"
#include "ppp_lte_service.h"
#include "ppp_runner.h"
#include "ppp_trial_preflight.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Connection-keeping mode. pppd owns the default route and DNS for the whole
 * Mac while it runs; this process only watches, stops it and removes the fixed
 * service ID's leftover State keys. No reconnect: any end stops and cleans up. */
#define UP_SESSION_MS (UINT64_C(30) * 24 * 60 * 60 * 1000)
#define UP_DRAIN_MS 10000u
#define UP_READ_TIMEOUT_MS 75000u
/* Must match the fixed lcp-echo-interval/lcp-echo-failure argv: pppd notices a
 * dead link (interval * (failure + 1)) well before the USB IN timeout fires,
 * so it ends on its own and removes its own keys first. */
#define UP_ECHO_INTERVAL_MS 10000u
#define UP_ECHO_FAILURE 3u
/* Read-ahead: the oldest queued IN request may be (slots-1) echo intervals
 * older than the last data on an idle link (see ppp_usb.h). */
_Static_assert(UP_ECHO_INTERVAL_MS * (UP_ECHO_FAILURE + 1) +
               UP_ECHO_INTERVAL_MS * (PPP_USB_IN_SLOTS_DEFAULT - 1) < UP_READ_TIMEOUT_MS,
               "LCP echo failure must be detected before the oldest USB IN request times out");
/* Stop through TERM well before the supervisor's own session deadline KILL. */
#define UP_CEILING_MS (UP_SESSION_MS - 600000)
#define UP_ATTRIBUTION_MS 30000u
#define UP_ADDRESS_MS 60000u
#define UP_PRIMARY_MS 10000u
#define UP_LOST_MS 120000u
#define UP_SAMPLE_MS 250u
#define UP_CONNECTED_SAMPLE_MS 1000u
#define UP_RESTORE_POLLS 20u
#define UP_BUSY_SLICES 100u

enum up_mode { UP_HELP, UP_RUN, UP_CLEANUP };
struct up_context {
    struct trial_preflight preflight;
    struct ppp_trial_observer observer;
    struct ppp_runner *runner;
    struct ppp_runner_status status;
    struct ppp_session_observation child;
    struct ppp_trial_interface interface;
    struct ppp_lte_link link;
    struct ppp_lte_service_cleanup pre, reap, post;
    struct ppp_lte_primary primary;
    SCDynamicStoreRef store;
    FILE *output;
    volatile sig_atomic_t stop;
    uint64_t start_ms, last_ms, last_sample_ms, stable_seen_ms, connected_ms, moved_bytes;
    unsigned busy_slices;
    uint64_t loop_slices;
    unsigned seen_losses, seen_restores;
    int captured, start_valid, link_started, connected, user_stop, clock_error, release_error;
    int final_observed, reap_cleaned, output_error;
    const char *reason;
};
static volatile sig_atomic_t up_hangup;
/* One session or cleanup at a time across processes (manual runs and the
 * daemon's children). Held until the run returns; never inherited by pppd. */
#define UP_LOCK_PATH "/var/run/dji-cellular-dongle-mac.lock"
static int up_lock_fd = -1;
static const char *up_lock(void) {
    int fd = open(UP_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return "lock-error";
    /* Only a plain root-owned single-link file; a planted FIFO or link is refused. */
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1) {
        (void)close(fd); return "lock-error";
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int busy = errno == EWOULDBLOCK;
        (void)close(fd);
        return busy ? "lock-held" : "lock-error";
    }
    up_lock_fd = fd;
    return NULL;
}
static void up_on_hangup(int number) { (void)number; up_hangup = 1; }

static const char *up_name(const char *const *names, size_t count, unsigned value) {
    return value < count ? names[value] : "unknown";
}
#define UP_NAME(names, value) up_name(names, sizeof(names) / sizeof(names[0]), (unsigned)(value))
static const char *const up_supervisor_errors[] = {"none", "child", "usb", "core", "timeout",
    "clock", "close"};
static const char *const up_session_errors[] = {"none", "metadata", "acquire", "bind", "start",
    "supervisor", "dispose"};
static const char *const up_usb_errors[] = {"none", "timeout", "aborted", "no-device",
    "not-open", "stall", "overrun", "zero-length", "oversize", "other"};
static const char *const up_usb_sites[] = {"none", "submit", "completion", "write", "connect",
    "abort", "validate"};
static const char *const up_lcp[] = {"none", "configuring", "opened"};
static const char *const up_ncp[] = {"none", "configuring", "nak", "rejected", "opened"};
static const char *const up_terminate[] = {"none", "local", "peer", "both"};

static int up_parse(int argc, char **argv, enum up_mode *mode) {
    if (argc != 2) return -1;
    if (!strcmp(argv[1], "--help")) *mode = UP_HELP;
    else if (!strcmp(argv[1], "--up")) *mode = UP_RUN;
    else if (!strcmp(argv[1], "--cleanup")) *mode = UP_CLEANUP;
    else return -1;
    return 0;
}
static int up_now(uint64_t *milliseconds) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || (uint64_t)ts.tv_sec > (UINT64_MAX - 999) / 1000) return -1;
    *milliseconds = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return 0;
}
static void up_sleep(long milliseconds) {
    const struct timespec delay = {.tv_sec = milliseconds / 1000, .tv_nsec = (milliseconds % 1000) * 1000000L};
    (void)nanosleep(&delay, NULL);
}
static void up_stop(struct up_context *c, const char *reason) {
    if (!c->stop) c->reason = reason;
    c->stop = 1;
}
static void up_line(struct up_context *c, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void up_line(struct up_context *c, const char *format, ...) {
    if (c->output_error) return;
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(c->output, format, arguments);
    va_end(arguments);
    if (result < 0 || fflush(c->output)) { c->output_error = 1; up_stop(c, "output-error"); }
}
/* Child still owned, session healthy and the clock moving forward. Never
 * samples anything; a signal can land in any OS call, so callers recheck. */
static int up_live(struct up_context *c) {
    /* Pull pending signal flags first: a signal can land inside any CF/proc call. */
    if (ppp_runner_observe(c->runner, &c->status, &c->child)) return -1;
    if (c->stop || up_hangup || c->status.signal_number || c->status.stop_requested ||
        c->status.error || c->status.session.error || c->status.session.stop_requested ||
        !c->child.started || c->child.reaped || c->child.ownership_lost ||
        c->child.stop_requested || !c->captured || !c->observer.identity) return -1;
    uint64_t now = c->last_ms;
    if (up_now(&now) || now < c->last_ms) { c->clock_error = 1; return -1; }
    c->last_ms = now;
    return 0;
}
/* Why the session cannot go on; signal requests are checked by the caller first. */
static const char *up_end_reason(const struct up_context *c) {
    if (c->clock_error) return "clock-error";
    /* A USB failure kills pppd: name the cause, not the reap it led to. */
    if (c->status.session.error) {
        if (c->status.session.supervisor_valid && c->status.session.supervisor_error == PPP_ERROR_USB)
            return "usb-error";
        return "session-error";
    }
    if (c->status.error) return "runner-error";
    if (c->child.reaped || c->child.ownership_lost) return "pppd-exit";
    return "child-unproven";
}
/* The session cannot go on: a user request wins over any child/session cause. */
static void up_halt(struct up_context *c) {
    if (up_hangup || c->status.signal_number) { c->user_stop = 1; up_stop(c, "user-stop"); }
    else up_stop(c, up_end_reason(c));
}
static int up_sample(struct up_context *c, struct ppp_trial_interface *interface) {
    ppp_trial_observer_sample(&c->observer);
    if (up_live(c)) { up_halt(c); return -1; }
    return ppp_trial_observer_current(&c->observer, interface);
}
static void up_connect_check(struct up_context *c, uint64_t now, int state) {
    if (state != PPP_LTE_LINK_STABLE) {
        if (now - c->start_ms >= UP_ADDRESS_MS) up_stop(c, "address-unproven");
        return;
    }
    if (!c->stable_seen_ms) c->stable_seen_ms = now;
    (void)ppp_lte_service_primary(c->store, c->interface.name, &c->primary);
    if (c->primary.ipv4 == PPP_LTE_PRIMARY_THIS) {
        c->connected = 1; c->connected_ms = now;
        c->seen_losses = c->link.losses; c->seen_restores = c->link.restores;
        up_line(c, "up=connected elapsed-ms=%llu primary-ipv4=%s primary-ipv6=%s"
            " primary-interface=%s dns=%s ip-resets=%u\n",
            (unsigned long long)(now - c->start_ms), ppp_lte_primary_label(c->primary.ipv4),
            ppp_lte_primary_label(c->primary.ipv6),
            c->primary.interface < 0 ? "unproven" : c->primary.interface ? "this-interface" : "other",
            ppp_lte_dns_label(c->primary.dns), c->link.resets);
    } else if (now - c->stable_seen_ms >= UP_PRIMARY_MS) up_stop(c, "primary-not-taken");
}
/* Attribution first, then the address watch; at most one sample per period. */
static void up_watch(struct up_context *c, uint64_t now) {
    uint64_t period = c->connected ? UP_CONNECTED_SAMPLE_MS : UP_SAMPLE_MS;
    if (c->last_sample_ms && now - c->last_sample_ms < period) return;
    c->last_sample_ms = now;
    struct ppp_trial_interface interface;
    memset(&interface, 0, sizeof(interface));
    int current = up_sample(c, &interface);
    if (c->stop || c->clock_error) return;
    now = c->last_ms;
    if (!c->link_started) {
        if (!current) {
            /* Only the fixed service ID may be ours; anything else is not this run. */
            CFStringRef mine = ppp_lte_service_key(PPP_LTE_ENTITY_PPP);
            if (!c->observer.service || !mine || !CFEqual(c->observer.service, mine)) {
                up_stop(c, "attribution-mismatch"); return;
            }
            c->interface = interface;
            if (ppp_lte_link_init(&c->link, interface.name)) { up_stop(c, "link-init-error"); return; }
            c->link_started = 1;
        } else {
            if (!c->observer.identity || c->observer.ambiguous || c->observer.snapshot_error)
                up_stop(c, "attribution-lost");
            else if (now - c->start_ms >= UP_ATTRIBUTION_MS) up_stop(c, "interface-unproven");
            return;
        }
    } else if (current || strcmp(interface.name, c->interface.name) || interface.index != c->interface.index) {
        up_stop(c, "attribution-lost"); return;
    }
    int state = ppp_lte_link_step(&c->link, now);
    if (state < 0) { c->clock_error = 1; up_stop(c, "clock-error"); return; }
    if (!c->connected) { up_connect_check(c, now, state); return; }
    if (c->link.losses != c->seen_losses) {
        c->seen_losses = c->link.losses;
        up_line(c, "up=address-lost link=%s losses=%u\n", ppp_lte_link_label(c->link.state), c->link.losses);
    }
    if (c->link.restores != c->seen_restores) {
        c->seen_restores = c->link.restores;
        up_line(c, "up=address-restored restores=%u\n", c->link.restores);
    }
    if (c->link.lost_since_ms && now - c->link.lost_since_ms >= UP_LOST_MS) up_stop(c, "address-lost-timeout");
}
/* pppd is gone: its State keys must not keep a dead primary until CLOSED. */
static void up_reaped(struct up_context *c) {
    if (c->reap_cleaned || !c->child.reaped) return;
    c->reap_cleaned = 1;
    (void)ppp_lte_service_cleanup(c->store, &c->reap);
}
static int up_target(void *context, io_service_t service, IOUSBDeviceInterface **device,
                     uint64_t registry_id, const struct descriptor_config *config) {
    struct up_context *c = context;
    struct ppp_session *session = NULL;
    const uint32_t read_timeout = UP_READ_TIMEOUT_MS;
    struct ppp_session_request request = {.service = service, .device = device,
        .registry_id = registry_id, .config = config,
        .budgets = {5000, 10000, UP_SESSION_MS, UP_DRAIN_MS},
        .capacity = PPP_SUPERVISOR_BUFFER, .launch = PPP_LAUNCH_MODE_UP, .read_timeout_ms = &read_timeout};
    if (ppp_session_start(&request, &c->preflight.backend, &session)) {
        c->reason = "session-rejected"; return DJI_SYSTEM_ERROR;
    }
    if (ppp_runner_begin(&session, &c->runner)) {
        c->reason = "runner-rejected";
        (void)ppp_session_stop(session);
        while (ppp_session_step(session) != 1) up_sleep(10);
        (void)ppp_session_release(&session);
        return DJI_SYSTEM_ERROR;
    }
    uint32_t wait = 10;
    for (;;) {
        int slice = ppp_runner_slice(c->runner, &c->stop, &wait, &c->status);
        c->loop_slices++;
        if (slice < 0) { up_stop(c, "runner-call-error"); up_sleep(10); continue; }
        if (ppp_runner_observe(c->runner, &c->status, &c->child)) {
            up_stop(c, "observation-error");
            if (slice == 1) break;
            continue;
        }
        /* Busy traffic keeps the shortest wait for the next UP_BUSY_SLICES slices
         * (about 100 ms): one USB read takes two slices (submit, then collect),
         * so a 1 ms/10 ms alternation would idle the read path most of the time. */
        uint64_t moved = c->child.child_output_bytes + c->child.usb_input_bytes;
        if (moved != c->moved_bytes) c->busy_slices = UP_BUSY_SLICES;
        wait = c->busy_slices ? 1 : 10;
        if (c->busy_slices) c->busy_slices--;
        c->moved_bytes = moved;
        if (c->child.started && !c->captured) {
            c->captured = 1;
            if (c->child.reaped || c->child.ownership_lost) up_stop(c, "start-observation-error");
            else {
                ppp_trial_observer_child(&c->observer, c->child.pid);
                if (!c->observer.identity) up_stop(c, "identity-unproven");
                if (up_now(&c->start_ms) || c->start_ms > UINT64_MAX - UP_SESSION_MS) {
                    c->clock_error = 1; up_stop(c, "start-clock-error");
                } else c->start_valid = 1;
                c->last_ms = c->start_ms;
            }
        }
        /* User requests first, so a stop is never filed as a child failure. */
        if (up_hangup || c->status.signal_number) { c->user_stop = 1; up_stop(c, "user-stop"); }
        if (c->child.reaped) { up_stop(c, up_end_reason(c)); up_reaped(c); }
        if (slice == 1) break;
        if (c->stop) continue;
        if (up_live(c)) { up_halt(c); continue; }
        if (c->last_ms - c->start_ms >= UP_CEILING_MS) { up_stop(c, "session-ceiling"); continue; }
        up_watch(c, c->last_ms);
    }
    if (!c->reason) c->reason = "session-ended";
    /* No peer_done: remote LCP/PDP termination stays unproven. */
    while (ppp_runner_restore(c->runner, &c->status) != 0) up_sleep(10);
    c->final_observed = !ppp_runner_observe(c->runner, &c->status, &c->child);
    if (c->final_observed && c->child.reaped) up_reaped(c);
    if (ppp_runner_release(&c->runner)) { c->release_error = 1; return DJI_SYSTEM_ERROR; }
    return DJI_OK;
}
static int up_service_text(const struct ppp_lte_service_cleanup *r, int ran, char *text, size_t size) {
    if (!ran) return snprintf(text, size, "not-run");
    char before[96], after[96];
    if (ppp_lte_service_label(&r->before, before, sizeof(before)) < 0 ||
        ppp_lte_service_label(&r->after, after, sizeof(after)) < 0) return snprintf(text, size, "unproven");
    return snprintf(text, size, "%s:removed:%u:failed:%u:before:%s:after:%s",
        ppp_lte_service_status_label(r->status), r->removed, r->failed, before, after);
}
/* After the stop: primary leaves this service (Wi-Fi or nothing comes back). */
static const char *up_primary_restored(struct up_context *c) {
    for (unsigned i = 0; i < UP_RESTORE_POLLS; i++) {
        struct ppp_lte_primary p;
        if (!ppp_lte_service_primary(c->store, NULL, &p) &&
            p.ipv4 != PPP_LTE_PRIMARY_UNPROVEN && p.ipv4 != PPP_LTE_PRIMARY_THIS &&
            p.ipv6 != PPP_LTE_PRIMARY_UNPROVEN && p.ipv6 != PPP_LTE_PRIMARY_THIS) return "yes";
        up_sleep(250);
    }
    return "no";
}
static int up_cleanup_only(struct up_context *c, const struct trial_backend *b) {
    if (b->identity(b->context)) {
        up_line(c, "cleanup=blocked gate=root-identity\n");
        return c->output_error ? 1 : 3;
    }
    const char *locked = up_lock();
    if (locked) { up_line(c, "cleanup=blocked gate=%s\n", locked); return c->output_error ? 1 : 3; }
    c->store = SCDynamicStoreCreate(NULL, CFSTR("ppp_lte_up cleanup"), NULL, NULL);
    if (!c->store) { up_line(c, "cleanup=blocked gate=store-error\n"); return c->output_error ? 1 : 4; }
    (void)ppp_lte_service_cleanup(c->store, &c->post);
    CFRelease(c->store); c->store = NULL;
    char text[256];
    (void)up_service_text(&c->post, 1, text, sizeof(text));
    int clean = ppp_lte_service_clean(&c->post);
    up_line(c, "cleanup=%s sc=%s\n", clean ? "clean" : "unproven", text);
    return !clean ? 4 : c->output_error ? 1 : 0;
}
static int lte_up_run(int argc, char **argv, FILE *output, const struct trial_backend *backend);
/* Private fake-TU entry; NULL selects the strict shared trial gates. SIGPIPE
 * is ignored and SIGHUP latched from the first host step until the last line
 * is written, so a closed reader yields exit 1, never a silent death. */
static int lte_up(int argc, char **argv, FILE *output, const struct trial_backend *backend) {
    enum up_mode mode = UP_HELP;
    if (up_parse(argc, argv, &mode) || mode == UP_HELP) return lte_up_run(argc, argv, output, backend);
    struct sigaction hangup = {.sa_handler = up_on_hangup}, ignore = {.sa_handler = SIG_IGN};
    struct sigaction old_hangup, old_pipe;
    sigemptyset(&hangup.sa_mask); sigemptyset(&ignore.sa_mask);
    up_hangup = 0;
    int hangup_installed = !sigaction(SIGHUP, &hangup, &old_hangup);
    int pipe_installed = !sigaction(SIGPIPE, &ignore, &old_pipe);
    int result = lte_up_run(argc, argv, output, backend);
    if (up_lock_fd >= 0) { (void)close(up_lock_fd); up_lock_fd = -1; }
    if (pipe_installed) (void)sigaction(SIGPIPE, &old_pipe, NULL);
    if (hangup_installed) (void)sigaction(SIGHUP, &old_hangup, NULL);
    return result;
}
static int lte_up_run(int argc, char **argv, FILE *output, const struct trial_backend *backend) {
    struct up_context c = {.output = output};
    enum up_mode mode = UP_HELP;
    if (up_parse(argc, argv, &mode)) return 2;
    if (mode == UP_HELP) {
        return fputs("usage: ppp_lte_up --up | --cleanup | --help\n"
            "--up keeps one LTE PPP link with the Mac's default route and DNS until Ctrl+C;\n"
            "--cleanup removes leftover State keys of the fixed service ID only.\n"
            "Both need root and separate execution approval.\n", output) < 0 || fflush(output) ? 1 : 0;
    }
    const struct trial_backend defaults = {NULL, trial_identity, trial_metadata, trial_preflight};
    const struct trial_backend *b = backend ? backend : &defaults;
    if (!b->identity || !b->metadata || !b->preflight) return 2;
    if (mode == UP_CLEANUP) return up_cleanup_only(&c, b);
    const char *gate = "root-identity";
    if (b->identity(b->context)) goto blocked;
    gate = up_lock();
    if (gate) goto blocked;
    gate = trial_executable(b);
    if (strcmp(gate, "clear")) goto blocked;
    gate = "root-home-preflight";
    if (b->preflight(b->context, &c.preflight) ||
        c.preflight.status != TRIAL_PREFLIGHT_COMPLETE || !c.preflight.metadata.clear ||
        !c.preflight.backend.real_uid || !c.preflight.backend.home || !c.preflight.backend.metadata)
        goto blocked;
    gate = "store-error";
    c.store = SCDynamicStoreCreate(NULL, CFSTR("ppp_lte_up"), NULL, NULL);
    if (!c.store) goto blocked;
    /* Before the baseline: a leftover fixed key there would block attribution. */
    (void)ppp_lte_service_cleanup(c.store, &c.pre);
    gate = c.pre.status == PPP_LTE_CLEANUP_BLOCKED_PPPD ? "pppd-running" :
        c.pre.status == PPP_LTE_CLEANUP_BLOCKED_PEER ? "instance-running" :
        c.pre.status == PPP_LTE_CLEANUP_BLOCKED_PROCESSES ? "process-list" :
        c.pre.status != PPP_LTE_CLEANUP_DONE ? "store-error" :
        c.pre.after.unknown ? "service-unknown-keys" : c.pre.after.present ? "service-residue" : NULL;
    if (gate) goto blocked;
    /* A stop that arrived during the checks: never start a data session. */
    gate = up_hangup ? "user-stop" : NULL;
    if (gate) goto blocked;
    up_line(&c, "up=starting sc-pre-removed=%u\n", c.pre.removed);
    ppp_trial_observer_begin(&c.observer);
    int discovery = DJI_SYSTEM_ERROR;
    if (c.observer.snapshot_error) c.reason = "baseline-unproven";
    else if (!c.output_error) discovery = discovery_with_one_target(up_target, &c);
    if (!c.reason) c.reason = discovery == DJI_OK ? "target-not-run" : "discovery-error";
    if (c.child.reaped || !c.child.started) {
        /* Idempotent final cleanup; a live or unreaped pppd blocks it by itself. */
        (void)ppp_lte_service_cleanup(c.store, &c.post);
    }
    const char *restored = !ppp_lte_service_clean(&c.post) ? "unproven" : up_primary_restored(&c);
    ppp_trial_observer_finish(&c.observer);
    int observed = ppp_trial_observer_result(&c.observer);
    char diagnostic[1024];
    int length = ppp_trial_observer_diagnostic(&c.observer, diagnostic, sizeof(diagnostic));
    ppp_trial_observer_dispose(&c.observer);
    CFRelease(c.store); c.store = NULL;
    if (length < 0 || (size_t)length >= sizeof(diagnostic)) return 1;
    const struct ppp_session_observation *k = &c.child;
    int fresh = c.final_observed && c.status.session.supervisor_valid, child_valid = fresh && k->started;
    int clean = ppp_lte_service_clean(&c.post);
    int stopped = c.user_stop && !strcmp(c.reason, "user-stop") && clean && !strcmp(restored, "yes") &&
        child_valid && k->exited && k->exit_code == 5 && !k->kill_signals && !c.release_error &&
        !c.status.error && !c.status.unrestored_actions;
    /* Global IPv4/IPv6/DNS equality stays a diagnostic only: over a long session
     * Wi-Fi roaming or DHCP changes them for reasons unrelated to this run. */
    (void)observed;
    char exit_text[24] = "unproven", connected_ms[24] = "none", ready[24] = "none", stable[24] = "none";
    if (child_valid && k->exited) (void)snprintf(exit_text, sizeof(exit_text), "code:%d", k->exit_code);
    else if (child_valid && k->signaled) (void)snprintf(exit_text, sizeof(exit_text), "signal:%d", k->term_signal);
    if (c.connected) (void)snprintf(connected_ms, sizeof(connected_ms), "%llu", (unsigned long long)(c.connected_ms - c.start_ms));
    if (c.link_started && c.link.first_pin_ms >= c.start_ms)
        (void)snprintf(ready, sizeof(ready), "%llu", (unsigned long long)(c.link.first_pin_ms - c.start_ms));
    if (c.link_started && c.link.first_stable_ms >= c.start_ms)
        (void)snprintf(stable, sizeof(stable), "%llu", (unsigned long long)(c.link.first_stable_ms - c.start_ms));
    char pre[256], reap[256], post[256], leftover[96] = "not-run";
    (void)up_service_text(&c.pre, 1, pre, sizeof(pre));
    (void)up_service_text(&c.reap, c.reap_cleaned, reap, sizeof(reap));
    (void)up_service_text(&c.post, c.post.status != PPP_LTE_CLEANUP_NOT_RUN, post, sizeof(post));
    /* What pppd itself left after it was reaped, before anything was removed. */
    if (c.reap_cleaned && ppp_lte_service_label(&c.reap.before, leftover, sizeof(leftover)) < 0)
        (void)snprintf(leftover, sizeof(leftover), "unproven");
    const struct ppp_nego_summary *n = &k->negotiation;
    char frames_rx[24] = "unproven", frames_tx[24] = "unproven", echo[24] = "unproven", reply[24] = "unproven";
    char in_bytes[24] = "unproven", in_reads[24] = "unproven", empty[24] = "unproven";
    char validate_ms[24] = "unproven", write_ms[24] = "unproven", slices[24], discarded[24] = "unproven";
    (void)snprintf(slices, sizeof(slices), "%llu", (unsigned long long)c.loop_slices);
    if (fresh) {
        (void)snprintf(frames_rx, sizeof(frames_rx), "%llu", (unsigned long long)n->frames[PPP_NEGO_PEER]);
        (void)snprintf(frames_tx, sizeof(frames_tx), "%llu", (unsigned long long)n->frames[PPP_NEGO_LOCAL]);
        (void)snprintf(echo, sizeof(echo), "%lu/%lu", (unsigned long)n->codes[PPP_NEGO_PEER][PPP_NEGO_LCP][9],
            (unsigned long)n->codes[PPP_NEGO_LOCAL][PPP_NEGO_LCP][9]);
        (void)snprintf(reply, sizeof(reply), "%lu/%lu", (unsigned long)n->codes[PPP_NEGO_PEER][PPP_NEGO_LCP][10],
            (unsigned long)n->codes[PPP_NEGO_LOCAL][PPP_NEGO_LCP][10]);
        (void)snprintf(in_bytes, sizeof(in_bytes), "%llu", (unsigned long long)k->usb_input_bytes);
        (void)snprintf(in_reads, sizeof(in_reads), "%llu", (unsigned long long)k->usb_input_reads);
        (void)snprintf(empty, sizeof(empty), "%llu", (unsigned long long)k->usb_empty_polls);
        (void)snprintf(validate_ms, sizeof(validate_ms), "%llu", (unsigned long long)(k->usb_validate_us / 1000));
        (void)snprintf(write_ms, sizeof(write_ms), "%llu", (unsigned long long)(k->usb_write_us / 1000));
        (void)snprintf(discarded, sizeof(discarded), "%llu", (unsigned long long)k->usb_in_discarded);
    }
    char resets[16] = "none", losses[16] = "none", restores[16] = "none";
    if (c.link_started) {
        (void)snprintf(resets, sizeof(resets), "%u", c.link.resets);
        (void)snprintf(losses, sizeof(losses), "%u", c.link.losses);
        (void)snprintf(restores, sizeof(restores), "%u", c.link.restores);
    }
    const char *session_error = c.status.session.error == PPP_SESSION_ERROR_SUPERVISOR &&
        c.status.session.supervisor_valid && c.status.session.supervisor_error == PPP_ERROR_TIMEOUT ?
        "supervisor-timeout" : UP_NAME(up_session_errors, c.status.session.error);
    const char *cleanup = c.release_error ? "release-error" : c.status.error ? "runner-error" :
        c.status.session.cleanup_error ? "session-cleanup-error" : "none";
    int result = stopped ? 0 : clean ? 3 : 4;
    up_line(&c, "up-result=%s reason=%s connected=%s connected-ms=%s ip-ready-ms=%s ip-stable-ms=%s"
        " ip-resets=%s ip-losses=%s ip-restores=%s connect-primary-ipv4=%s connect-primary-ipv6=%s connect-dns=%s"
        " sc-pre=%s pppd-leftover=%s sc-reap=%s sc-post=%s primary-restored=%s"
        " local=%s session-error=%s cleanup=%s clock=%s child=%s child-exit=%s term-count=%u kill-count=%u"
        " callback-quiet=%s handlers=%s supervisor-error=%s usb-error=%s usb-error-site=%s"
        " ppp-lcp=%s ppp-ipcp=%s ppp-terminate=%s ppp-lcp-echo=%s ppp-lcp-echo-reply=%s ppp-frames-rx=%s ppp-frames-tx=%s usb-in-bytes=%s usb-in-reads=%s usb-in-empty-polls=%s usb-validate-ms=%s usb-write-ms=%s usb-in-discarded=%s loop-slices=%s"
        " remote-termination=unproven next-run-cleanup=%s %s\n",
        result == 0 ? "stopped" : result == 3 ? "ended" : "cleanup-required", c.reason,
        c.connected ? "yes" : "no", connected_ms, ready, stable, resets, losses, restores,
        c.connected ? ppp_lte_primary_label(c.primary.ipv4) : "unproven",
        c.connected ? ppp_lte_primary_label(c.primary.ipv6) : "unproven",
        c.connected ? ppp_lte_dns_label(c.primary.dns) : "unproven",
        pre, leftover, reap, post, restored,
        c.status.session.local_done ? "closed" : "not-started", session_error, cleanup,
        c.clock_error ? "error" : "ok",
        k->reaped ? "reaped" : k->ownership_lost ? "ownership-lost" : "unproven", exit_text,
        k->term_signals, k->kill_signals, k->peer_quiet ? "observed" : "unproven",
        c.status.unrestored_actions ? "unrestored" : "restored-or-not-installed",
        fresh ? UP_NAME(up_supervisor_errors, c.status.session.supervisor_error) : "unproven",
        fresh ? UP_NAME(up_usb_errors, k->usb_error_kind) : "unproven",
        fresh ? UP_NAME(up_usb_sites, k->usb_error_site) : "unproven",
        fresh ? UP_NAME(up_lcp, n->lcp) : "unproven", fresh ? UP_NAME(up_ncp, n->ipcp) : "unproven",
        fresh ? UP_NAME(up_terminate, n->terminate) : "unproven",
        echo, reply, frames_rx, frames_tx, in_bytes, in_reads, empty, validate_ms, write_ms, discarded, slices, clean ? "not-required" : "required", diagnostic);
    (void)discovery;
    /* An unproven cleanup outranks a lost output line. */
    return result == 4 ? 4 : c.output_error ? 1 : result;
blocked:
    if (c.store) { CFRelease(c.store); c.store = NULL; }
    {
        char pre[256] = "not-run";
        if (c.pre.status != PPP_LTE_CLEANUP_NOT_RUN) (void)up_service_text(&c.pre, 1, pre, sizeof(pre));
        int residue = c.pre.status == PPP_LTE_CLEANUP_DONE && !ppp_lte_service_clean(&c.pre);
        up_line(&c, "up=blocked gate=%s sc-pre=%s next-run-cleanup=%s\n", gate, pre,
                residue ? "required" : "not-required");
        return residue ? 4 : c.output_error ? 1 : 3;
    }
}
int main(int argc, char **argv) { return lte_up(argc, argv, stdout, NULL); }
