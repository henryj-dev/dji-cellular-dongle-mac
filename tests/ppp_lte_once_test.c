#define _DARWIN_C_SOURCE
#include "discovery_internal.h"
#include "discovery.h"
#include "ppp_lte_network.h"
#include "ppp_runner.h"
#include "ppp_trial_preflight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-once line=%d\n", __LINE__); } } while (0)
enum scenario { EXACT, ROOT_BLOCK, EXEC_BLOCK, HOME_BLOCK, BASELINE_ERROR,
    CAPTURE_ERROR, ATTRIBUTION_ERROR, NETWORK_ERROR, SIGNAL_STOP, OWNERSHIP_LOST,
    CALLBACK_WAIT, RESTORE_FAILURE, CLOCK_ERROR, CLOCK_SIGNAL, GATE_SIGNAL,
    SESSION_REJECT, RUNNER_REJECT, DISCOVERY_ERROR, EARLY_CLOSED, RELEASE_ERROR, MODEM_RESULT,
    LATE_ERROR, DIAG_CLOCK_ERROR, DIAG_CLOCK_BACK, FINAL_OBSERVE_ERROR, USB_ERROR, CLEAN_CLOSE,
    ADDRESS_MISSING, TRACE_OVERFLOW };
static struct {
    enum scenario scenario;
    unsigned host, identity, metadata, preflight, baseline, discovery, starts, slices;
    unsigned capture, samples, net_init, net_steps, net_progress, net_abort, closed, restores, released;
    unsigned term, kill, quiet, child_poll, session_release, session_steps;
    uint64_t now;
    /* Fixture clock values: start read, last read before any supervisor error,
     * and the first read once an error is visible (the diagnostic read). */
    uint64_t start_ms, last_live, diag_ms;
    /* Fixture clock value recorded as the network's first address pin. */
    uint64_t pinned_ms;
    /* Fixture clock value recorded as the network's address loss. */
    uint64_t lost_ms;
    /* Fixture clock value recorded as the network's settled pin. */
    uint64_t stable_ms;
    int stop, ended;
    struct ppp_runner_status status;
    struct ppp_session_observation child;
} f;
static uid_t fake_uid(void *context) { CHECK(context == &f); return 0; }
static int fake_home(void *context, uid_t uid, const char **home) {
    CHECK(context == &f && uid == 0); *home = "/fixture"; return 0;
}
static int fake_metadata(void *context, const char *path, struct stat *st) {
    (void)context; f.metadata++; f.host++;
    *st = (struct stat){.st_mode = !strcmp(path, "/usr/sbin/pppd") ? S_IFREG | 0755 : S_IFDIR | 0755};
    return f.scenario == EXEC_BLOCK ? -1 : 0;
}
static int fake_identity(void *context) { (void)context; f.identity++; f.host++; return f.scenario == ROOT_BLOCK ? -1 : 0; }
static const char *fake_executable(const struct trial_backend *b) {
    struct stat st;
    return b->metadata(b->context, "/usr/sbin/pppd", &st) ? "metadata-error" : "clear";
}
static int fake_preflight(void *context, struct trial_preflight *p) {
    (void)context; f.preflight++; f.host++;
    *p = (struct trial_preflight){.status = TRIAL_PREFLIGHT_COMPLETE, .metadata.clear = 1,
        .backend = {&f, fake_uid, fake_home, fake_metadata}};
    return f.scenario == HOME_BLOCK ? -1 : 0;
}
static int fake_clock(clockid_t clock, struct timespec *ts) {
    CHECK(clock == CLOCK_MONOTONIC); f.host++;
    if (f.scenario == CLOCK_ERROR) return -1;
    /* The first clock after the first sample is the gate's recheck: the signal
     * lands inside the clock call, after every flag looked clean. */
    if (f.scenario == CLOCK_SIGNAL && f.samples == 1) f.status.signal_number = SIGINT;
    int error = f.status.session.supervisor_error != PPP_ERROR_NONE;
    if (error && !f.diag_ms && f.scenario == DIAG_CLOCK_ERROR) return -1;
    uint64_t value = f.now += 10;
    /* Backward only against the last live read, still after the start. */
    if (error && !f.diag_ms && f.scenario == DIAG_CLOCK_BACK) value = f.last_live - 1;
    if (f.capture == 1 && !f.start_ms) f.start_ms = value;
    if (error && !f.diag_ms) f.diag_ms = value;
    else if (!error) f.last_live = value;
    ts->tv_sec = (time_t)(value / 1000); ts->tv_nsec = (long)(value % 1000) * 1000000;
    return 0;
}
static int fake_sleep(const struct timespec *delay, struct timespec *remaining) {
    CHECK(delay->tv_nsec == 10000000L && !remaining); return 0;
}
static void fake_observer_begin(struct ppp_trial_observer *o) {
    f.baseline++; f.host++; memset(o, 0, sizeof(*o));
    if (f.scenario == BASELINE_ERROR) o->snapshot_error = 1;
}
static void fake_observer_child(struct ppp_trial_observer *o, pid_t pid) {
    CHECK(pid == 789 && !f.child_poll && f.slices == 1); f.capture++; f.host++;
    o->identity = f.scenario != CAPTURE_ERROR;
}
static void fake_observer_sample(struct ppp_trial_observer *o) {
    CHECK(f.capture == 1 && !f.child.reaped && !f.child.ownership_lost); f.samples++; f.host++;
    o->current_valid = 1; o->attributed = 1;
    if (f.scenario == ATTRIBUTION_ERROR) o->ambiguous = 1;
    if (f.scenario == GATE_SIGNAL && f.net_init) f.status.signal_number = SIGTERM;
}
static int fake_observer_current(const struct ppp_trial_observer *o, struct ppp_trial_interface *i) {
    if (o->ambiguous || !o->current_valid) return -1;
    *i = (struct ppp_trial_interface){.name = "ppp-fixture", .index = 7}; return 0;
}
static void fake_observer_finish(struct ppp_trial_observer *o) {
    (void)o; CHECK(!f.starts || f.released || f.session_release);
}
static int fake_observer_result(const struct ppp_trial_observer *o) { (void)o; return 0; }
static int fake_observer_diagnostic(const struct ppp_trial_observer *o, char *b, size_t n) {
    (void)o; return snprintf(b, n, "attribution=fixture service-state=removed kernel-interface=removed");
}
static void fake_observer_dispose(struct ppp_trial_observer *o) { (void)o; }
static int fake_session_start(const struct ppp_session_request *r, const struct ppp_metadata_backend *b,
                              struct ppp_session **s) {
    CHECK(r->service == 1 && r->registry_id == 42 && r->device && r->config && b->context == &f);
    CHECK(r->budgets.drain_ms == 3000 && r->budgets.start_ms == 5000 && r->budgets.connect_ms == 10000);
    CHECK(!*s); f.host++;
    if (f.scenario == SESSION_REJECT) return -1;
    *s = (struct ppp_session *)&f; f.starts++; return 0;
}
static int fake_session_stop(struct ppp_session *s) { CHECK(s == (struct ppp_session *)&f); f.stop = 1; return 0; }
static int fake_session_step(struct ppp_session *s) {
    CHECK(s == (struct ppp_session *)&f && f.stop); return ++f.session_steps >= 3;
}
static int fake_session_release(struct ppp_session **s) {
    CHECK(*s == (struct ppp_session *)&f && f.session_steps >= 3); *s = NULL; f.session_release++; return 0;
}
static int fake_runner_begin(struct ppp_session **s, struct ppp_runner **r) {
    CHECK(*s == (struct ppp_session *)&f && !*r);
    if (f.scenario == RUNNER_REJECT) return -1;
    *s = NULL; *r = (struct ppp_runner *)&f; return 0;
}
static int fake_runner_observe(struct ppp_runner *r, struct ppp_runner_status *s, struct ppp_session_observation *o) {
    CHECK(r == (struct ppp_runner *)&f);
    /* The observation after restore fails and must not overwrite the copies. */
    if (f.scenario == FINAL_OBSERVE_ERROR && f.restores) return -1;
    *s = f.status; *o = f.child; return 0;
}
static void fake_close(void) {
    f.kill = 1; f.quiet = 1;
    f.child.term_signals = f.term; f.child.kill_signals = f.kill;
    f.child.reaped = !f.child.ownership_lost;
    f.child.stdout_eof = f.child.stderr_eof = f.child.exec_done = 1;
    f.child.peer_quiet = f.child.peer_closed = 1;
    f.child.signaled = 1; f.child.term_signal = 9;
    f.status.session.local_done = 1; f.status.session.state = PPP_SESSION_CLOSED;
    f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR;
    f.status.session.supervisor_valid = 1;
    f.status.session.supervisor_phase = PPP_CLOSED;
    /* An earlier supervisor error stays; otherwise the expected DRAIN timeout.
     * CLEAN_CLOSE models a CLOSED supervisor with no error at all. */
    if (f.scenario == CLEAN_CLOSE) f.status.session.error = PPP_SESSION_ERROR_NONE;
    else if (!f.status.session.supervisor_error) f.status.session.supervisor_error = PPP_ERROR_TIMEOUT;
    f.closed = 1;
}
static int fake_runner_slice(struct ppp_runner *r, const volatile sig_atomic_t *stop,
                             const uint32_t *wait, struct ppp_runner_status *s) {
    CHECK(r == (struct ppp_runner *)&f && !wait); f.slices++; f.host++;
    if (f.slices == 1) {
        f.child = (struct ppp_session_observation){.pid = 789, .started = 1, .exec_done = 1};
        f.status.session.state = PPP_SESSION_RUNNING;
        f.status.session.supervisor_valid = 1; f.status.session.supervisor_phase = PPP_START;
    } else { CHECK(f.capture == 1); f.child_poll++; }
    if (f.slices == 2 && !f.closed) {
        f.status.session.supervisor_phase = PPP_CONNECT;
        f.child.connect_attempted = f.child.output_observed = 1;
    }
    /* A stale copy with negotiation counts must still print unproven. */
    if (f.scenario == FINAL_OBSERVE_ERROR && f.slices == 2) {
        f.child.negotiation.lcp = PPP_NEGO_LCP_OPENED;
        f.child.negotiation.frames[PPP_NEGO_PEER] = 3; f.child.negotiation.frames[PPP_NEGO_LOCAL] = 4;
        f.child.negotiation.codes[PPP_NEGO_PEER][PPP_NEGO_IPCP][1] = 2;
        f.child.negotiation.codes[PPP_NEGO_LOCAL][PPP_NEGO_IPV4][0] = 5;
        f.child.negotiation.trace.length = 1;
        f.child.negotiation.trace.events[0] = (struct ppp_nego_event){.count = 1, .protocol = PPP_NEGO_LCP, .code = 1};
    }
    /* Worst-case trace text: every event at its longest label, all frames
     * dropped after it, and an out-of-range length/protocol. */
    if (f.scenario == TRACE_OVERFLOW && f.slices == 2) {
        struct ppp_nego_trace *t = &f.child.negotiation.trace;
        for (unsigned i = 0; i < PPP_NEGO_TRACE_EVENTS; i++)
            t->events[i] = (struct ppp_nego_event){.count = UINT32_MAX, .direction = PPP_NEGO_PEER,
                .protocol = PPP_NEGO_IPV6CP, .code = 255};
        t->events[0].protocol = 99; t->events[1].direction = PPP_NEGO_LOCAL;
        t->length = 200; t->dropped = UINT32_MAX;
    }
    /* The modem answers the hook with a numbered CME error before any CONNECT. */
    if (f.scenario == MODEM_RESULT && f.slices == 3) {
        f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR;
        f.status.session.supervisor_error = PPP_ERROR_CORE;
        f.status.session.supervisor_phase = PPP_CLEANUP;
        f.child.stream_state = OFFLINE_RECOVERY_UNKNOWN; f.child.stream_error = OFFLINE_ERROR_RESULT;
        f.child.echo_seen = 1; f.child.modem_result = OFFLINE_MODEM_CME;
        f.child.modem_code = 30; f.child.modem_code_valid = 1;
        f.child.control_bytes = 40; f.child.usb_input_bytes = 40; f.child.child_output_bytes = 22;
    }
    /* A USB IN timeout in SESSION: only its fixed label reaches the output. */
    if (f.scenario == USB_ERROR && f.slices == 3) {
        f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR;
        f.status.session.supervisor_error = PPP_ERROR_USB;
        f.status.session.supervisor_phase = PPP_CLEANUP;
        f.child.stream_state = OFFLINE_DATA_STREAM; f.child.stream_error = OFFLINE_ERROR_IO;
        f.child.echo_seen = 1; f.child.control_bytes = 39;
        f.child.usb_input_bytes = 260; f.child.child_output_bytes = 160;
        f.child.usb_error_kind = PPP_USB_ERROR_TIMEOUT;
        f.child.usb_error_site = PPP_USB_SITE_COMPLETION;
        /* Passive negotiation summary: only its labels and counts are printed. */
        f.child.negotiation = (struct ppp_nego_summary){.frames = {7, 9}, .fcs_errors = {1, 2},
            .too_long = {0, 1}, .lcp = PPP_NEGO_LCP_OPENED, .auth = PPP_NEGO_AUTH_CHAP,
            .auth_result = PPP_NEGO_RESULT_PENDING, .ipcp = PPP_NEGO_NCP_NAK,
            .ipv6cp = PPP_NEGO_NCP_CONFIGURING, .terminate = PPP_NEGO_TERMINATE_LOCAL,
            .codes[PPP_NEGO_PEER][PPP_NEGO_IPCP] = {[1] = 2, [2] = 1},
            .codes[PPP_NEGO_LOCAL][PPP_NEGO_IPCP] = {[1] = 3, [5] = 1},
            .codes[PPP_NEGO_PEER][PPP_NEGO_LCP] = {[1] = 4, [5] = 1, [9] = 6},
            .codes[PPP_NEGO_LOCAL][PPP_NEGO_LCP] = {[1] = 5, [8] = 1, [9] = 2},
            .codes[PPP_NEGO_PEER][PPP_NEGO_IPV4] = {[0] = 3},
            .codes[PPP_NEGO_LOCAL][PPP_NEGO_IPV4] = {[0] = 8},
            /* Order record: control events one each, data folded with a count. */
            .trace = {.length = 9, .gap = 9, .events = {
                {1, PPP_NEGO_PEER, PPP_NEGO_LCP, 1}, {1, PPP_NEGO_LOCAL, PPP_NEGO_LCP, 1},
                {1, PPP_NEGO_LOCAL, PPP_NEGO_LCP, 2}, {1, PPP_NEGO_PEER, PPP_NEGO_LCP, 2},
                {1, PPP_NEGO_LOCAL, PPP_NEGO_IPCP, 1}, {1, PPP_NEGO_PEER, PPP_NEGO_IPCP, 2},
                {2, PPP_NEGO_LOCAL, PPP_NEGO_IPV4, 0}, {1, PPP_NEGO_PEER, PPP_NEGO_IPV6CP, 1},
                {1, PPP_NEGO_PEER, PPP_NEGO_LCP, 1}}}};
    }
    /* Closed on its own before the first network step. */
    if (f.scenario == EARLY_CLOSED && f.slices == 2) fake_close();
    /* Slice 3 lands while the network is still pending (it needs 3 progress steps). */
    if (f.scenario == SIGNAL_STOP && f.slices == 3) { CHECK(f.net_init && !f.status.session.local_done); f.status.signal_number = SIGINT; }
    if (f.scenario == OWNERSHIP_LOST && f.slices == 3) { CHECK(f.net_init); f.child.ownership_lost = 1; }
    if (!f.closed && (*stop || f.status.signal_number || f.child.ownership_lost)) {
        f.stop = 1; f.status.stop_requested = 1; f.term = 1;
        static unsigned stopped_at;
        if (!f.ended) { f.ended = 1; stopped_at = f.slices; }
        unsigned finish = f.scenario == CALLBACK_WAIT ? 12 : 4;
        if (f.slices - stopped_at >= finish) fake_close();
    }
    /* Cleanup after stop takes wall time; no live clock read happens meanwhile. */
    if (f.scenario == LATE_ERROR && f.ended) f.now += 1000;
    *s = f.status; return f.status.session.local_done;
}
static int fake_runner_restore(struct ppp_runner *r, struct ppp_runner_status *s) {
    CHECK(r == (struct ppp_runner *)&f && f.closed && f.quiet); f.restores++;
    if (f.scenario == RESTORE_FAILURE && f.restores == 1) { s->unrestored_actions = 1; return 1; }
    f.status.unrestored_actions = 0; *s = f.status; return 0;
}
static int fake_runner_release(struct ppp_runner **r) {
    CHECK(*r == (struct ppp_runner *)&f && f.closed && f.quiet && f.restores);
    f.released++;
    if (f.scenario == RELEASE_ERROR) return -1;
    *r = NULL; return 0;
}
static int fake_discovery(discovery_target_callback callback, void *context) {
    f.discovery++; f.host++;
    if (f.scenario == DISCOVERY_ERROR) return DJI_SYSTEM_ERROR;
    struct descriptor_config config = {0};
    IOUSBDeviceInterface *device = NULL;
    return callback(context, 1, &device, 42, &config);
}
static int fake_endpoint(struct in_addr address) {
    uint32_t host = ntohl(address.s_addr);
    return host && host != 0xffffffffu && (host >> 24) != 127 &&
        (host & 0xffff0000u) != 0xa9fe0000u && (host >> 28) < 0xe ? 0 : -1;
}
static int fake_net_init(struct ppp_lte_network *n, struct in_addr endpoint, uint16_t port,
                         uint64_t start, int (*gate)(void *, struct ppp_trial_interface *), void *context) {
    CHECK(endpoint.s_addr && port && start); f.net_init++;
    memset(n, 0, sizeof(*n)); n->gate = gate; n->context = context;
    return 0;
}
/* Pending for two steps, then exact echo. A failed gate is a network failure. */
static int fake_net_step(struct ppp_lte_network *n) {
    struct ppp_trial_interface interface;
    f.net_steps++;
    if (n->gate(n->context, &interface)) { n->reason = "fixture-gate"; return -1; }
    f.net_progress++;
    if (f.scenario == NETWORK_ERROR) { n->reason = "fixture-network-error"; return -1; }
    /* The first progressing step pins the address at the current fixture clock. */
    if (!n->pinned) { n->pinned = 1; n->pinned_ms = f.pinned_ms = f.now; }
    /* The pinned IPv4 then disappears while still in the IP stage. */
    if (f.scenario == ADDRESS_MISSING && f.net_progress == 2) {
        n->failed_stage = n->stage; n->stage = PPP_LTE_FAILED; n->reason = "address-missing";
        n->lost_ms = f.lost_ms = f.now; return -1;
    }
    /* The second progressing step marks the pin settled after two IP-stage resets. */
    if (f.net_progress == 2) { n->stable_ms = f.stable_ms = f.now; n->address_resets = 2; }
    n->sent = f.net_progress >= 2 ? sizeof(PPP_LTE_PROBE) - 1 : 4;
    if (f.net_progress < 3) return 0;
    n->received = sizeof(PPP_LTE_PROBE) - 1;
    n->stage = PPP_LTE_DONE; return 1;
}
static void fake_net_abort(struct ppp_lte_network *n, const char *reason) {
    (void)n; CHECK(reason); f.net_abort++;
}
/* The default table holds the bare symbol, not a call-site macro. */
int trial_preflight(void *context, struct trial_preflight *p) { return fake_preflight(context, p); }
#define trial_identity fake_identity
#define trial_metadata fake_metadata
#define trial_executable fake_executable
#define trial_preflight(...) fake_preflight(__VA_ARGS__)
#define clock_gettime fake_clock
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
#define ppp_lte_endpoint_allowed fake_endpoint
#define ppp_lte_network_init fake_net_init
#define ppp_lte_network_step fake_net_step
#define ppp_lte_network_abort fake_net_abort
#define main once_main
#include "../src/ppp_lte_once.c"
#undef main

