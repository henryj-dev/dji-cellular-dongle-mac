#define _DARWIN_C_SOURCE
#include "discovery_internal.h"
#include "discovery.h"
#include "ppp_lte_link.h"
#include "ppp_lte_network.h"
#include "ppp_lte_service.h"
#include "ppp_runner.h"
#include "ppp_trial_preflight.h"
#include "status_probe.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Real fixed labels/keys; every OS-touching call below is replaced in up.c only. */
#include "../src/ppp_lte_service.c"
#include "../src/ppp_lte_link.c"
/* The real link step is never run here (fake below); its symbol must still link. */
int ppp_lte_endpoint_allowed(struct in_addr address) { (void)address; return -1; }

static unsigned checks, failures;
/* Tentative declaration of up.c's latch, so fixtures can raise it. */
static volatile sig_atomic_t up_hangup;
/* Mirrors UP_CEILING_MS; asserted equal after the include. */
#define FIXTURE_CEILING_MS (UINT64_C(30) * 24 * 60 * 60 * 1000 - 600000)
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-up scenario=%d line=%d\n", (int)f.scenario, __LINE__); } } while (0)
enum scenario { USER_STOP, HANGUP, PPPD_EXIT, USB_UNPLUG, PRIMARY_NOT_TAKEN, ATTRIBUTION_MISMATCH,
    INTERFACE_UNPROVEN, ADDRESS_UNPROVEN, ADDRESS_LOST_TIMEOUT, ADDRESS_RESTORED, CEILING,
    SAMPLE_SIGNAL, ATTRIBUTION_LOST, PRE_REMOVED, POST_DIRTY, RESTORE_NO, ROOT_BLOCK, STORE_ERROR,
    PRE_PPPD, PRE_INSTANCE, PRE_UNKNOWN, PRE_RESIDUE, BASELINE_ERROR, SESSION_REJECT, RUNNER_REJECT,
    OUTPUT_LOST, OWNERSHIP_LOST, OUTPUT_LATE, EARLY_HANGUP, SCENARIOS };
