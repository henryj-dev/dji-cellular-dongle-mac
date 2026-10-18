#define _DARWIN_C_SOURCE
#include "discovery_internal.h"
#include "discovery.h"
#include "ppp_lte_network.h"
#include "ppp_runner.h"
#include "ppp_trial_preflight.h"
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct once_options { struct in_addr endpoint; uint16_t port; };
struct once_context {
    struct once_options options;
    struct trial_preflight preflight;
    struct ppp_trial_observer observer;
    struct ppp_runner *runner;
    struct ppp_runner_status status;
    struct ppp_session_observation child;
    struct ppp_lte_network network;
    FILE *output;
    volatile sig_atomic_t stop;
    uint64_t start_ms, last_ms;
    int captured, network_started, echo, clock_error, release_error;
    /* Highest supervisor phase seen at a slice boundary before its first error,
     * -1 if none (a transition inside one slice can be missed). tracked: some
     * successful observation had a valid supervisor. error_ms comes from its own
     * clock read when the first error is observed, measured from start_ms. */
    int phase_reached, tracked, error_seen, error_ms_valid, start_valid, final_observed;
    uint64_t error_ms;
    const char *reason;
};
static const char *once_name(const char *const *names, size_t count, unsigned value) {
    return value < count ? names[value] : "unknown";
}
#define ONCE_NAME(names, value) once_name(names, sizeof(names) / sizeof(names[0]), (unsigned)(value))
static const char *const once_supervisor_errors[] = {"none", "child", "usb", "core", "timeout",
    "clock", "close"};
static const char *const once_phases[] = {"start", "connect", "session", "drain"};
static const char *const once_stream_states[] = {"control-wait", "data-stream", "stopping",
    "recovery-unknown", "closed"};
static const char *const once_stream_errors[] = {"none", "argument", "control", "limit", "result",
    "io", "count", "overflow", "timeout", "clock", "close"};
static const char *const once_modem_results[] = {"none", "error", "no-carrier", "no-dialtone",
    "busy", "no-answer", "cme", "cms"};
static const char *const once_usb_errors[] = {"none", "timeout", "aborted", "no-device",
    "not-open", "stall", "overrun", "zero-length", "oversize", "other"};
static const char *const once_usb_sites[] = {"none", "submit", "completion", "write", "connect",
    "abort", "validate"};
/* Passive negotiation labels, indexed by the ppp_negotiation.h enum values. */
static const char *const once_lcp[] = {"none", "configuring", "opened"};
static const char *const once_ncp[] = {"none", "configuring", "nak", "rejected", "opened"};
static const char *const once_auth[] = {"unknown", "none", "pap", "chap", "eap", "other"};
static const char *const once_auth_results[] = {"none", "pending", "success", "failure"};
static const char *const once_terminate[] = {"none", "local", "peer", "both"};
/* Indexed by enum ppp_lte_stage. */
static const char *const once_network_stages[] = {"ip", "open", "connect", "connected", "send",
    "shutdown", "receive", "done", "failed"};
/* peer/local count of one protocol/code cell, or unproven without a fresh observation. */
static void once_codes(char text[24], int fresh, const struct ppp_nego_summary *n,
                       enum ppp_nego_protocol protocol, unsigned code) {
    if (!fresh) { (void)snprintf(text, 24, "unproven"); return; }
    (void)snprintf(text, 24, "%lu/%lu", (unsigned long)n->codes[PPP_NEGO_PEER][protocol][code],
        (unsigned long)n->codes[PPP_NEGO_LOCAL][protocol][code]);
}
/* Indexed by enum ppp_nego_protocol; ip/ip6/oth carry no code number. */
static const char *const once_trace_protocols[] = {"lcp", "pap", "chap", "eap", "ipcp", "ip6cp",
    "ip", "ip6", "oth"};
/* Longest token: "p.ip6cp255*4294967295" (21) plus a comma, per event; then
 * "+4294967295", the truncation mark and NUL. Sized so it never truncates. */
