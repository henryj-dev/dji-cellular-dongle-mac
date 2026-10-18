#include "ppp_supervisor.h"
#include "ppp_launch.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>

static void fail(struct ppp_supervisor *s, enum ppp_error error) {
    if (s->error == PPP_ERROR_NONE) s->error = error;
}
static uint64_t now_ms(void *context) {
    struct ppp_supervisor *s = context;
    if (s->clock_failed) return UINT64_MAX;
    uint64_t now = s->peer.now_ms(s->peer.context);
    if (now == UINT64_MAX || now < s->last_ms) {
        s->clock_failed = 1;
        fail(s, PPP_ERROR_CLOCK);
        return UINT64_MAX; /* Make the core stop before any I/O callback. */
    }
    s->last_ms = now;
    return now;
}
static int clock_check(struct ppp_supervisor *s) {
    uint64_t now = now_ms(s);
    if (!s->error && now >= s->deadline_ms) fail(s, PPP_ERROR_TIMEOUT);
    return s->error ? -1 : 0;
}
static void phase(struct ppp_supervisor *s, enum ppp_phase next, uint64_t budget) {
    s->phase = next;
    if (budget > UINT64_MAX - s->last_ms) {
        s->clock_failed = 1; fail(s, PPP_ERROR_CLOCK);
    } else s->deadline_ms = s->last_ms + budget;
}
static struct offline_io stdio_write(void *context, const unsigned char *data, size_t length) {
    struct ppp_supervisor *s = context;
    return ppp_child_write(&s->child, data, length);
}
static struct offline_io usb_write(void *context, const unsigned char *data, size_t length) {
    struct ppp_supervisor *s = context;
    struct offline_io result = s->peer.write(s->peer.context, data, length);
    if (result.code != OFFLINE_IO_OK || result.count != length) fail(s, PPP_ERROR_USB);
    return result;
}
static int core_close(void *context) {
    struct ppp_supervisor *s = context;
    int result = ppp_child_close(&s->child);
    s->peer_closed = 1;
    int peer_result = s->peer.close(s->peer.context);
    if (result || peer_result) fail(s, PPP_ERROR_CLOSE);
    return result || peer_result;
}
static void core_release(void *context) {
    struct ppp_supervisor *s = context;
    s->peer.release(s->peer.context);
}
static void core_result(struct ppp_supervisor *s, int result) {
    if (result == OFFLINE_OK || result == OFFLINE_FULL) return;
    fail(s, s->stream.error == OFFLINE_ERROR_TIMEOUT ? PPP_ERROR_TIMEOUT :
            s->stream.error == OFFLINE_ERROR_CLOCK ? PPP_ERROR_CLOCK : PPP_ERROR_CORE);
}

int ppp_supervisor_start(struct ppp_supervisor *s, const struct ppp_peer *peer,
                         const struct ppp_budgets *budgets, size_t capacity,
                         const char *path, char *const argv[], char *const envp[], const char *cwd) {
    if (!s || !peer || !peer->read || !peer->write || !peer->now_ms || !peer->close ||
        !peer->release || (!!peer->quiesce != !!peer->poll_quiet) ||
        !budgets || !budgets->start_ms || !budgets->connect_ms ||
        !budgets->session_ms || !budgets->drain_ms || !capacity ||
        capacity > PPP_SUPERVISOR_BUFFER) return -1;
    struct ppp_peer copy = *peer;
    struct ppp_budgets times = *budgets;
    uint64_t now = copy.now_ms(copy.context);
    if (now == UINT64_MAX || times.start_ms > UINT64_MAX - now) return -1;
    *s = (struct ppp_supervisor){.peer = copy, .budgets = times, .phase = PPP_START,
        .last_ms = now, .deadline_ms = now + times.start_ms};
    ppp_negotiation_init(&s->negotiation);
    ppp_child_init(&s->child);
    struct offline_backend backend = {s, stdio_write, usb_write, now_ms, core_close, core_release};
    if (offline_stream_init(&s->stream, &backend, s->rx_storage, capacity,
                            s->tx_storage, capacity) != OFFLINE_OK ||
        offline_queue_init(&s->pending_tx, s->pending_storage, capacity) != OFFLINE_OK) return -1;
    return ppp_child_start(&s->child, path, argv, envp, cwd);
}