static struct {
    enum scenario scenario;
    unsigned host, identity, preflight, stores, store_releases, baseline, discovery, starts, slices;
    unsigned capture, samples, link_inits, link_steps, cleanups, primaries, closed_at_reap_cleanup;
    unsigned restores, released, finishes, session_steps, session_release, sleeps, hangup_installs, restores_sig;
    unsigned sigpipe_installs, waits_short, waits_long, short_run, max_short_run, odd_runs;
    uint64_t now, step_ms, start_ms, signal_at_ms;
    int stop, ended, closed, quiet, connected_seen;
    void (*hangup)(int);
    FILE *out;
    int late_closed;
    FILE *late_stream;
    struct ppp_runner_status status;
    struct ppp_session_observation child;
} f;
/* Output size when the observer was disposed, right before the summary line. */
static long dispose_size = -1;
static SCDynamicStoreRef fake_store_value(void) { return (SCDynamicStoreRef)CFRetain(CFSTR("fake-store")); }
static SCDynamicStoreRef fake_store_create(CFAllocatorRef a, CFStringRef name, SCDynamicStoreCallBack cb,
                                           SCDynamicStoreContext *context) {
    (void)a; (void)name; (void)cb; (void)context; f.stores++; f.host++;
    return f.scenario == STORE_ERROR ? NULL : fake_store_value();
}
static uid_t fake_uid(void *context) { (void)context; return 0; }
static int fake_home(void *context, uid_t uid, const char **home) { (void)context; (void)uid; *home = "/fixture"; return 0; }
static int fake_metadata(void *context, const char *path, struct stat *st) {
    (void)context; f.host++;
    *st = (struct stat){.st_mode = !strcmp(path, "/usr/sbin/pppd") ? S_IFREG | 0755 : S_IFDIR | 0755};
    return 0;
}
static int fake_identity(void *context) { (void)context; f.identity++; f.host++; return f.scenario == ROOT_BLOCK ? -1 : 0; }
static const char *fake_executable(const struct trial_backend *b) { (void)b; return "clear"; }
static int fake_preflight(void *context, struct trial_preflight *p) {
    (void)context; f.preflight++; f.host++;
    *p = (struct trial_preflight){.status = TRIAL_PREFLIGHT_COMPLETE, .metadata.clear = 1,
        .backend = {&f, fake_uid, fake_home, fake_metadata}};
    return 0;
}
static int fake_clock(clockid_t clock, struct timespec *ts) {
    CHECK(clock == CLOCK_MONOTONIC);
    uint64_t value = f.now += f.step_ms;
    /* Ceiling: once connected, the clock jumps just past the session ceiling. */
    if (f.scenario == CEILING && f.connected_seen && f.start_ms && value - f.start_ms < FIXTURE_CEILING_MS)
        value = f.now = f.start_ms + FIXTURE_CEILING_MS + 5;
    ts->tv_sec = (time_t)(value / 1000); ts->tv_nsec = (long)(value % 1000) * 1000000;
    return 0;
}
static int fake_sleep(const struct timespec *delay, struct timespec *remaining) {
    CHECK(!remaining && (delay->tv_nsec == 10000000L || delay->tv_nsec == 250000000L) && !delay->tv_sec);
    f.sleeps++; return 0;
}
static int fake_sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    CHECK(number == SIGHUP || number == SIGPIPE);
    if (old) {
        *old = (struct sigaction){.sa_handler = SIG_DFL};
        if (number == SIGHUP) { f.hangup = action->sa_handler; f.hangup_installs++; }
        else { CHECK(action->sa_handler == SIG_IGN); f.sigpipe_installs++; }
    } else {
        CHECK(action->sa_handler == SIG_DFL); f.restores_sig++; if (number == SIGHUP) f.hangup = NULL;
        /* Handlers come back only after the last line was written. */
        if (f.out && dispose_size >= 0) CHECK(ftell(f.out) > dispose_size);
        if (f.out && dispose_size < 0) CHECK(ftell(f.out) > 0);
    }
    return 0;
}
static void fake_observer_begin(struct ppp_trial_observer *o) {
    f.baseline++; memset(o, 0, sizeof(*o));
    /* The baseline must come after the pre-start cleanup. */
    CHECK(f.cleanups == 1);
    if (f.scenario == BASELINE_ERROR) o->snapshot_error = 1;
}
static void fake_observer_child(struct ppp_trial_observer *o, pid_t pid) {
    CHECK(pid == 98765 && f.slices == 1); f.capture++; o->identity = 1; o->pid = pid; f.start_ms = f.now;
}
static void fake_observer_sample(struct ppp_trial_observer *o) {
    CHECK(f.capture == 1 && !f.child.reaped && !f.child.ownership_lost); f.samples++;
    if (f.scenario == INTERFACE_UNPROVEN) { o->current_valid = 0; return; }
    if (f.scenario == SAMPLE_SIGNAL && f.connected_seen) {
        /* The signal lands inside this sample, which also loses the service. */
        f.status.signal_number = SIGINT; o->current_valid = 0; o->attributed = 0; return;
    }
    o->current_valid = 1; o->attributed = 1;
    o->service = f.scenario == ATTRIBUTION_MISMATCH ? CFSTR("State:/Network/Service/OTHER/PPP") :
        ppp_lte_service_key(PPP_LTE_ENTITY_PPP);
    if (f.scenario == ATTRIBUTION_LOST && f.connected_seen) o->ambiguous = 1;
}
static int fake_observer_current(const struct ppp_trial_observer *o, struct ppp_trial_interface *i) {
    if (o->ambiguous || !o->current_valid) return -1;
    memset(i, 0, sizeof(*i)); strcpy(i->name, "private-if-marker"); i->index = 7; return 0;
}
static void fake_observer_finish(struct ppp_trial_observer *o) { (void)o; f.finishes++; CHECK(!f.starts || f.released || f.session_release); }
static int fake_observer_result(const struct ppp_trial_observer *o) { (void)o; return 0; }
static int fake_observer_diagnostic(const struct ppp_trial_observer *o, char *b, size_t n) {
    (void)o; return snprintf(b, n, "attribution=fixture service-state=removed");
}
static void fake_observer_dispose(struct ppp_trial_observer *o) { (void)o; if (f.out) dispose_size = ftell(f.out); }
static int fake_session_start(const struct ppp_session_request *r, const struct ppp_metadata_backend *b,
                              struct ppp_session **s) {
    CHECK(r->launch == PPP_LAUNCH_MODE_UP && r->read_timeout_ms && *r->read_timeout_ms == 75000);
    CHECK(r->budgets.start_ms == 5000 && r->budgets.connect_ms == 10000 && r->budgets.drain_ms == 10000);
    CHECK(r->budgets.session_ms == UINT64_C(2592000000) && r->capacity == PPP_SUPERVISOR_BUFFER);
    CHECK(r->service == 1 && r->registry_id == 42 && b->context == &f && !*s);
    if (f.scenario == SESSION_REJECT) return -1;
    *s = (struct ppp_session *)&f; f.starts++; return 0;
}
static int fake_session_stop(struct ppp_session *s) { CHECK(s == (struct ppp_session *)&f); f.stop = 1; return 0; }
static int fake_session_step(struct ppp_session *s) { CHECK(s == (struct ppp_session *)&f && f.stop); return ++f.session_steps >= 3; }
static int fake_session_release(struct ppp_session **s) { *s = NULL; f.session_release++; return 0; }
static int fake_runner_begin(struct ppp_session **s, struct ppp_runner **r) {
    if (f.scenario == RUNNER_REJECT) return -1;
    *s = NULL; *r = (struct ppp_runner *)&f; return 0;
}
static int fake_runner_observe(struct ppp_runner *r, struct ppp_runner_status *s, struct ppp_session_observation *o) {
    CHECK(r == (struct ppp_runner *)&f); *s = f.status; *o = f.child; return 0;
}
/* TERM path: pppd exits 5 on its own after the drain; error paths are killed. */
static void fake_reap(int code, int signal_number) {
    f.child.reaped = 1; f.child.stdout_eof = f.child.stderr_eof = 1;
    if (signal_number) { f.child.signaled = 1; f.child.term_signal = signal_number; f.child.kill_signals = 1; }
    else { f.child.exited = 1; f.child.exit_code = code; }
}
static void fake_close(void) {
    f.quiet = 1; f.child.peer_quiet = f.child.peer_closed = 1;
    f.status.session.local_done = 1; f.status.session.state = PPP_SESSION_CLOSED;
    f.status.session.supervisor_phase = PPP_CLOSED;
    if (!f.status.session.supervisor_error) {
        f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR; f.status.session.supervisor_error = PPP_ERROR_TIMEOUT;
    }
    f.closed = 1;
}
static int fake_runner_slice(struct ppp_runner *r, const volatile sig_atomic_t *stop,
                             const uint32_t *wait, struct ppp_runner_status *s) {
    CHECK(r == (struct ppp_runner *)&f && wait && *wait >= 1 && *wait <= 10); f.slices++;
    if (f.slices > 200000) { fprintf(stderr, "runaway scenario %d\n", (int)f.scenario); exit(1); }
    if (f.slices == 1) {
        f.child = (struct ppp_session_observation){.pid = 98765, .started = 1, .exec_done = 1};
        f.status.session = (struct ppp_session_status){.state = PPP_SESSION_RUNNING, .supervisor_valid = 1,
            .supervisor_phase = PPP_SESSION};
    }
    /* Traffic counters move while connected. */
    /* Sparse traffic: one 100-byte read every 150 slices, so the 1 ms hold
     * (100 slices) and the return to 10 ms are both visible. */
    if (f.connected_seen && f.slices % 150 == 0) { f.child.usb_input_bytes += 100; f.child.usb_input_reads++; }
    if (*wait == 1) { f.waits_short++; f.short_run++; }
    else {
        if (*wait == 10) f.waits_long++;
        if (f.short_run > f.max_short_run) f.max_short_run = f.short_run;
        if (f.short_run && f.short_run != 100) f.odd_runs++;
        f.short_run = 0;
    }
    uint64_t elapsed = f.start_ms ? f.now - f.start_ms : 0;
    if (!f.ended && f.connected_seen && f.signal_at_ms && elapsed >= f.signal_at_ms) {
        if (f.scenario == HANGUP && f.hangup) f.hangup(SIGHUP);
        else if (f.scenario == PPPD_EXIT) { fake_reap(15, 0); f.ended = 1; }
        else if (f.scenario == USB_UNPLUG) {
            f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR;
            f.status.session.supervisor_error = PPP_ERROR_USB; f.status.session.supervisor_phase = PPP_CLEANUP;
            f.child.usb_error_kind = PPP_USB_ERROR_NO_DEVICE; f.child.usb_error_site = PPP_USB_SITE_COMPLETION;
            fake_reap(0, SIGKILL); f.ended = 1;
        } else if (f.scenario == OWNERSHIP_LOST) { f.child.ownership_lost = 1; f.ended = 1;
        } else if (f.scenario == USER_STOP || f.scenario == ADDRESS_RESTORED || f.scenario == POST_DIRTY ||
                   f.scenario == RESTORE_NO || f.scenario == PRE_REMOVED || f.scenario == OUTPUT_LOST)
            f.status.signal_number = SIGINT;
    }
    static unsigned stopped_at;
    if (!f.closed && (*stop || f.status.signal_number || f.ended)) {
        if (!f.stop) { f.stop = 1; stopped_at = f.slices; f.status.stop_requested = *stop || f.status.signal_number; }
        /* Unplug: the callback answer is late, the WAIT is long. */
        unsigned finish = f.scenario == USB_UNPLUG ? 40 : 4;
        if (f.slices - stopped_at == 2 && !f.child.reaped && !f.child.ownership_lost) {
            f.child.term_signals = 1; fake_reap(5, 0);
        }
        if (f.slices - stopped_at >= finish) fake_close();
    }
    *s = f.status; return f.status.session.local_done;
}
static int fake_runner_restore(struct ppp_runner *r, struct ppp_runner_status *s) {
    CHECK(r == (struct ppp_runner *)&f && f.closed && f.quiet); f.restores++;
    f.status.unrestored_actions = 0; *s = f.status; return 0;
}
static int fake_runner_release(struct ppp_runner **r) { CHECK(f.closed && f.restores); f.released++; *r = NULL; return 0; }
static int fake_discovery(discovery_target_callback callback, void *context) {
    f.discovery++;
    struct descriptor_config config = {0};
    IOUSBDeviceInterface *device = NULL;
    return callback(context, 1, &device, 42, &config);
}
static int fake_link_init(struct ppp_lte_link *l, const char *name) {
    f.link_inits++; CHECK(!strcmp(name, "private-if-marker"));
    return ppp_lte_link_init(l, name);
}
/* Fixture address timeline relative to the link start. */
static uint64_t link_start;
static int fake_link_step(struct ppp_lte_link *l, uint64_t now) {
    f.link_steps++;
    if (!link_start) link_start = now;
    uint64_t t = now - link_start;
    l->last_ms = now;
    if (f.scenario == ADDRESS_UNPROVEN) { l->state = PPP_LTE_LINK_SETTLING; if (!l->first_pin_ms) l->first_pin_ms = now; return l->state; }
    if (t < 3000) { l->state = PPP_LTE_LINK_SETTLING; if (!l->first_pin_ms) l->first_pin_ms = now; return l->state; }
    if (!l->first_stable_ms) l->first_stable_ms = now;
    int lost = (f.scenario == ADDRESS_LOST_TIMEOUT && t >= 20000) ||
        (f.scenario == ADDRESS_RESTORED && t >= 20000 && t < 26000);
    if (lost) {
        if (!l->lost_since_ms) { l->lost_since_ms = now; l->losses++; l->resets++; }
        l->state = PPP_LTE_LINK_ABSENT; return l->state;
    }
    if (l->lost_since_ms) { l->lost_since_ms = 0; l->restores++; }
    l->stable_once = 1; l->state = PPP_LTE_LINK_STABLE; return l->state;
}
static int fake_cleanup(SCDynamicStoreRef store, struct ppp_lte_service_cleanup *r) {
    CHECK(store != NULL); f.cleanups++;
    *r = (struct ppp_lte_service_cleanup){.status = PPP_LTE_CLEANUP_DONE};
    if (f.cleanups == 1) {
        if (f.scenario == EARLY_HANGUP) up_hangup = 1;
        if (f.scenario == PRE_PPPD) r->status = PPP_LTE_CLEANUP_BLOCKED_PPPD;
        if (f.scenario == PRE_INSTANCE) r->status = PPP_LTE_CLEANUP_BLOCKED_PEER;
        if (f.scenario == PRE_UNKNOWN) r->before.unknown = r->after.unknown = 1;
        if (f.scenario == PRE_RESIDUE) { r->before.present = r->after.present = 2; r->failed = 1; }
        if (f.scenario == PRE_REMOVED) { r->before.present = 6; r->removed = 2; }
        return 0;
    }
    /* After a reap: what pppd left; the KILL path leaves everything. */
    if (f.child.reaped && !f.closed_at_reap_cleanup && f.cleanups == 2) {
        f.closed_at_reap_cleanup = f.closed ? 2 : 1;
        r->before.present = f.child.kill_signals ? (1u << PPP_LTE_ENTITIES) - 1 : 1u << PPP_LTE_ENTITY_IPV6;
        r->removed = (unsigned)__builtin_popcount(r->before.present);
        return 0;
    }
    if (f.scenario == POST_DIRTY) { r->before.present = r->after.present = 1; r->failed = 1; }
    return 0;
}
static int fake_primary(SCDynamicStoreRef store, const char *name, struct ppp_lte_primary *p) {
    CHECK(store != NULL); f.primaries++;
    *p = (struct ppp_lte_primary){.ipv4 = PPP_LTE_PRIMARY_THIS, .ipv6 = PPP_LTE_PRIMARY_THIS,
        .interface = name ? 1 : -1, .dns = PPP_LTE_DNS_ACTIVE};
    if (f.scenario == PRIMARY_NOT_TAKEN) p->ipv4 = PPP_LTE_PRIMARY_OTHER;
    if (f.closed && f.scenario != RESTORE_NO) { p->ipv4 = PPP_LTE_PRIMARY_OTHER; p->ipv6 = PPP_LTE_PRIMARY_NONE; }
    if (name && p->ipv4 == PPP_LTE_PRIMARY_THIS) f.connected_seen = 1;
    /* The reader goes away right before the connected line. */
    if (f.scenario == OUTPUT_LATE && name && !f.late_closed) { fclose(f.late_stream); f.late_closed = 1; }
    return 0;
}
static FILE *broken_output;
/* Lock file: never the host /var/run; counts must balance after each run. */
static unsigned lock_opens, lock_closes, lock_takes;
static int lock_busy, lock_open_error;
static int lock_fake_open(const char *path, int flags, ...) {
    if (strcmp(path, "/var/run/dji-cellular-dongle-mac.lock") || !(flags & O_CLOEXEC) || !(flags & O_NOFOLLOW)) return -1;
    if (lock_open_error) { errno = EACCES; return -1; }
    lock_opens++; return 777;
}
static int lock_fake_flock(int fd, int operation) {
    if (fd != 777 || operation != (LOCK_EX | LOCK_NB)) return -1;
    if (lock_busy) { errno = EWOULDBLOCK; return -1; }
    lock_takes++; return 0;
}
static int lock_planted; /* 1: FIFO, 2: second hard link, 3: not root-owned */
static int lock_fake_fstat(int fd, struct stat *st) {
    if (fd != 777) return -1;
    *st = (struct stat){.st_mode = lock_planted == 1 ? S_IFIFO | 0644 : S_IFREG | 0644,
        .st_nlink = lock_planted == 2 ? 2 : 1, .st_uid = lock_planted == 3 ? 501 : 0};
    return 0;
}
static int lock_fake_close(int fd) { if (fd == 777) lock_closes++; return 0; }
int trial_preflight(void *context, struct trial_preflight *p) { return fake_preflight(context, p); }
#define trial_identity fake_identity
#define trial_metadata fake_metadata
#define trial_executable fake_executable
#define trial_preflight(...) fake_preflight(__VA_ARGS__)
#define clock_gettime fake_clock
#define nanosleep fake_sleep
#define sigaction(...) fake_sigaction(__VA_ARGS__)
#define SCDynamicStoreCreate fake_store_create
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
/* Signal query before dialing: 0 confirmed numbers, 1 I/O error, 2 cancelled,
 * 3 CSQ only (CESQ answered ERROR), 4 two +CSQ lines (CSQ refused). Kept outside f, which setup() clears. */