#define ONCE_TRACE_SIZE (PPP_NEGO_TRACE_EVENTS * 22u + 16u)
/* Ordered fixed labels: p/l direction, protocol, code; *N for folded repeats;
 * +N for frames dropped after the trace filled. "~" ends a truncated text. */
static void once_trace(char text[ONCE_TRACE_SIZE], int fresh, const struct ppp_nego_trace *t) {
    if (!fresh) { (void)snprintf(text, ONCE_TRACE_SIZE, "unproven"); return; }
    unsigned length = t->length < PPP_NEGO_TRACE_EVENTS ? t->length : PPP_NEGO_TRACE_EVENTS;
    if (!length && !t->dropped) { (void)snprintf(text, ONCE_TRACE_SIZE, "none"); return; }
    size_t used = 0;
    int truncated = 0;
    text[0] = 0;
    for (unsigned i = 0; i <= length && !truncated; i++) {
        int written;
        if (i == length) {
            if (!t->dropped) break;
            written = snprintf(text + used, ONCE_TRACE_SIZE - used, "%s+%lu", length ? "," : "",
                (unsigned long)t->dropped);
        } else {
            const struct ppp_nego_event *e = &t->events[i];
            int control = e->protocol < PPP_NEGO_IPV4;
            char code[8] = "", count[16] = "";
            if (control) (void)snprintf(code, sizeof(code), "%u", (unsigned)e->code);
            if (e->count > 1) (void)snprintf(count, sizeof(count), "*%lu", (unsigned long)e->count);
            written = snprintf(text + used, ONCE_TRACE_SIZE - used, "%s%s.%s%s%s", i ? "," : "",
                e->direction == PPP_NEGO_PEER ? "p" : "l",
                ONCE_NAME(once_trace_protocols, e->protocol < PPP_NEGO_OTHER ? e->protocol : PPP_NEGO_OTHER),
                code, count);
        }
        if (written < 0 || (size_t)written >= ONCE_TRACE_SIZE - used - 1) truncated = 1;
        else used += (size_t)written;
    }
    /* One byte stays free for the mark; only reached if the bound above is wrong. */
    if (truncated) { text[used] = '~'; text[used + 1] = 0; }
}
static int once_now(uint64_t *milliseconds);
static void once_track(struct once_context *c) {
    const struct ppp_session_status *s = &c->status.session;
    if (!s->supervisor_valid) return;
    c->tracked = 1;
    if (s->supervisor_error == PPP_ERROR_NONE) {
        if (s->supervisor_phase <= PPP_DRAIN && (int)s->supervisor_phase > c->phase_reached)
            c->phase_reached = (int)s->supervisor_phase;
    } else if (!c->error_seen) {
        c->error_seen = 1;
        /* last_ms freezes at stop/done, so read the clock separately. This read
         * never feeds last_ms, clock_error, stop or any budget; a failed or
         * backward read leaves error-ms unproven. */
        uint64_t now;
        if (c->start_valid && !once_now(&now) && now >= c->start_ms && now >= c->last_ms) {
            c->error_ms = now - c->start_ms; c->error_ms_valid = 1;
        }
    }
}
static const char *once_session_error(const struct ppp_session_status *s) {
    static const char *const names[] = {"none", "metadata", "acquire", "bind", "start",
        "supervisor", "dispose"};
    if (s->error == PPP_SESSION_ERROR_SUPERVISOR && s->supervisor_valid &&
        s->supervisor_error == PPP_ERROR_TIMEOUT) return "supervisor-timeout";
    return (unsigned)s->error < sizeof(names) / sizeof(names[0]) ? names[s->error] : "unknown";
}
static const char *once_cleanup(const struct once_context *c) {
    if (c->release_error) return "release-error";
    if (c->status.error) return "runner-error";
    if (c->status.session.cleanup_error) return "session-cleanup-error";
    return "none";
}
/* Deliberately one spelling/order: no optional overrides, payload or default. */
static int once_parse(int argc, char **argv, struct once_options *options) {
    if (argc == 2 && !strcmp(argv[1], "--help")) return 1;
    if (argc != 6 || strcmp(argv[1], "--lte-once") || strcmp(argv[2], "--endpoint") ||
        strcmp(argv[4], "--port")) return -1;
    size_t length = strnlen(argv[3], INET_ADDRSTRLEN);
    if (!length || length >= INET_ADDRSTRLEN || inet_pton(AF_INET, argv[3], &options->endpoint) != 1 ||
        ppp_lte_endpoint_allowed(options->endpoint)) return -1;
    char canonical[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &options->endpoint, canonical, sizeof(canonical)) || strcmp(argv[3], canonical))
        return -1;
    length = strnlen(argv[5], 6);
    if (!length || length > 5 || argv[5][0] == '0') return -1;
    unsigned port = 0;
    for (size_t i = 0; i < length; i++) {
        if (argv[5][i] < '0' || argv[5][i] > '9') return -1;
        port = port * 10 + (unsigned)(argv[5][i] - '0');
    }
    if (!port || port > UINT16_MAX) return -1;
    options->port = (uint16_t)port;
    return 0;
}
static int once_now(uint64_t *milliseconds) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || (uint64_t)ts.tv_sec > (UINT64_MAX - 999) / 1000) return -1;
    *milliseconds = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return 0;
}
static int once_flags(struct once_context *c) {
    return ppp_runner_observe(c->runner, &c->status, &c->child) || c->stop ||
        c->status.stop_requested || c->status.signal_number || c->status.error ||
        c->status.session.stop_requested || c->status.session.error ||
        !c->child.started || c->child.reaped || c->child.ownership_lost ||
        c->child.stop_requested || !c->captured || !c->observer.identity ? -1 : 0;
}
static int once_live(struct once_context *c) {
    if (once_flags(c)) return -1;
    uint64_t now = c->last_ms;
    if (once_now(&now) || now < c->last_ms || now - c->start_ms >= 60000) {
        c->clock_error = 1; return -1;
    }
    c->last_ms = now;
    /* A signal can arrive inside the clock call as well. */
    return once_flags(c);
}
static int once_gate(void *context, struct ppp_trial_interface *interface) {
    struct once_context *c = context;
    if (once_live(c)) return -1;
    /* Never sample a reaped or ownership-lost PID. Recheck flags after CF/proc
     * calls too; a signal can arrive inside any read-only OS boundary. */
    ppp_trial_observer_sample(&c->observer);
    if (once_live(c)) return -1;
    return ppp_trial_observer_current(&c->observer, interface);
}
static void once_stop(struct once_context *c, const char *reason) {
    if (!c->stop) c->reason = reason;
    c->stop = 1;
    if (c->network_started) ppp_lte_network_abort(&c->network, reason);
}
static void once_wait(void) {
    const struct timespec delay = {.tv_nsec = 10000000L};
    (void)nanosleep(&delay, NULL);
}
static int once_target(void *context, io_service_t service, IOUSBDeviceInterface **device,
                        uint64_t registry_id, const struct descriptor_config *config) {
    struct once_context *c = context;
    struct ppp_session *session = NULL;
    struct ppp_session_request request = {.service = service, .device = device,
        .registry_id = registry_id, .config = config, .budgets = {5000, 10000, 60000, 3000},
        .capacity = PPP_SUPERVISOR_BUFFER};
    if (ppp_session_start(&request, &c->preflight.backend, &session)) {
        c->reason = "session-rejected"; return DJI_SYSTEM_ERROR;
    }
    if (ppp_runner_begin(&session, &c->runner)) {
        /* Rejection does not consume the session. No borrow ends during WAIT. */
        c->reason = "runner-rejected";
        (void)ppp_session_stop(session);
        while (ppp_session_step(session) != 1) once_wait();
        (void)ppp_session_release(&session);
        return DJI_SYSTEM_ERROR;
    }
    /* session was consumed; only the runner owns it from here onward. */
    int done = 0;
    while (!done) {
        int slice = ppp_runner_slice(c->runner, &c->stop, NULL, &c->status);
        if (slice < 0) { once_stop(c, "runner-call-error"); once_wait(); continue; }
        /* First start slice returns without child poll. Capture before ANY
         * subsequent runner slice, not after SESSION/exec/CONNECT readiness. */
        if (ppp_runner_observe(c->runner, &c->status, &c->child)) {
            once_stop(c, "observation-error"); continue;
        }
        once_track(c);
        if (c->child.started && !c->captured) {
            c->captured = 1;
            if (c->child.reaped || c->child.ownership_lost) once_stop(c, "start-observation-error");
            else {
                ppp_trial_observer_child(&c->observer, c->child.pid);
                if (!c->observer.identity) once_stop(c, "identity-unproven");
                if (once_now(&c->start_ms) || c->start_ms > UINT64_MAX - 60000) {
                    c->clock_error = 1; once_stop(c, "start-clock-error");
                } else c->start_valid = 1;
                c->last_ms = c->start_ms;
            }
        }
        done = slice == 1;
        if (done) break;
        if (c->stop) continue;
        if (once_live(c)) { once_stop(c, "child-or-clock-unproven"); continue; }
        struct ppp_trial_interface interface;
        if (!c->network_started) {
            if (!once_gate(c, &interface)) {
                c->network_started = 1;
                if (ppp_lte_network_init(&c->network, c->options.endpoint, c->options.port,
                    c->start_ms, once_gate, c)) once_stop(c, "network-init-error");
            } else if (c->clock_error || once_flags(c)) once_stop(c, "child-or-clock-unproven");
            else if (!c->observer.identity || c->observer.ambiguous || c->observer.snapshot_error ||
                c->observer.attributed || c->last_ms - c->start_ms >= 30000)
                once_stop(c, "interface-unproven");
        } else {
            int result = ppp_lte_network_step(&c->network);
            if (result == 1) { c->echo = 1; once_stop(c, "echo-observed"); }
            else if (result < 0) once_stop(c, c->network.reason ? c->network.reason : "network-error");
        }
    }
    if (!c->reason) c->reason = "session-ended";
    /* DONE closes the socket; abort is idempotent for every other path. */
    if (c->network_started) ppp_lte_network_abort(&c->network, c->reason);
    /* No peer_done: the caller cannot assert remote LCP/PDP termination.
     * CLOSED requires the existing TERM/DRAIN deadline/error KILL/reap/quiet path. */
    /* Release requires full restoration; the borrow must outlive the runner. */
    while (ppp_runner_restore(c->runner, &c->status) != 0) once_wait();
    /* A failed final observe leaves c->child stale; diagnostics then stay unproven. */
    c->final_observed = !ppp_runner_observe(c->runner, &c->status, &c->child);
    if (c->final_observed) once_track(c);
    /* Release fails only after CLOSED and full restore; the runner is reported,
     * not retried, and discovery then drops its borrowed references. */
    if (ppp_runner_release(&c->runner)) { c->release_error = 1; return DJI_SYSTEM_ERROR; }
    return DJI_OK;
}
/* Private fake-TU entry. NULL selects the same strict shared gates, not a
 * relaxed production config; tests replace every downstream OS entry. */