int ppp_supervisor_start_observed(struct ppp_supervisor *s, const struct ppp_peer *peer,
                                  const struct ppp_budgets *budgets, size_t capacity,
                                  const char *path, char *const argv[], char *const envp[], const char *cwd) {
    if (!peer || !peer->begin_connect) return -1;
    int result = ppp_supervisor_start(s, peer, budgets, capacity, path, argv, envp, cwd);
    if (!result) s->observed_start = 1;
    return result;
}
int ppp_supervisor_start_fixed(struct ppp_supervisor *s, const struct ppp_peer *peer,
                                const struct ppp_budgets *budgets, size_t capacity) {
    if (!peer || !peer->begin_connect) return -1;
    struct ppp_launch_policy policy;
    if (ppp_launch_prepare(&policy) != 0) return -1;
    return ppp_supervisor_start_observed(s, peer, budgets, capacity,
                                         policy.path, policy.argv, policy.envp, policy.cwd);
}
int ppp_supervisor_start_up(struct ppp_supervisor *s, const struct ppp_peer *peer,
                            const struct ppp_budgets *budgets, size_t capacity) {
    if (!peer || !peer->begin_connect) return -1;
    struct ppp_launch_up_policy policy;
    if (ppp_launch_prepare_up(&policy) != 0) return -1;
    return ppp_supervisor_start_observed(s, peer, budgets, capacity,
                                         policy.path, policy.argv, policy.envp, policy.cwd);
}
int ppp_supervisor_ready(struct ppp_supervisor *s) {
    if (!s || !s->child.started || s->observed_start || s->error || s->ready ||
        s->phase >= PPP_DRAIN) return -1;
    s->ready = 1;
    return 0;
}
int ppp_supervisor_stop(struct ppp_supervisor *s) {
    if (!s || !s->child.started || s->error || s->phase >= PPP_CLEANUP) return -1;
    if (s->stopped) return 0;
    s->stopped = 1;
    if (clock_check(s)) return -1;
    if (s->phase == PPP_START || s->phase == PPP_CONNECT) {
        s->cancel_requested = 1; /* Close the I/O/readiness gate before callbacks. */
        phase(s, PPP_CANCEL, s->budgets.drain_ms);
        if (s->error) return -1;
        if (s->stream.state != OFFLINE_CONTROL_WAIT || s->stream.error != OFFLINE_ERROR_NONE ||
            s->stream.recovery_unknown) { fail(s, PPP_ERROR_CORE); return -1; }
        int result = offline_stream_stop(&s->stream, s->budgets.drain_ms);
        s->cancel_core_expected = result == OFFLINE_INVALID &&
            s->stream.error == OFFLINE_ERROR_CONTROL && s->stream.recovery_unknown &&
            s->stream.state == OFFLINE_RECOVERY_UNKNOWN;
        if (!s->cancel_core_expected) { fail(s, PPP_ERROR_CORE); return -1; }
        unsigned sent = s->child.term_signals;
        if (ppp_child_signal(&s->child, SIGTERM)) fail(s, PPP_ERROR_CHILD);
        s->term_sent = s->child.term_signals != sent;
        return s->error ? -1 : 0;
    }
    if (s->phase != PPP_SESSION || !s->stream.ready) { fail(s, PPP_ERROR_CORE); return 0; }
    core_result(s, offline_stream_stop(&s->stream, s->budgets.drain_ms));
    if (s->error) return -1;
    phase(s, PPP_DRAIN, s->budgets.drain_ms);
    if (s->error) return -1;
    if (ppp_child_signal(&s->child, SIGTERM)) fail(s, PPP_ERROR_CHILD);
    return s->error ? -1 : 0;
}
int ppp_supervisor_peer_done(struct ppp_supervisor *s) {
    if (!s || s->phase != PPP_DRAIN || s->peer_done || s->error) return -1;
    s->peer_done = 1;
    return 0;
}