static int echo_expected(enum scenario s) {
    return s == EXACT || s == CALLBACK_WAIT || s == RESTORE_FAILURE || s == RELEASE_ERROR ||
        s == LATE_ERROR || s == DIAG_CLOCK_ERROR || s == DIAG_CLOCK_BACK ||
        s == FINAL_OBSERVE_ERROR || s == CLEAN_CLOSE || s == TRACE_OVERFLOW;
}
/* Fixed reason/label each scenario must print; NULL skips the check. */
static const char *expected_text(enum scenario s) {
    switch (s) {
    case EXACT: case CALLBACK_WAIT: case RESTORE_FAILURE: case LATE_ERROR:
    case DIAG_CLOCK_ERROR: case DIAG_CLOCK_BACK: case FINAL_OBSERVE_ERROR: case TRACE_OVERFLOW:
        return "reason=echo-observed payload-sent=14 payload-received=14 local=closed"
            " session-error=supervisor-timeout cleanup=none clock=ok";
    case ROOT_BLOCK: return "execution=blocked gate=root-identity";
    case EXEC_BLOCK: return "execution=blocked gate=metadata-error";
    case HOME_BLOCK: return "execution=blocked gate=root-home-preflight";
    case BASELINE_ERROR: return "reason=baseline-unproven payload-sent=0 payload-received=0 local=not-started";
    case CAPTURE_ERROR: return "reason=identity-unproven payload-sent=0";
    case ATTRIBUTION_ERROR: return "reason=interface-unproven payload-sent=0";
    case NETWORK_ERROR: return "app-echo=unproven reason=fixture-network-error";
    case SIGNAL_STOP: case OWNERSHIP_LOST:
        return "app-echo=unproven reason=child-or-clock-unproven payload-sent=4 payload-received=0";
    case CLOCK_ERROR: return "reason=start-clock-error payload-sent=0 payload-received=0 local=closed"
            " session-error=supervisor-timeout cleanup=none clock=error";
    case CLOCK_SIGNAL: return "app-echo=unproven reason=child-or-clock-unproven payload-sent=0";
    case GATE_SIGNAL: return "app-echo=unproven reason=fixture-gate payload-sent=0 payload-received=0";
    case SESSION_REJECT: return "reason=session-rejected payload-sent=0 payload-received=0 local=not-started";
    case RUNNER_REJECT: return "reason=runner-rejected payload-sent=0 payload-received=0 local=not-started";
    case DISCOVERY_ERROR: return "reason=discovery-error payload-sent=0 payload-received=0 local=not-started";
    case EARLY_CLOSED: return "app-echo=unproven reason=session-ended payload-sent=0 payload-received=0 local=closed";
    case RELEASE_ERROR: return "reason=echo-observed payload-sent=14 payload-received=14 local=closed"
            " session-error=supervisor-timeout cleanup=release-error";
    case MODEM_RESULT: return "app-echo=unproven reason=child-or-clock-unproven payload-sent=4"
            " payload-received=0 local=closed session-error=supervisor cleanup=none clock=ok"
            " child=reaped stdout-eof=observed stderr-eof=observed callback-quiet=observed"
            " handlers=restored-or-not-installed term-count=1 kill-count=1"
            " supervisor-error=core phase-reached=connect error-ms=";
    case USB_ERROR: return "app-echo=unproven reason=child-or-clock-unproven payload-sent=4"
            " payload-received=0 local=closed session-error=supervisor cleanup=none clock=ok";
    case CLEAN_CLOSE: return "reason=echo-observed payload-sent=14 payload-received=14 local=closed"
            " session-error=none cleanup=none clock=ok";
    case ADDRESS_MISSING: return "app-echo=unproven reason=address-missing payload-sent=4 payload-received=0";
    }
    return NULL;
}
/* Every new diagnostic after error-ms when no fresh final observation exists. */
#define NEW_UNPROVEN " connect-sent=unproven output-observed=unproven stream=unproven" \
    " stream-error=unproven modem-echo=unproven modem-result=unproven control-bytes=unproven" \
    " child-output-bytes=unproven usb-input-bytes=unproven child-stderr-bytes=unproven" \
    " child-exit=unproven exec=unproven usb-error=unproven usb-error-site=unproven" \
    " remote-termination=unproven"
