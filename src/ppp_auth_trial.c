#define _DARWIN_C_SOURCE
#include "ppp_auth_probe.h"
#include "ppp_child.h"
#include "ppp_launch.h"
#include "ppp_trial_observer.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ppp_trial_preflight.h"
static volatile sig_atomic_t trial_interrupted;
static void trial_signal(int number) { trial_interrupted = number; }
static int trial_now(uint64_t *value) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || (uint64_t)ts.tv_sec > (UINT64_MAX - 999) / 1000) return -1;
    *value = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return 0;
}
static void trial_wait(void) {
    /* No unbounded EINTR retry; signal flag is checked in normal context. */
    const struct timespec delay = {.tv_nsec = 10000000L};
    (void)nanosleep(&delay, NULL);
}
struct trial_result {
    struct ppp_child child;
    struct ppp_auth_probe probe;
    int handlers_restored, status_only, unresolved, clock_error, started;
};
static void trial_run(struct trial_result *r, struct ppp_trial_observer *observer) {
    *r = (struct trial_result){0};
    ppp_child_init(&r->child); ppp_auth_probe_init(&r->probe);
    struct sigaction old[2], action = {.sa_handler = trial_signal};
    const int signals[] = {SIGINT, SIGTERM};
    unsigned installed = 0;
    int stopping = 0, kill_attempted = 0;
    uint64_t start = 0, last = 0, stop = 0, killed = 0;
    sigemptyset(&action.sa_mask);
    trial_interrupted = 0;
    for (; installed < 2; installed++) {
        if (sigaction(signals[installed], &action, &old[installed])) {
            ppp_auth_probe_fail(&r->probe, "handler-install"); goto restore;
        }
    }
    if (trial_now(&start)) { ppp_auth_probe_fail(&r->probe, "clock-error"); goto restore; }
    last = start;
    if (trial_interrupted) { ppp_auth_probe_fail(&r->probe, "signal"); goto restore; }
    struct ppp_launch_policy policy;
    if (ppp_launch_prepare(&policy) || ppp_child_start(&r->child, policy.path,
        policy.argv, policy.envp, policy.cwd)) {
        ppp_auth_probe_fail(&r->probe, "spawn-error"); goto restore;
    }
    r->started = 1;
    /* No waitpid/poll can run before the process start identity is captured. */
    ppp_trial_observer_child(observer, r->child.pid);
    for (;;) {
        struct ppp_child *c = &r->child;
        if (!c->reaped && !c->ownership_lost) ppp_trial_observer_sample(observer);
        size_t budget = PPP_AUTH_SLICE;
        if (!r->status_only && c->stderr_bytes > PPP_AUTH_BYTE_MAX - PPP_AUTH_SLICE) {
            r->status_only = 1;
            ppp_auth_probe_fail(&r->probe, "stderr-reserve-limit");
        }
        uint64_t before = c->stderr_bytes;
        int poll_error = r->status_only ? ppp_child_poll_status(c) : ppp_child_poll(c);
        if (c->stderr_bytes < before || c->stderr_bytes - before > budget) {
            ppp_auth_probe_fail(&r->probe, "stderr-accounting"); r->status_only = 1;
        } else budget -= (size_t)(c->stderr_bytes - before);
        if (poll_error) ppp_auth_probe_fail(&r->probe, c->exec_error ? "exec-error" : "child-error");
        if (trial_interrupted) ppp_auth_probe_fail(&r->probe, "signal");
        uint64_t now = last;
        if (trial_now(&now) || now < last) {
            r->clock_error = 1; ppp_auth_probe_fail(&r->probe, "clock-error");
        } else last = now;
        if (!stopping && now - start >= 20000) ppp_auth_probe_fail(&r->probe, "probe-timeout");
        if (!stopping && (r->probe.failed || r->probe.rejects == 5 || c->reaped || c->ownership_lost)) {
            stopping = 1; stop = now;
            ppp_child_end_input(c);
            /* Signal API performs status-only ownership check before kill. */
            if (!c->reaped && !c->ownership_lost) (void)ppp_child_signal(c, SIGTERM);
        }
        if (stopping && !c->reaped && !c->ownership_lost && !kill_attempted &&
            (r->clock_error || now - stop >= 3000)) {
            kill_attempted = 1; killed = now;
            (void)ppp_child_signal(c, SIGKILL);
        }
        /* Drain only within the original lifetime budgets. Reserve failure means
         * status-only from here, including cleanup: no further stderr/stdout. */
        if (!r->status_only && !r->clock_error && c->exec_done && !c->exec_error &&
            !c->stdout_eof && budget) {
            size_t remaining = (size_t)(PPP_AUTH_BYTE_MAX - r->probe.received);
            if (!remaining) ppp_auth_probe_fail(&r->probe, "stdout-limit");
            else {
                unsigned char bytes[PPP_AUTH_SLICE];
                size_t capacity = budget < remaining ? budget : remaining;
                struct offline_io io = ppp_child_read(c, bytes, capacity);
                if (io.code == OFFLINE_IO_OK) {
                    if (!io.count || io.count > capacity) ppp_auth_probe_fail(&r->probe, "read-count");
                    else {
                        budget -= io.count;
                        if (!stopping) (void)ppp_auth_probe_input(&r->probe, bytes, io.count);
                        else r->probe.received += io.count;
                    }
                } else if (io.code == OFFLINE_IO_ERROR) ppp_auth_probe_fail(&r->probe, "stdout-error");
                else if (io.code == OFFLINE_IO_EOF && r->probe.rejects != 5)
                    ppp_auth_probe_fail(&r->probe, "stdout-eof");
            }
        }
        if (trial_interrupted) ppp_auth_probe_fail(&r->probe, "signal");
        if (!stopping && !r->probe.failed && c->exec_done && !c->exec_error && budget) {
            size_t length;
            const unsigned char *data = ppp_auth_probe_output(&r->probe, &length);
            if (data) {
                if (length > budget) length = budget;
                size_t remaining = (size_t)(PPP_AUTH_BYTE_MAX - r->probe.sent);
                if (length > remaining) length = remaining;
                if (!length) ppp_auth_probe_fail(&r->probe, "stdin-limit");
                else {
                    struct offline_io io = ppp_child_write(c, data, length);
                    if (io.code == OFFLINE_IO_OK) {
                        if (!io.count || io.count > length) ppp_auth_probe_fail(&r->probe, "write-count");
                        else (void)ppp_auth_probe_written(&r->probe, io.count);
                    } else if (io.code == OFFLINE_IO_ERROR || io.code == OFFLINE_IO_EOF)
                        ppp_auth_probe_fail(&r->probe, "stdin-error");
                }
            }
        }
        if ((c->reaped || c->ownership_lost) && (r->status_only || r->clock_error ||
            (c->stdout_eof && c->stderr_eof && c->exec_done))) break;
        if (stopping && r->clock_error) {
            /* Clock unavailable: no deadline arithmetic, traffic or endless wait.
             * Two status-only observations after signals, then honest unresolved. */
            for (unsigned attempt = 0; attempt < 2 && !c->reaped && !c->ownership_lost; attempt++) {
                trial_wait();
                (void)ppp_child_poll_status(c);
            }
            break;
        }
        if (stopping && (kill_attempted ? now - killed >= 2000 : now - stop >= 5000)) break;
        trial_wait();
    }
    r->unresolved = !r->child.reaped && !r->child.ownership_lost;
    if (!r->unresolved) (void)ppp_child_close(&r->child);
    /* On unresolved, DO NOT close a live owned child or fabricate EOF. The CLI
     * returns, OS closes fds and may reparent it. No ownership/resource promise
     * survives return. transport-abandoned is reported separately below. */
restore:
    r->handlers_restored = 1;
    while (installed) {
        installed--;
        if (sigaction(signals[installed], &old[installed], NULL)) r->handlers_restored = 0;
    }
}
static int auth_trial(int argc, char **argv, FILE *output, const struct trial_backend *backend) {
    static const char usage[] =
        "사용법: ppp_auth_trial --help | --local-auth-once\n"
        "동글 없는 단발 인증 거부 관측. 임시 PPP/시스템 상태 변경 가능.\n"
        "불완전한 LCP 진단 상대이며 RFC 1661 MUST reply를 따르지 않습니다.\n";
    if (!output) return 1;
    int help = argc == 2 && argv && argv[1] && !strcmp(argv[1], "--help");
    if (help || argc != 2 || !argv || !argv[1] || strcmp(argv[1], "--local-auth-once")) {
        if (fputs(usage, output) == EOF || fflush(output)) return 1;
        return help ? 0 : 2;
    }
    const struct trial_backend defaults = {NULL, trial_identity, trial_metadata, trial_preflight};
    const struct trial_backend *b = backend ? backend : &defaults;
    const char *gate = "backend-error";
    struct trial_preflight preflight = {.reason = "not-run"};
    if (!b->identity || !b->metadata || !b->preflight) goto blocked;
    gate = "root-identity";
    if (b->identity(b->context)) goto blocked;
    gate = trial_executable(b);
    if (strcmp(gate, "clear")) goto blocked;
    gate = "root-home-preflight";
    if (b->preflight(b->context, &preflight) ||
        preflight.status != TRIAL_PREFLIGHT_COMPLETE || !preflight.metadata.clear) goto blocked;
    struct ppp_trial_observer observer;
    ppp_trial_observer_begin(&observer);
    struct trial_result result;
    trial_run(&result, &observer);
    ppp_trial_observer_finish(&observer);
    char observation[1024];
    int n = ppp_trial_observer_diagnostic(&observer, observation, sizeof(observation));
    int observed = ppp_trial_observer_result(&observer);
    ppp_trial_observer_dispose(&observer);
    if (n < 0 || (size_t)n >= sizeof(observation)) return 1;
    struct ppp_child *c = &result.child;
    int eof = c->stdout_eof && c->stderr_eof && c->exec_done;
    int success = result.probe.rejects == 5 && !result.probe.failed && c->reaped &&
        eof && !c->io_error && result.handlers_restored && observed == 0;
    if (fprintf(output, "auth-reject=%s rejects=%u requests=%u peer-requests=%u reason=%s"
        " exec=%s child-cleanup=%s transport=%s stdout-eof=%s stderr-eof=%s"
        " handlers=%s stdin-bytes=%" PRIu64 " stdout-bytes=%" PRIu64 " stderr-bytes=%" PRIu64
        " term-count=%u kill-count=%u stderr-mode=%s peer=incomplete-must-reply-violation"
        " installed-source=unmatched sensitive-log=unsuppressed %s\n",
        result.probe.rejects == 5 ? "observed-all" : "not-observed-all", result.probe.rejects,
        result.probe.requests, result.probe.peer_requests, result.probe.reason,
        c->exec_done ? (c->exec_error ? "failed" : "confirmed") : "unconfirmed",
        result.unresolved ? "unresolved" : c->reaped ? "reaped" : c->ownership_lost ? "ownership-lost" : "not-started",
        !result.started ? "not-started" : eof ? "observed-eof" : "transport-abandoned",
        c->stdout_eof ? "observed" : "unproven",
        c->stderr_eof ? "observed" : "unproven", result.handlers_restored ? "restored" : "restore-failed",
        result.probe.sent, result.probe.received, c->stderr_bytes, c->term_signals, c->kill_signals,
        result.status_only ? "status-only" : "bounded-drain", observation) < 0 || fflush(output)) return 1;
    return success ? 0 : 3;
blocked: {
    char diagnostic[1024];
    int size = trial_preflight_diagnostic(&preflight, diagnostic, sizeof(diagnostic));
    if (size < 0 || (size_t)size >= sizeof(diagnostic)) return 1;
    if (fprintf(output, "execution=blocked gate=%s reason=%s %s"
        " auth-reject=unverified child-cleanup=not-started\n", gate, preflight.reason, diagnostic) < 0 ||
        fflush(output)) return 1;
    return 3;
}
}
int main(int argc, char **argv) { return auth_trial(argc, argv, stdout, NULL); }