static void read_child(struct ppp_supervisor *s) {
    if (s->stdout_eof) return;
    size_t space = s->pending_tx.capacity - s->pending_tx.length;
    if (space > s->stdout_remaining) space = s->stdout_remaining;
    if (!space) return; /* True pipe backpressure, not a dropped completion. */
    unsigned char data[PPP_SUPERVISOR_BUFFER];
    struct offline_io result = ppp_child_read(&s->child, data, space);
    if (result.code == OFFLINE_IO_EOF) s->stdout_eof = 1;
    else if (result.code == OFFLINE_IO_OK) {
        s->stdout_remaining -= result.count;
        s->child_output_bytes += result.count;
        if (offline_queue_enqueue(&s->pending_tx, data, result.count) != OFFLINE_OK)
            fail(s, PPP_ERROR_CORE);
        else if (s->observed_start) s->output_observed = 1;
    } else if (result.code != OFFLINE_IO_AGAIN && result.code != OFFLINE_IO_INTR)
        fail(s, PPP_ERROR_CHILD);
}
static void flush_pending(struct ppp_supervisor *s) {
    if (!s->stream.ready || !s->pending_tx.length) return;
    size_t length, consumed = 0;
    const unsigned char *span = offline_queue_peek(&s->pending_tx, &length);
    uint64_t accepted = s->stream.tx.accepted;
    core_result(s, offline_stream_input(&s->stream, OFFLINE_TX, OFFLINE_IO_OK,
                                        span, length, length, &consumed));
    /* Diagnostic copy of exactly the bytes the core queued, the tail of the
     * consumed span. No effect on consumption, order or errors. */
    uint64_t queued = s->stream.tx.accepted - accepted;
    if (queued && queued <= consumed)
        ppp_negotiation_input(&s->negotiation, PPP_NEGO_LOCAL, span + consumed - (size_t)queued, (size_t)queued);
    if (consumed && offline_queue_consume(&s->pending_tx, consumed) != OFFLINE_OK)
        fail(s, PPP_ERROR_CORE);
}
static void read_usb(struct ppp_supervisor *s) {
    if (!s->usb_length && !s->peer_done) {
        struct offline_io result = s->peer.read(s->peer.context, s->usb_input, sizeof(s->usb_input));
        if (result.code == OFFLINE_IO_AGAIN) {
            (void)clock_check(s);
            return; /* No completion, but callback time still counts. */
        }
        if (result.code != OFFLINE_IO_OK) { fail(s, PPP_ERROR_USB); return; }
        if (!result.count || result.count > sizeof(s->usb_input)) { fail(s, PPP_ERROR_USB); return; }
        s->usb_length = result.count;
        s->usb_offset = 0;
        s->usb_input_bytes += result.count;
        if (clock_check(s)) return; /* No new writes after a late callback. */
    }
    if (!s->usb_length) return;
    size_t consumed = 0, remaining = s->usb_length - s->usb_offset;
    uint64_t accepted = s->stream.rx.accepted;
    core_result(s, offline_stream_input(&s->stream, OFFLINE_RX, OFFLINE_IO_OK,
        s->usb_input + s->usb_offset, remaining, remaining, &consumed));
    /* Diagnostic copy of only the bytes queued toward pppd: control text the
     * core parsed before CONNECT is consumed but never queued, so never fed. */
    uint64_t queued = s->stream.rx.accepted - accepted;
    if (queued && queued <= consumed)
        ppp_negotiation_input(&s->negotiation, PPP_NEGO_PEER,
                              s->usb_input + s->usb_offset + consumed - (size_t)queued, (size_t)queued);
    s->usb_offset += consumed;
    if (s->usb_offset == s->usb_length) s->usb_offset = s->usb_length = 0;
    if (s->error) return;
    if (s->stream.state == OFFLINE_DATA_STREAM && !s->connected) {
        s->connected = 1;
        phase(s, PPP_SESSION, s->budgets.session_ms);
        if (s->error) return;
        core_result(s, offline_stream_ready(&s->stream));
    }
}
static void error_cleanup(struct ppp_supervisor *s) {
    if (!s->error || s->error_cleanup_started) return;
    s->error_cleanup_started = 1;
    s->phase = PPP_CLEANUP;
    uint64_t now = now_ms(s);
    if (!s->clock_failed) {
        if (s->budgets.drain_ms > UINT64_MAX - now) {
            s->clock_failed = 1; fail(s, PPP_ERROR_CLOCK);
        } else s->transport_deadline_ms = now + s->budgets.drain_ms;
    }
    if (!s->child.reaped && !s->child.ownership_lost && !s->kill_sent) {
        s->kill_sent = 1;
        if (ppp_child_signal(&s->child, SIGKILL)) fail(s, PPP_ERROR_CHILD);
    }
    if (!s->stream.recovery_unknown) {
        size_t consumed;
        (void)offline_stream_input(&s->stream, OFFLINE_RX, OFFLINE_IO_ERROR,
                                   NULL, 0, 0, &consumed);
    }
}
static int cleanup(struct ppp_supervisor *s) {
    if (!s->cancel_requested || s->error) s->phase = PPP_CLEANUP;
    ppp_child_end_input(&s->child);
    int expected_exit = WIFEXITED(s->child.wait_status) && !WEXITSTATUS(s->child.wait_status);
    int expected_term = s->cancel_requested && s->term_sent &&
        WIFSIGNALED(s->child.wait_status) && WTERMSIG(s->child.wait_status) == SIGTERM;
    if (s->child.io_error || s->child.exec_error || s->child.ownership_lost ||
        (s->child.reaped && !expected_exit && !expected_term))
        fail(s, PPP_ERROR_CHILD);
    error_cleanup(s);
    if (!s->quiesce_started) {
        s->quiesce_started = 1;
        if (s->peer.quiesce && s->peer.quiesce(s->peer.context)) fail(s, PPP_ERROR_USB);
    }
    error_cleanup(s);
    struct ppp_quiet result = s->peer.poll_quiet ? s->peer.poll_quiet(s->peer.context) :
                                                (struct ppp_quiet){1, 0};
    if (result.error) fail(s, PPP_ERROR_USB);
    s->peer_quiet = !!result.quiet;
    error_cleanup(s);
    (void)clock_check(s); /* Includes time spent in quiesce/completion callbacks. */
    error_cleanup(s); /* A normal cleanup may have failed only just now. */
    if ((s->error || s->cancel_requested) && s->stdout_remaining &&
        !s->child.stdout_eof && s->child.output_fd >= 0) {
        unsigned char discard[PPP_SUPERVISOR_BUFFER];
        struct offline_io io = ppp_child_read(&s->child, discard, s->stdout_remaining);
        if (io.code == OFFLINE_IO_OK) {
            s->stdout_remaining -= io.count;
            if (io.count > UINT64_MAX - s->child.stdout_discarded) fail(s, PPP_ERROR_CHILD);
            else s->child.stdout_discarded += io.count;
        } else if (io.code == OFFLINE_IO_ERROR) fail(s, PPP_ERROR_CHILD);
    }
    s->stdout_eof = s->child.stdout_eof;
    /* Deadlines never stand in for waitpid ownership or returned callbacks. */
    if ((!s->child.reaped && !s->child.ownership_lost) || !s->peer_quiet) return 0;
    int transport_done = s->child.stdout_eof && s->child.stderr_eof && s->child.exec_done;
    if (!transport_done && (!s->error || (!s->clock_failed &&
        s->last_ms < s->transport_deadline_ms))) return 0;
    if (!s->error && !s->cancel_requested) {
        if (s->pending_tx.length || s->stream.tx.length || s->stream.rx.length ||
            s->usb_length || !s->peer_done || s->child.input_fd >= 0 ||
            !WIFEXITED(s->child.wait_status) || WEXITSTATUS(s->child.wait_status))
            fail(s, PPP_ERROR_CHILD);
        else core_result(s, offline_stream_drain_complete(&s->stream));
        error_cleanup(s);
    }
    core_result(s, offline_stream_join(&s->stream));
    (void)offline_stream_poll(&s->stream);
    if (s->stream.error == OFFLINE_ERROR_CLOSE) fail(s, PPP_ERROR_CLOSE);
    if (s->stream.state != OFFLINE_CLOSED) { fail(s, PPP_ERROR_CORE); return -1; }
    s->phase = PPP_CLOSED;
    return 1;
}
int ppp_supervisor_step(struct ppp_supervisor *s) {
    if (!s || !s->child.started) return -1;
    if (s->phase == PPP_CLOSED) return 1;
    s->stdout_remaining = PPP_SUPERVISOR_BUFFER;
    (void)clock_check(s);
    /* Exactly one stderr discard allowance per step, including cleanup and
     * internal signal/readiness polls (which use status-only polling). */
    if (ppp_child_poll(&s->child)) fail(s, PPP_ERROR_CHILD);
    if (s->cancel_requested || s->phase == PPP_CLEANUP || s->error) return cleanup(s);
    flush_pending(s);
    if (s->error) return cleanup(s);
    read_child(s); /* Reap/exit does not hide unread stdout. */
    if (s->error) return cleanup(s);
    if (s->phase == PPP_START && s->child.exec_done && s->observed_start && s->output_observed)
        s->ready = 1;
    if (s->phase == PPP_START && s->child.exec_done && s->ready) {
        if (s->observed_start || s->peer.begin_connect) {
            if (clock_check(s)) return cleanup(s);
            if (ppp_child_poll_status(&s->child) || s->stdout_eof ||
                s->child.reaped || s->child.ownership_lost)
                fail(s, PPP_ERROR_CHILD);
            if (s->error || clock_check(s)) return cleanup(s);
        }
        phase(s, PPP_CONNECT, s->budgets.connect_ms);
        if (s->error) return cleanup(s);
        if (s->peer.begin_connect) {
            s->connect_attempted = 1; /* Fixed before callback, never replayed. */
            if (s->peer.begin_connect(s->peer.context)) fail(s, PPP_ERROR_USB);
            if (clock_check(s)) return cleanup(s);
        }
    }
    if (s->error) return cleanup(s);
    if (s->phase >= PPP_CONNECT) read_usb(s);
    if (s->error) return cleanup(s);
    if (s->stream.ready) {
        flush_pending(s);
        if (s->error) return cleanup(s);
        core_result(s, offline_stream_step(&s->stream, OFFLINE_TX));
        if (s->error || clock_check(s)) return cleanup(s);
        core_result(s, offline_stream_step(&s->stream, OFFLINE_RX));
        if (s->error || clock_check(s)) return cleanup(s);
    }
    if (s->stdout_eof && s->phase < PPP_SESSION) {
        fail(s, PPP_ERROR_CHILD);
        return cleanup(s);
    }
    if (s->stdout_eof && s->phase == PPP_SESSION)
        (void)ppp_supervisor_stop(s); /* Transport drain, not LCP success. */
    if (s->error) return cleanup(s);
    if (s->phase == PPP_DRAIN) {
        if (s->peer_done && !s->usb_length && !s->stream.rx.length)
            ppp_child_end_input(&s->child);
        if (s->stdout_eof && !s->pending_tx.length && !s->stream.tx.length &&
            !s->stream.rx.length && !s->usb_length && s->peer_done)
            return cleanup(s);
    }
    return s->error ? cleanup(s) : 0;
}
int ppp_supervisor_diagnostic(const struct ppp_supervisor *s, char *buffer, size_t capacity) {
    static const char *const phases[] = {"start", "connect", "session", "drain", "cancel", "cleanup", "closed"};
    static const char *const errors[] = {"none", "child", "usb", "core", "timeout", "clock", "close"};
    if (!s || (!buffer && capacity) || (unsigned)s->phase >= sizeof(phases) / sizeof(phases[0]) ||
        (unsigned)s->error >= sizeof(errors) / sizeof(errors[0])) return -1;
    char child[512];
    if (ppp_child_diagnostic(&s->child, child, sizeof(child)) < 0) return -1;
    return snprintf(buffer, capacity,
        "supervisor phase=%s error=%s connected=%d ready=%d cancellation=%d recovery=%s core-error=%s pending=%zu usb_retained=%zu quiesce=%d peer_quiet=%d %s",
        phases[s->phase], errors[s->error], s->connected, s->ready, s->cancel_requested,
        s->error || s->stream.recovery_unknown ? "unknown" : "unproven",
        s->stream.error == OFFLINE_ERROR_CONTROL ? "control" : s->stream.error == OFFLINE_ERROR_NONE ? "none" : "other",
        s->pending_tx.length, s->usb_length - s->usb_offset, s->quiesce_started, s->peer_quiet, child);
}