static int signal_mode;
static unsigned signal_probes;
static int fake_probe_signal(io_service_t service, IOUSBDeviceInterface **device, uint64_t registry_id,
                             const struct descriptor_config *config, const volatile sig_atomic_t *prior,
                             struct status_probe_result *r) {
    (void)service; (void)device; (void)registry_id; (void)config;
    signal_probes++;
    CHECK(prior == &up_hangup);
    memset(r, 0, sizeof(*r));
    r->stop_index = STATUS_PROBE_SLOTS;
    if (signal_mode == 1 || signal_mode == 2) {
        r->stop = signal_mode == 1 ? STATUS_STOP_IO_ERROR : STATUS_STOP_CANCELLED;
        return signal_mode == 1 ? DJI_GONE : DJI_CANCELLED;
    }
    struct status_command_result *csq = &r->commands[1], *cesq = &r->commands[STATUS_PROBE_CESQ_INDEX];
    *csq = (struct status_command_result){.sent = 1, .complete = 1, .values = 2, .value_lines = 1, .value = {20, 99},
        .result = STATUS_RESULT_OK, .attribution = STATUS_ATTRIBUTION_MATCHED};
    if (signal_mode == 3) {
        *cesq = (struct status_command_result){.sent = 1, .complete = 1, .result = STATUS_RESULT_ERROR,
            .attribution = STATUS_ATTRIBUTION_MATCHED};
        r->stop = STATUS_STOP_FINAL_ERROR; r->stop_index = STATUS_PROBE_CESQ_INDEX;
        return DJI_OK;
    }
    *cesq = (struct status_command_result){.sent = 1, .complete = 1, .values = 6, .value_lines = 1, .value = {99, 99, 255, 255, 19, 46},
        .result = STATUS_RESULT_OK, .attribution = STATUS_ATTRIBUTION_MATCHED};
    if (signal_mode == 4) csq->value_lines = 2; /* two +CSQ lines in one reply */
    r->stop = STATUS_STOP_NONE;
    return DJI_OK;
}
#define usb_observer_probe_signal fake_probe_signal
#define ppp_lte_link_init fake_link_init
#define ppp_lte_link_step fake_link_step
#define ppp_lte_service_cleanup(...) fake_cleanup(__VA_ARGS__)
#define ppp_lte_service_primary fake_primary
#define CFRelease(x) (f.store_releases++, CFRelease(x))
#define open(...) lock_fake_open(__VA_ARGS__)
#define flock(...) lock_fake_flock(__VA_ARGS__)
#define close(...) lock_fake_close(__VA_ARGS__)
#define fstat(...) lock_fake_fstat(__VA_ARGS__)
#define main up_main
#include "../src/ppp_lte_up.c"
#undef main
#undef close
#undef fstat
#undef flock
#undef open
#undef CFRelease
#undef ppp_lte_service_cleanup
_Static_assert(FIXTURE_CEILING_MS == UP_CEILING_MS, "fixture ceiling");