static int lte_once(int argc, char **argv, FILE *output, const struct trial_backend *backend) {
    struct once_context c = {.output = output, .phase_reached = -1};
    int parsed = once_parse(argc, argv, &c.options);
    if (parsed == 1) {
        return fputs("usage: ppp_lte_once --lte-once --endpoint <numeric-IPv4> --port <1..65535>\n"
            "fixed ASCII echo plus EOF; separate device/LTE execution approval required\n", output) < 0 ? 1 : 0;
    }
    if (parsed < 0) return 2;
    const struct trial_backend defaults = {NULL, trial_identity, trial_metadata, trial_preflight};
    const struct trial_backend *b = backend ? backend : &defaults;
    if (!b->identity || !b->metadata || !b->preflight) return 2;
    const char *gate = "root-identity";
    if (b->identity(b->context)) goto blocked;
    gate = trial_executable(b);
    if (strcmp(gate, "clear")) goto blocked;
    gate = "root-home-preflight";
    if (b->preflight(b->context, &c.preflight) ||
        c.preflight.status != TRIAL_PREFLIGHT_COMPLETE || !c.preflight.metadata.clear ||
        !c.preflight.backend.real_uid || !c.preflight.backend.home || !c.preflight.backend.metadata)
        goto blocked;
    ppp_trial_observer_begin(&c.observer);
    int discovery = DJI_SYSTEM_ERROR;
    if (c.observer.snapshot_error) c.reason = "baseline-unproven";
    else discovery = discovery_with_one_target(once_target, &c);
    /* The target sets its own reason; NULL means it never ran. */
    if (!c.reason) c.reason = discovery == DJI_OK ? "target-not-run" : "discovery-error";
    ppp_trial_observer_finish(&c.observer);
    int observed = ppp_trial_observer_result(&c.observer);
    char diagnostic[1024];
    int length = ppp_trial_observer_diagnostic(&c.observer, diagnostic, sizeof(diagnostic));
    ppp_trial_observer_dispose(&c.observer);
    if (length < 0 || (size_t)length >= sizeof(diagnostic)) return 1;
    /* Fixed labels and counts only: no modem text, PPP bytes or identifiers. */
    const struct ppp_session_observation *k = &c.child;
    /* New diagnostics need a successful final observation of a valid supervisor
     * (child fields: a started child too). Anything else, including rejection
     * paths and a stale c.child, prints unproven instead of none/no/0. */
    int fresh = c.final_observed && c.status.session.supervisor_valid, child_valid = fresh && k->started;
    char error_ms[24] = "unproven", modem[24] = "unproven", exit_text[24] = "unproven";
    char control_bytes[24] = "unproven", output_bytes[24] = "unproven", usb_bytes[24] = "unproven";
    char stderr_bytes[24] = "unproven";
    char frames_rx[24] = "unproven", frames_tx[24] = "unproven", fcs_errors[24] = "unproven";
    char long_frames[24] = "unproven";
    const struct ppp_nego_summary *n = &k->negotiation;
    if (c.error_seen ? c.error_ms_valid : fresh) {
        if (c.error_seen) (void)snprintf(error_ms, sizeof(error_ms), "%llu", (unsigned long long)c.error_ms);
        else (void)snprintf(error_ms, sizeof(error_ms), "none");
    }
    /* The pre-error phase is fixed once an error is seen; without one, only a
     * fresh final observation proves no later pre-error phase was missed. */
    const char *phase = !c.tracked || (!c.error_seen && !fresh) ? "unproven" :
        c.phase_reached < 0 ? "none" : ONCE_NAME(once_phases, c.phase_reached);
    if (fresh) {
        if (k->modem_code_valid)
            (void)snprintf(modem, sizeof(modem), "%s:%u", ONCE_NAME(once_modem_results, k->modem_result), k->modem_code);
        else (void)snprintf(modem, sizeof(modem), "%s", ONCE_NAME(once_modem_results, k->modem_result));
        (void)snprintf(control_bytes, sizeof(control_bytes), "%llu", (unsigned long long)k->control_bytes);
        (void)snprintf(usb_bytes, sizeof(usb_bytes), "%llu", (unsigned long long)k->usb_input_bytes);
        (void)snprintf(frames_rx, sizeof(frames_rx), "%llu", (unsigned long long)n->frames[PPP_NEGO_PEER]);
        (void)snprintf(frames_tx, sizeof(frames_tx), "%llu", (unsigned long long)n->frames[PPP_NEGO_LOCAL]);
        (void)snprintf(fcs_errors, sizeof(fcs_errors), "%llu", (unsigned long long)n->fcs_errors[PPP_NEGO_PEER] +
            (unsigned long long)n->fcs_errors[PPP_NEGO_LOCAL]);
        (void)snprintf(long_frames, sizeof(long_frames), "%llu", (unsigned long long)n->too_long[PPP_NEGO_PEER] +
            (unsigned long long)n->too_long[PPP_NEGO_LOCAL]);
    }
    if (child_valid) {
        (void)snprintf(output_bytes, sizeof(output_bytes), "%llu", (unsigned long long)k->child_output_bytes);
        (void)snprintf(stderr_bytes, sizeof(stderr_bytes), "%llu", (unsigned long long)k->stderr_bytes);
        if (k->exited) (void)snprintf(exit_text, sizeof(exit_text), "code:%d", k->exit_code);
        else if (k->signaled) (void)snprintf(exit_text, sizeof(exit_text), "signal:%d", k->term_signal);
    }
    /* Negotiation code counts (RFC 1661 codes; IPv4 data has no code, cell 0). */
    char ipcp_req[24], ipcp_ack[24], ipcp_term[24], lcp_req[24], lcp_term[24];
    char lcp_echo[24], lcp_protrej[24], ipv4_data[24];
    once_codes(ipcp_req, fresh, n, PPP_NEGO_IPCP, 1); once_codes(ipcp_ack, fresh, n, PPP_NEGO_IPCP, 2);
    once_codes(ipcp_term, fresh, n, PPP_NEGO_IPCP, 5); once_codes(lcp_req, fresh, n, PPP_NEGO_LCP, 1);
    once_codes(lcp_term, fresh, n, PPP_NEGO_LCP, 5); once_codes(lcp_echo, fresh, n, PPP_NEGO_LCP, 9);
    once_codes(lcp_protrej, fresh, n, PPP_NEGO_LCP, 8); once_codes(ipv4_data, fresh, n, PPP_NEGO_IPV4, 0);
    /* Network diagnostics: first pin time from the child start, and the stage of
     * the first network failure (else the current stage). Never a decision input. */
    char ip_ready[24] = "unproven";
    const struct ppp_lte_network *w = &c.network;
    if (c.network_started && c.start_valid) {
        if (!w->pinned_ms) (void)snprintf(ip_ready, sizeof(ip_ready), "none");
        else if (w->pinned_ms >= c.start_ms)
            (void)snprintf(ip_ready, sizeof(ip_ready), "%llu", (unsigned long long)(w->pinned_ms - c.start_ms));
    }
    /* When the last pin passed the settle window, and IP-stage unpins. */
    char ip_stable[24] = "unproven", ip_resets[24] = "unproven";
    if (c.network_started && c.start_valid) {
        if (!w->stable_ms) (void)snprintf(ip_stable, sizeof(ip_stable), "none");
        else if (w->stable_ms >= c.start_ms)
            (void)snprintf(ip_stable, sizeof(ip_stable), "%llu", (unsigned long long)(w->stable_ms - c.start_ms));
    }
    if (c.network_started) (void)snprintf(ip_resets, sizeof(ip_resets), "%u", w->address_resets);
    const char *network_stage = !c.network_started ? "none" :
        ONCE_NAME(once_network_stages, w->stage == PPP_LTE_FAILED ? w->failed_stage : w->stage);
    /* Pinned address went missing/changed: its last_ms from the child start. */
    char ip_lost[24] = "unproven";
    if (c.network_started && c.start_valid) {
        if (!w->lost_ms) (void)snprintf(ip_lost, sizeof(ip_lost), "none");
        else if (w->lost_ms >= c.start_ms)
            (void)snprintf(ip_lost, sizeof(ip_lost), "%llu", (unsigned long long)(w->lost_ms - c.start_ms));
    }
    char trace[ONCE_TRACE_SIZE];
    once_trace(trace, fresh, &n->trace);
    if (fprintf(output, "app-echo=%s reason=%s payload-sent=%zu payload-received=%zu"
        " local=%s session-error=%s cleanup=%s clock=%s child=%s stdout-eof=%s stderr-eof=%s"
        " callback-quiet=%s handlers=%s"
        " term-count=%u kill-count=%u"
        " supervisor-error=%s phase-reached=%s error-ms=%s connect-sent=%s output-observed=%s"
        " stream=%s stream-error=%s modem-echo=%s modem-result=%s control-bytes=%s"
        " child-output-bytes=%s usb-input-bytes=%s child-stderr-bytes=%s child-exit=%s exec=%s"
        " usb-error=%s usb-error-site=%s"
        " remote-termination=unproven overall=partial"
        " ppp-lcp=%s ppp-auth=%s ppp-auth-result=%s ppp-ipcp=%s ppp-ipv6cp=%s ppp-terminate=%s"
        " ppp-frames-rx=%s ppp-frames-tx=%s ppp-fcs-errors=%s ppp-long-frames=%s"
        " ppp-ipcp-req=%s ppp-ipcp-ack=%s ppp-ipcp-term=%s ppp-lcp-req=%s ppp-lcp-term=%s"
        " ppp-lcp-echo=%s ppp-lcp-protrej=%s ppp-data=%s ip-ready-ms=%s ip-stable-ms=%s"
        " ip-resets=%s network-stage=%s"
        " ip-lost-ms=%s ppp-trace=%s %s\n",
        c.echo ? "exact-eof" : "unproven", c.reason,
        c.network_started ? c.network.sent : 0, c.network_started ? c.network.received : 0,
        c.status.session.local_done ? "closed" : "not-started",
        once_session_error(&c.status.session), once_cleanup(&c),
        c.clock_error ? "error" : "ok",
        c.child.reaped ? "reaped" : c.child.ownership_lost ? "ownership-lost" : "unproven",
        c.child.stdout_eof ? "observed" : "unproven", c.child.stderr_eof ? "observed" : "unproven",
        c.child.peer_quiet ? "observed" : "unproven",
        c.status.unrestored_actions ? "unrestored" : "restored-or-not-installed",
        c.child.term_signals, c.child.kill_signals,
        fresh ? ONCE_NAME(once_supervisor_errors, c.status.session.supervisor_error) : "unproven",
        phase, error_ms,
        !fresh ? "unproven" : k->connect_attempted ? "yes" : "no",
        !fresh ? "unproven" : k->output_observed ? "yes" : "no",
        fresh ? ONCE_NAME(once_stream_states, k->stream_state) : "unproven",
        fresh ? ONCE_NAME(once_stream_errors, k->stream_error) : "unproven",
        !fresh ? "unproven" : k->echo_seen ? "seen" : "none", modem,
        control_bytes, output_bytes, usb_bytes, stderr_bytes, exit_text,
        /* Exec pipe closed without an error report: not proof pppd ran. */
        !child_valid ? "unproven" : k->exec_error ? "error" : k->exec_done ? "ended" : "unproven",
        /* Fixed USB labels only, never the raw IOReturn. */
        fresh ? ONCE_NAME(once_usb_errors, k->usb_error_kind) : "unproven",
        fresh ? ONCE_NAME(once_usb_sites, k->usb_error_site) : "unproven",
        /* Passive frame classification: counts and labels only, never bytes. */
        fresh ? ONCE_NAME(once_lcp, n->lcp) : "unproven",
        fresh ? ONCE_NAME(once_auth, n->auth) : "unproven",
        fresh ? ONCE_NAME(once_auth_results, n->auth_result) : "unproven",
        fresh ? ONCE_NAME(once_ncp, n->ipcp) : "unproven",
        fresh ? ONCE_NAME(once_ncp, n->ipv6cp) : "unproven",
        fresh ? ONCE_NAME(once_terminate, n->terminate) : "unproven",
        frames_rx, frames_tx, fcs_errors, long_frames,
        ipcp_req, ipcp_ack, ipcp_term, lcp_req, lcp_term, lcp_echo, lcp_protrej, ipv4_data,
        ip_ready, ip_stable, ip_resets, network_stage, ip_lost, trace, diagnostic) < 0 || fflush(output)) return 1;
    /* Application success never promotes unimplemented remote termination or
     * ownership loss/observation errors to a fully successful trial. */
    (void)discovery; (void)observed;
    return 3;
blocked:
    if (fprintf(output, "execution=blocked gate=%s app-echo=unproven local=not-started overall=partial\n", gate) < 0 ||
        fflush(output)) return 1;
    return 3;
}
int main(int argc, char **argv) { return lte_once(argc, argv, stdout, NULL); }