/* Passive negotiation tokens without a fresh final observation. */
#define NEGO_UNPROVEN " overall=partial ppp-lcp=unproven ppp-auth=unproven ppp-auth-result=unproven" \
    " ppp-ipcp=unproven ppp-ipv6cp=unproven ppp-terminate=unproven ppp-frames-rx=unproven" \
    " ppp-frames-tx=unproven ppp-fcs-errors=unproven ppp-long-frames=unproven "
/* Negotiation code counts without a fresh final observation. */
#define CODES_UNPROVEN "ppp-ipcp-req=unproven ppp-ipcp-ack=unproven ppp-ipcp-term=unproven" \
    " ppp-lcp-req=unproven ppp-lcp-term=unproven ppp-lcp-echo=unproven ppp-lcp-protrej=unproven" \
    " ppp-data=unproven "
/* Network never started: no pin time, no stage, no loss time. */
#define NETWORK_NONE "ip-ready-ms=unproven ip-stable-ms=unproven ip-resets=unproven" \
    " network-stage=none ip-lost-ms=unproven "
static int error_ms_value(const char *text, unsigned long long *value) {
    const char *ms = strstr(text, " error-ms=");
    if (!ms || ms[10] < '0' || ms[10] > '9') return -1;
    char *end; *value = strtoull(ms + 10, &end, 10);
    return *end == ' ' ? 0 : -1;
}
/* Numeric value after key, immediately followed by the given next text. */
static int ms_value(const char *text, const char *key, const char *next, unsigned long long *value) {
    const char *ms = strstr(text, key);
    size_t size = strlen(key);
    if (!ms || ms[size] < '0' || ms[size] > '9') return -1;
    char *end; *value = strtoull(ms + size, &end, 10);
    return !strncmp(end, next, strlen(next)) ? 0 : -1;
}
static void invoke(int argc, char **argv, int expected, int no_host, const char *text_expected) {
    FILE *output = tmpfile(); CHECK(output != NULL); if (!output) return;
    CHECK(lte_once(argc, argv, output, NULL) == expected);
    if (no_host) CHECK(!f.host && !f.starts && !f.net_steps);
    if (f.scenario == RUNNER_REJECT) CHECK(f.starts == 1 && f.session_release == 1 && !f.released && !f.capture);
    else CHECK(!f.starts || (f.released == 1 && f.closed && f.quiet && f.capture == 1));
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    char text[4096] = {0}; (void)fread(text, 1, sizeof(text) - 1, output);
    CHECK(!strstr(text, "192.0.2.1") && !strstr(text, "1234") && !strstr(text, "789") && !strstr(text, "/fixture"));
    if (!no_host && expected == 3) CHECK(!strstr(text, "app-echo=exact-eof") == !echo_expected(f.scenario));
    if (f.scenario == MODEM_RESULT && !no_host)
        CHECK(strstr(text, " connect-sent=yes output-observed=yes stream=recovery-unknown"
            " stream-error=result modem-echo=seen modem-result=cme:30 control-bytes=40"
            " child-output-bytes=22 usb-input-bytes=40 child-stderr-bytes=0 child-exit=signal:9"
            " exec=ended usb-error=none usb-error-site=none remote-termination=unproven"
            " overall=partial ppp-lcp=none ppp-auth=unknown ppp-auth-result=none ppp-ipcp=none"
            " ppp-ipv6cp=none ppp-terminate=none ppp-frames-rx=0 ppp-frames-tx=0"
            " ppp-fcs-errors=0 ppp-long-frames=0 ppp-ipcp-req=0/0 ppp-ipcp-ack=0/0 ppp-ipcp-term=0/0"
            " ppp-lcp-req=0/0 ppp-lcp-term=0/0 ppp-lcp-echo=0/0 ppp-lcp-protrej=0/0 ppp-data=0/0"
            " ip-ready-ms=") != NULL && strstr(text, " ip-lost-ms=none ppp-trace=none attribution=fixture "));
    if (f.scenario == USB_ERROR && !no_host)
        CHECK(strstr(text, " supervisor-error=usb phase-reached=connect error-ms=") &&
              strstr(text, " stream=data-stream stream-error=io modem-echo=seen modem-result=none"
                  " control-bytes=39 child-output-bytes=160 usb-input-bytes=260") &&
              strstr(text, " exec=ended usb-error=timeout usb-error-site=completion"
                  " remote-termination=unproven overall=partial ppp-lcp=opened ppp-auth=chap"
                  " ppp-auth-result=pending ppp-ipcp=nak ppp-ipv6cp=configuring ppp-terminate=local"
                  " ppp-frames-rx=7 ppp-frames-tx=9 ppp-fcs-errors=3 ppp-long-frames=1"
                  " ppp-ipcp-req=2/3 ppp-ipcp-ack=1/0 ppp-ipcp-term=0/1 ppp-lcp-req=4/5"
                  " ppp-lcp-term=1/0 ppp-lcp-echo=6/2 ppp-lcp-protrej=0/1 ppp-data=3/8 ip-ready-ms=") &&
              strstr(text, " ip-lost-ms=none ppp-trace=p.lcp1,l.lcp1,l.lcp2,p.lcp2,l.ipcp1,p.ipcp2,l.ip*2,"
                  "p.ip6cp1,p.lcp1 attribution=fixture "));
    /* Worst case renders in full: clamped length, oth for an unknown protocol,
     * the dropped count last, and no truncation mark. */
    if (f.scenario == TRACE_OVERFLOW) {
        char expected[PPP_NEGO_TRACE_EVENTS * 22 + 64];
        size_t used = (size_t)snprintf(expected, sizeof(expected), " ppp-trace=p.oth*4294967295,l.ip6cp255*4294967295");
        for (unsigned i = 2; i < PPP_NEGO_TRACE_EVENTS; i++)
            used += (size_t)snprintf(expected + used, sizeof(expected) - used, ",p.ip6cp255*4294967295");
        (void)snprintf(expected + used, sizeof(expected) - used, ",+4294967295 attribution=fixture");
        CHECK(used < sizeof(expected) && strstr(text, expected) && !strchr(text, '~'));
        /* The whole line was read: it ends with the observer diagnostic. */
        size_t size = strlen(text);
        CHECK(size > ONCE_TRACE_SIZE && size < sizeof(text) - 1 &&
              !strcmp(text + size - 25, "kernel-interface=removed\n"));
    }
    /* Labels only: no raw IOReturn or other hexadecimal value is printed. */
    CHECK(!strstr(text, "0x"));
    /* error-ms is the first-error diagnostic read minus the start read. */
    unsigned long long ms = 0;
    int has_ms = !error_ms_value(text, &ms);
    if (!no_host && (f.scenario == EXACT || f.scenario == MODEM_RESULT || f.scenario == USB_ERROR ||
        f.scenario == LATE_ERROR || f.scenario == FINAL_OBSERVE_ERROR))
        CHECK(has_ms && f.start_ms && f.diag_ms > f.start_ms && ms == f.diag_ms - f.start_ms);
    /* Cleanup after stop advanced the clock: not the value frozen at stop. */
    if (f.scenario == LATE_ERROR)
        CHECK(has_ms && f.last_live > f.start_ms && ms >= f.last_live - f.start_ms + 1000);
    /* Diagnostic clock failure or regression: unproven, and the run itself
     * (stop reason, payload counts, clock=ok) is the same as EXACT. */
    if (f.scenario == DIAG_CLOCK_ERROR) CHECK(!f.diag_ms && f.start_ms);
    if (f.scenario == DIAG_CLOCK_BACK)
        CHECK(f.diag_ms && f.diag_ms == f.last_live - 1 && f.diag_ms > f.start_ms);
    if (f.scenario == DIAG_CLOCK_ERROR || f.scenario == DIAG_CLOCK_BACK)
        CHECK(strstr(text, " supervisor-error=timeout phase-reached=connect error-ms=unproven"
            " connect-sent=yes") != NULL);
    if (f.scenario == CLOCK_ERROR) CHECK(strstr(text, " error-ms=unproven ") != NULL);
    /* No observation at all: nothing is asserted as none/no/0. */
    if (f.scenario == BASELINE_ERROR || f.scenario == SESSION_REJECT ||
        f.scenario == RUNNER_REJECT || f.scenario == DISCOVERY_ERROR)
        CHECK(strstr(text, " supervisor-error=unproven phase-reached=unproven error-ms=unproven"
            NEW_UNPROVEN NEGO_UNPROVEN CODES_UNPROVEN NETWORK_NONE "ppp-trace=unproven ") != NULL);
    /* Network never started for the other pre-network stops either. */
    if (f.scenario == CAPTURE_ERROR || f.scenario == ATTRIBUTION_ERROR ||
        f.scenario == CLOCK_ERROR || f.scenario == CLOCK_SIGNAL)
        CHECK(strstr(text, " " NETWORK_NONE) != NULL);
    /* The stale copy still says connect-sent; it must not become the result. */
    if (f.scenario == FINAL_OBSERVE_ERROR)
        CHECK(f.child.connect_attempted && f.child.exec_done &&
              f.child.negotiation.lcp == PPP_NEGO_LCP_OPENED &&
              strstr(text, " supervisor-error=unproven phase-reached=connect error-ms=") &&
              f.child.negotiation.codes[PPP_NEGO_PEER][PPP_NEGO_IPCP][1] == 2 &&
              strstr(text, NEW_UNPROVEN NEGO_UNPROVEN CODES_UNPROVEN "ip-ready-ms=") &&
              f.child.negotiation.trace.length == 1 && strstr(text, " ppp-trace=unproven "));
    /* Pin and settle times are their clocks minus the start read, the reset count
     * is the network's; a finished echo stays done. */
    unsigned long long ready = 0, stable = 0;
    if (!no_host && (f.scenario == EXACT || f.scenario == FINAL_OBSERVE_ERROR || f.scenario == CLEAN_CLOSE))
        CHECK(!ms_value(text, " ip-ready-ms=", " ip-stable-ms=", &ready) && f.pinned_ms >= f.start_ms &&
              ready == f.pinned_ms - f.start_ms &&
              !ms_value(text, " ip-stable-ms=", " ip-resets=2 network-stage=done ", &stable) &&
              f.stable_ms > f.pinned_ms && stable == f.stable_ms - f.start_ms);
    /* The address pinned, then went missing before settling: a number, no settle,
     * no reset, and the failing stage (ip). */
    if (f.scenario == ADDRESS_MISSING)
        CHECK(f.pinned_ms && !f.stable_ms &&
              !ms_value(text, " ip-ready-ms=", " ip-stable-ms=none ip-resets=0 network-stage=ip ", &ready) &&
              ready == f.pinned_ms - f.start_ms);
    /* Loss time is the network's lost_ms minus the start read. */
    if (f.scenario == ADDRESS_MISSING) {
        const char *lost = strstr(text, " network-stage=ip ip-lost-ms=");
        char *end = NULL;
        unsigned long long value = lost ? strtoull(lost + 29, &end, 10) : 0;
        CHECK(lost && lost[29] >= '0' && lost[29] <= '9' && end && *end == ' ' &&
              f.lost_ms > f.pinned_ms && value == f.lost_ms - f.start_ms);
    }
    /* Network ran without losing the pin: none. */
    if (!no_host && (f.scenario == EXACT || f.scenario == NETWORK_ERROR || f.scenario == CLEAN_CLOSE))
        CHECK(strstr(text, " ip-lost-ms=none ") != NULL);
    /* Network failed or stopped before any pin. */
    if (f.scenario == NETWORK_ERROR || f.scenario == GATE_SIGNAL || f.scenario == EARLY_CLOSED)
        CHECK(!f.pinned_ms && strstr(text, " ip-ready-ms=none ip-stable-ms=none ip-resets=0"
            " network-stage=ip ") != NULL);
    if (f.scenario == CLEAN_CLOSE)
        CHECK(!f.diag_ms && strstr(text, " supervisor-error=none phase-reached=connect error-ms=none"
            " connect-sent=yes output-observed=yes stream=control-wait stream-error=none"
            " modem-echo=none modem-result=none control-bytes=0 child-output-bytes=0"
            " usb-input-bytes=0 child-stderr-bytes=0 child-exit=signal:9 exec=ended"
            " usb-error=none usb-error-site=none remote-termination=unproven "));
    /* Every completed run names its pre-error phase; normal cleanup ends in a timeout. */
    if (f.scenario == EXACT && !no_host)
        CHECK(strstr(text, "supervisor-error=timeout phase-reached=connect") &&
              strstr(text, "modem-echo=none modem-result=none"));
    if (text_expected && !strstr(text, text_expected)) {
        failures++; fprintf(stderr, "FAIL lte-once scenario=%d text=%s\n", (int)f.scenario, text);
    }
    checks++;
    CHECK(fclose(output) == 0);
}
int main(void) {
    char *valid[] = {"once", "--lte-once", "--endpoint", "192.0.2.1", "--port", "1234"};
    for (enum scenario s = EXACT; s <= TRACE_OVERFLOW; s++) {
        memset(&f, 0, sizeof(f)); f.scenario = s; f.now = 1000;
        invoke(6, valid, 3, 0, expected_text(s));
        if (s >= ROOT_BLOCK && s <= HOME_BLOCK) CHECK(!f.baseline && !f.discovery && !f.starts);
        if (s == BASELINE_ERROR) CHECK(!f.discovery && !f.starts);
        if (s == CAPTURE_ERROR || s == CLOCK_ERROR || s == ATTRIBUTION_ERROR || s == CLOCK_SIGNAL)
            CHECK(!f.net_init && !f.net_steps);
        /* Interrupted mid-network: no step after the injection, abort reached. */
        if (s == SIGNAL_STOP || s == OWNERSHIP_LOST) CHECK(f.net_progress == 1 && f.net_abort);
        if (s == GATE_SIGNAL) CHECK(f.net_steps == 1 && !f.net_progress && f.net_abort);
        if (s == SESSION_REJECT || s == DISCOVERY_ERROR) CHECK(!f.starts && !f.slices && !f.capture);
        if (s == RUNNER_REJECT) CHECK(!f.slices && f.session_steps == 3);
        if (s == EARLY_CLOSED) CHECK(f.slices == 2 && f.net_init && !f.net_steps && f.net_abort);
        if (s == CALLBACK_WAIT) CHECK(f.slices >= 12);
        if (s == RESTORE_FAILURE) CHECK(f.restores == 2);
        if (s == ADDRESS_MISSING) CHECK(f.net_progress == 2 && f.net_abort);
    }
    char *help[] = {"once", "--help"};
    memset(&f, 0, sizeof(f)); invoke(2, help, 0, 1, NULL);
    const char *bad_ip[] = {"0.0.0.0", "127.1.2.3", "169.254.1.2", "224.0.0.1", "255.255.255.255", "host", "192.000.2.1", "192.0.2.1x", "1.2.3"};
    for (unsigned i = 0; i < sizeof(bad_ip) / sizeof(bad_ip[0]); i++) {
        valid[3] = (char *)bad_ip[i]; memset(&f, 0, sizeof(f)); invoke(6, valid, 2, 1, NULL);
    }
    valid[3] = "192.0.2.1";
    const char *bad_port[] = {"0", "01", "65536", "999999", "-1", "+1", "1x", ""};
    for (unsigned i = 0; i < sizeof(bad_port) / sizeof(bad_port[0]); i++) {
        valid[5] = (char *)bad_port[i]; memset(&f, 0, sizeof(f)); invoke(6, valid, 2, 1, NULL);
    }
    valid[5] = "1234";
    for (int n = 1; n < 6; n++) { memset(&f, 0, sizeof(f)); invoke(n, valid, 2, 1, NULL); }
    printf("lte-once checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