static void setup(enum scenario s) {
    memset(&f, 0, sizeof(f)); f.scenario = s; f.now = 1000; f.step_ms = 10; f.signal_at_ms = 30000;
    link_start = 0; up_hangup = 0; dispose_size = -1;
    /* Long waits: coarse clock so the fixture stays small. */
    if (s == INTERFACE_UNPROVEN || s == ADDRESS_UNPROVEN || s == ADDRESS_LOST_TIMEOUT ||
        s == PRIMARY_NOT_TAKEN) f.step_ms = 100;
    if (s == ADDRESS_RESTORED) f.signal_at_ms = 40000;
    if (s == ADDRESS_LOST_TIMEOUT) f.signal_at_ms = 0;
}
static const char *expected(enum scenario s) {
    switch (s) {
    case USER_STOP: case HANGUP:
        return "up-result=stopped reason=user-stop connected=yes";
    case PPPD_EXIT: return "up-result=ended reason=pppd-exit connected=yes";
    case USB_UNPLUG: return "up-result=ended reason=usb-error connected=yes";
    case PRIMARY_NOT_TAKEN: return "up-result=ended reason=primary-not-taken connected=no";
    case ATTRIBUTION_MISMATCH: return "up-result=ended reason=attribution-mismatch connected=no";
    case INTERFACE_UNPROVEN: return "up-result=ended reason=interface-unproven connected=no";
    case ADDRESS_UNPROVEN: return "up-result=ended reason=address-unproven connected=no";
    case ADDRESS_LOST_TIMEOUT: return "up-result=ended reason=address-lost-timeout connected=yes";
    case ADDRESS_RESTORED: return "up-result=stopped reason=user-stop connected=yes";
    case CEILING: return "up-result=ended reason=session-ceiling";
    case SAMPLE_SIGNAL: return "up-result=stopped reason=user-stop connected=yes";
    case ATTRIBUTION_LOST: return "up-result=ended reason=attribution-lost connected=yes";
    case PRE_REMOVED: return "up-result=stopped reason=user-stop";
    case POST_DIRTY: return "up-result=cleanup-required reason=user-stop";
    case RESTORE_NO: return "up-result=ended reason=user-stop";
    case ROOT_BLOCK: return "up=blocked gate=root-identity sc-pre=not-run next-run-cleanup=not-required";
    case STORE_ERROR: return "up=blocked gate=store-error sc-pre=not-run";
    case PRE_PPPD: return "up=blocked gate=pppd-running sc-pre=blocked-pppd";
    case PRE_INSTANCE: return "up=blocked gate=instance-running sc-pre=blocked-instance";
    case PRE_UNKNOWN: return "up=blocked gate=service-unknown-keys sc-pre=done:removed:0:failed:0:before:other:1:after:other:1 next-run-cleanup=required";
    case PRE_RESIDUE: return "up=blocked gate=service-residue sc-pre=done:removed:0:failed:1:before:ipv4:after:ipv4 next-run-cleanup=required";
    case BASELINE_ERROR: return "up-result=ended reason=baseline-unproven connected=no";
    case SESSION_REJECT: return "up-result=ended reason=session-rejected connected=no";
    case RUNNER_REJECT: return "up-result=ended reason=runner-rejected connected=no";
    case OUTPUT_LOST: case OUTPUT_LATE: return NULL;
    case OWNERSHIP_LOST: return "up-result=cleanup-required reason=pppd-exit connected=yes";
    case EARLY_HANGUP: return "up=blocked gate=user-stop sc-pre=done:removed:0:failed:0:before:none:after:none next-run-cleanup=not-required";
    case SCENARIOS: break;
    }
    return NULL;
}
static int expected_exit(enum scenario s) {
    switch (s) {
    case USER_STOP: case HANGUP: case ADDRESS_RESTORED: case SAMPLE_SIGNAL: case PRE_REMOVED: return 0;
    case POST_DIRTY: case PRE_UNKNOWN: case PRE_RESIDUE: case STORE_ERROR: return s == STORE_ERROR ? 3 : 4;
    case OUTPUT_LOST: case OUTPUT_LATE: return 1;
    case OWNERSHIP_LOST: return 4;
    default: return 3;
    }
}
static void run(enum scenario s) {
    setup(s); lock_opens = lock_closes = lock_takes = 0;
    signal_probes = 0;
    if (s == CEILING) f.signal_at_ms = 0;
    FILE *output = tmpfile(); CHECK(output != NULL); if (!output) return;
    char *argv[] = {"up", "--up"};
    FILE *target = output;
    /* A read-only stream: the first line already fails. */
    if (s == OUTPUT_LOST) { broken_output = fopen("/dev/null", "r"); target = broken_output; }
    /* Output that breaks while connected: a pipe whose reader is gone. */
    int fds[2] = {-1, -1};
    if (s == OUTPUT_LATE) {
        CHECK(pipe(fds) == 0); target = fdopen(fds[1], "w"); f.late_stream = fdopen(fds[0], "r");
        setvbuf(target, NULL, _IONBF, 0);
    }
    f.out = s == OUTPUT_LOST || s == OUTPUT_LATE ? NULL : output;
    int result = lte_up(2, argv, target, NULL);
    if (s == OUTPUT_LATE) { fclose(target); if (!f.late_closed) fclose(f.late_stream); }
    CHECK(result == expected_exit(s));
    /* The lock is taken once after the root check and always released. */
    CHECK(lock_closes == lock_opens && lock_takes == (s == ROOT_BLOCK ? 0u : 1u));
    if (broken_output) { fclose(broken_output); broken_output = NULL; }
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    char text[8192] = {0}; (void)fread(text, 1, sizeof(text) - 1, output);
    fclose(output);
    /* Output gone before the start: pppd is never launched; handlers restored. */
    if (s == OUTPUT_LOST) { CHECK(!f.discovery && !f.starts && f.restores_sig == 2 && f.stores == f.store_releases); return; }
    /* Lost while connected: the session still stops, closes and is released. */
    if (s == OUTPUT_LATE) { CHECK(f.released == 1 && f.closed && f.restores_sig == 2 && f.closed_at_reap_cleanup == 1 &&
                                  f.late_closed && f.child.exit_code == 5); return; }
    const char *want = expected(s);
    if (want && !strstr(text, want)) { failures++; fprintf(stderr, "FAIL lte-up scenario=%d text=%s\n", (int)s, text); }
    checks++;
    /* Never a name, address, UUID or pid. */
    CHECK(!strstr(text, "private") && !strstr(text, PPP_LTE_UP_SERVICE_ID) && !strstr(text, "98765") &&
          !strstr(text, "/fixture") && !strstr(text, "0x"));
    /* Every store taken is released; handlers restored when installed. */
    CHECK(f.stores == f.store_releases || f.scenario == STORE_ERROR);
    CHECK(f.hangup_installs == f.sigpipe_installs && f.restores_sig == 2 * f.hangup_installs && !f.hangup);
    int blocked = (s >= ROOT_BLOCK && s <= PRE_RESIDUE) || s == EARLY_HANGUP;
    if (blocked) {
        CHECK(!f.baseline && !f.discovery && !f.starts && f.hangup_installs == 1 && !strstr(text, "up=starting"));
        return;
    }
    CHECK(strstr(text, "up=starting sc-pre-removed=") == text);
    if (s == PRE_REMOVED) CHECK(strstr(text, "up=starting sc-pre-removed=2\n") == text);
    CHECK(f.finishes == 1);
    /* One signal line after the start and before the summary, whenever a target was reached. */
    if (s == BASELINE_ERROR) CHECK(!strstr(text, "up-signal"));
    else {
        char line[256];
        if (signal_mode == 1)
            (void)snprintf(line, sizeof(line), "\nup-signal=unproven csq-rssi=unproven rssi-dbm=unproven"
                " cesq-rsrp=unproven rsrp-dbm=unproven cesq-rsrq=unproven rsrq-db=unproven probe-status=%d"
                " stop=io-error\n", DJI_GONE);
        else if (signal_mode == 4)
            (void)snprintf(line, sizeof(line), "\nup-signal=partial csq-rssi=unproven rssi-dbm=unproven cesq-rsrp=46"
                " rsrp-dbm=-95 cesq-rsrq=19 rsrq-db=-10.5 probe-status=0 stop=none\n");
        else if (signal_mode == 3)
            (void)snprintf(line, sizeof(line), "\nup-signal=partial csq-rssi=20 rssi-dbm=-73 cesq-rsrp=unproven"
                " rsrp-dbm=unproven cesq-rsrq=unproven rsrq-db=unproven probe-status=0 stop=final-error\n");
        else
            (void)snprintf(line, sizeof(line), "\nup-signal=ok csq-rssi=20 rssi-dbm=-73 cesq-rsrp=46 rsrp-dbm=-95"
                " cesq-rsrq=19 rsrq-db=-10.5 probe-status=0 stop=none\n");
        const char *at = strstr(text, line), *summary = strstr(text, "up-result=");
        CHECK(at && summary && at < summary && signal_probes == 1);
        if (!at) fprintf(stderr, "FAIL lte-up signal text=%s\n", text);
    }
    if (s == BASELINE_ERROR || s == SESSION_REJECT || s == RUNNER_REJECT) {
        CHECK(!f.capture && !f.slices && strstr(text, " sc-reap=not-run ") && strstr(text, " pppd-leftover=not-run "));
        if (s == RUNNER_REJECT) CHECK(f.session_release == 1 && f.session_steps == 3);
        return;
    }
    CHECK(f.capture == 1 && f.released == 1 && f.closed);
    /* The reap cleanup ran before CLOSED, while the drain/WAIT still ran; a
     * possibly live pppd (ownership lost) is never cleaned up after. */
    if (s == OWNERSHIP_LOST) CHECK(!f.closed_at_reap_cleanup && f.cleanups == 1 &&
        strstr(text, " sc-reap=not-run sc-post=not-run primary-restored=unproven ") &&
        strstr(text, " next-run-cleanup=required "));
    else CHECK(f.closed_at_reap_cleanup == 1);
    int connected = strstr(text, "\nup=connected ") != NULL;
    CHECK(connected == (strstr(text, " connected=yes ") != NULL));
    if (connected) CHECK(strstr(text, "up=connected elapsed-ms=") &&
        strstr(text, " primary-ipv4=this-service primary-ipv6=this-service primary-interface=this-interface"
                     " dns=active ip-resets=0\n"));
    if (s == USER_STOP || s == HANGUP || s == SAMPLE_SIGNAL || s == PRE_REMOVED || s == ADDRESS_RESTORED)
        CHECK(strstr(text, " pppd-leftover=ipv6 sc-reap=done:removed:1:failed:0:before:ipv6:after:none"
                           " sc-post=done:removed:0:failed:0:before:none:after:none primary-restored=yes") &&
              strstr(text, " child-exit=code:5 term-count=1 kill-count=0 ") &&
              strstr(text, " ppp-lcp-echo=0/0 ppp-lcp-echo-reply=0/0 ") &&
              strstr(text, " usb-in-bytes=") && strstr(text, " usb-in-reads=") &&
              strstr(text, " remote-termination=unproven next-run-cleanup=not-required attribution=fixture"));
    if (s == USER_STOP) {
        /* The summary counts match the fixture: 100 bytes per completed read. */
        unsigned long long in_bytes = 0, in_reads = 0;
        const char *b = strstr(text, " usb-in-bytes="), *r = strstr(text, " usb-in-reads=");
        CHECK(b && r && sscanf(b, " usb-in-bytes=%llu", &in_bytes) == 1 && sscanf(r, " usb-in-reads=%llu", &in_reads) == 1);
        CHECK(in_reads == f.child.usb_input_reads && in_bytes == 100 * in_reads && in_reads > 0);
        /* Diagnostic timing labels are present; the loop count is the fixture's slices. */
        unsigned long long loop = 0;
        const char *l = strstr(text, " loop-slices=");
        CHECK(strstr(text, " usb-in-empty-polls=0 usb-validate-ms=0 usb-write-ms=0 usb-in-discarded=0 loop-slices=") && l &&
              sscanf(l, " loop-slices=%llu", &loop) == 1 && loop == f.slices);
        /* Each movement holds 1 ms for exactly 100 slices, then 10 ms again. */
        CHECK(f.waits_short > 0 && f.waits_long > 0 && f.max_short_run == 100 && !f.odd_runs);
    }
    if (s == USB_UNPLUG)
        CHECK(strstr(text, " pppd-leftover=ppp+ipv4+ipv6+dns+proxies+interface+smb sc-reap=done:removed:7") &&
              strstr(text, " child-exit=signal:9 ") && strstr(text, " usb-error=no-device usb-error-site=completion "));
    if (s == PPPD_EXIT) CHECK(strstr(text, " child-exit=code:15 term-count=0 kill-count=0 ") != NULL);
    if (s == ADDRESS_RESTORED)
        CHECK(strstr(text, "\nup=address-lost link=absent losses=1\nup=address-restored restores=1\n") &&
              strstr(text, " ip-losses=1 ip-restores=1 "));
    if (s == ADDRESS_LOST_TIMEOUT) CHECK(strstr(text, "\nup=address-lost link=absent losses=1\n") != NULL);
    if (s == HANGUP) CHECK(f.hangup_installs == 1);
    if (s == RESTORE_NO) CHECK(strstr(text, " primary-restored=no ") != NULL);
    if (s == POST_DIRTY) CHECK(strstr(text, " next-run-cleanup=required ") != NULL);
    if (s == INTERFACE_UNPROVEN) CHECK(!f.link_inits && f.now - f.start_ms >= 30000);
    if (s == ATTRIBUTION_MISMATCH) CHECK(!f.link_inits);
    if (s == ADDRESS_UNPROVEN) CHECK(f.link_inits == 1 && strstr(text, " ip-stable-ms=none "));
    /* A user stop is never filed as an attribution failure. */
    if (s == SAMPLE_SIGNAL) CHECK(!strstr(text, "attribution-lost"));
    /* The ceiling stops through the TERM path, never the supervisor KILL. */
    if (s == CEILING) CHECK(strstr(text, " child-exit=code:5 term-count=1 kill-count=0 ") != NULL);
}
static void cleanup_mode(void) {
    char *argv[] = {"up", "--cleanup"};
    for (unsigned i = 0; i < 4; i++) {
        memset(&f, 0, sizeof(f));
        if (i == 1) f.scenario = POST_DIRTY;
        if (i == 2) f.scenario = ROOT_BLOCK;
        if (i == 3) f.scenario = STORE_ERROR;
        f.cleanups = 1; /* skip the pre-start branch of the fixture */
        FILE *output = tmpfile();
        int result = lte_up(2, argv, output, NULL);
        CHECK(result == (i == 0 ? 0 : i == 2 ? 3 : 4));
        fseek(output, 0, SEEK_SET);
        char text[1024] = {0}; (void)fread(text, 1, sizeof(text) - 1, output); fclose(output);
        const char *want[] = {"cleanup=clean sc=done:removed:0:failed:0:before:none:after:none\n",
            "cleanup=unproven sc=done:removed:0:failed:1:before:ppp:after:ppp\n",
            "cleanup=blocked gate=root-identity\n", "cleanup=blocked gate=store-error\n"};
        CHECK(!strcmp(text, want[i]));
        CHECK(!f.discovery && !f.starts && !f.preflight && f.hangup_installs == 1 && (f.stores == f.store_releases || i == 3));
    }
}
/* The lock: held by another run blocks before any host step; always released. */
static void lock_test(void) {
    char *up[] = {"up", "--up"}, *clean[] = {"up", "--cleanup"};
    for (unsigned i = 0; i < 4; i++) {
        memset(&f, 0, sizeof(f)); f.now = 1000; f.step_ms = 10; link_start = 0;
        lock_opens = lock_closes = lock_takes = 0;
        lock_busy = i == 0 || i == 2; lock_open_error = i == 1 || i == 3;
        FILE *output = tmpfile();
        int result = lte_up(2, i < 2 ? up : clean, output, NULL);
        fseek(output, 0, SEEK_SET);
        char text[512] = {0}; (void)fread(text, 1, sizeof(text) - 1, output); fclose(output);
        const char *gate = lock_busy ? "lock-held" : "lock-error";
        char want[96];
        (void)snprintf(want, sizeof(want), i < 2 ? "up=blocked gate=%s sc-pre=not-run" : "cleanup=blocked gate=%s\n", gate);
        CHECK(result == 3 && strstr(text, want));
        CHECK(!f.stores && !f.preflight && !f.discovery && !lock_takes && lock_closes == lock_opens);
    }
    lock_busy = lock_open_error = 0;
    /* A planted FIFO, hard link or foreign-owned file is never used as the lock. */
    for (int planted = 1; planted <= 3; planted++) {
        memset(&f, 0, sizeof(f)); lock_opens = lock_closes = lock_takes = 0; lock_planted = planted;
        FILE *output = tmpfile();
        CHECK(lte_up(2, up, output, NULL) == 3);
        fseek(output, 0, SEEK_SET);
        char text[512] = {0}; (void)fread(text, 1, sizeof(text) - 1, output); fclose(output);
        CHECK(strstr(text, "up=blocked gate=lock-error") && !lock_takes && lock_closes == lock_opens && !f.stores);
    }
    lock_planted = 0;
}
static void cli(void) {
    char *help[] = {"up", "--help"}, *bad1[] = {"up", "--down"}, *bad2[] = {"up", "--up", "x"}, *bad3[] = {"up"};
    FILE *output = tmpfile();
    memset(&f, 0, sizeof(f));
    CHECK(lte_up(2, help, output, NULL) == 0 && !f.host);
    fseek(output, 0, SEEK_SET);
    char text[1024] = {0}; (void)fread(text, 1, sizeof(text) - 1, output); fclose(output);
    CHECK(strstr(text, "--up") && strstr(text, "--cleanup"));
    CHECK(lte_up(2, bad1, stdout, NULL) == 2 && lte_up(3, bad2, stdout, NULL) == 2 && lte_up(1, bad3, stdout, NULL) == 2);
    CHECK(!f.host && !f.stores);
}
/* A stop during the signal query: no session is started and nothing is dialed. */
static void signal_cancelled(void) {
    setup(USER_STOP); lock_opens = lock_closes = lock_takes = 0; signal_probes = 0; signal_mode = 2;
    FILE *output = tmpfile(); CHECK(output != NULL); if (!output) return;
    f.out = output;
    char *argv[] = {"up", "--up"};
    int result = lte_up(2, argv, output, NULL);
    CHECK(result == 3 && signal_probes == 1 && !f.starts && !f.slices && f.finishes == 1);
    CHECK(lock_closes == lock_opens && lock_takes == 1u && f.stores == f.store_releases);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    char text[8192] = {0}; (void)fread(text, 1, sizeof(text) - 1, output);
    fclose(output);
    char line[256];
    (void)snprintf(line, sizeof(line), "\nup-signal=unproven csq-rssi=unproven rssi-dbm=unproven cesq-rsrp=unproven"
        " rsrp-dbm=unproven cesq-rsrq=unproven rsrq-db=unproven probe-status=%d stop=cancelled\n", DJI_CANCELLED);
    CHECK(strstr(text, line) && strstr(text, "up-result=ended reason=user-stop connected=no "));
    signal_mode = 0;
}
int main(void) {
    /* The process under test ignores SIGPIPE itself; the fake sigaction does not. */
    (void)signal(SIGPIPE, SIG_IGN);
    for (enum scenario s = USER_STOP; s < SCENARIOS; s++) run(s);
    /* Signal query failures never change the session itself. */
    signal_mode = 1; run(USER_STOP);
    signal_mode = 3; run(USER_STOP);
    signal_mode = 4; run(USER_STOP);
    signal_mode = 0;
    signal_cancelled();
    cleanup_mode(); lock_test(); cli();
    printf("lte-up checks=%u failures=%u (fake runner/store/link; no device, pppd or network)\n", checks, failures);
    return failures ? 1 : 0;
}
