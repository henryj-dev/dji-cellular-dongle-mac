#include "ppp_session.h"
#include "discovery.h"
#include "observer_policy.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/wait.h>

struct ppp_session {
    struct ppp_supervisor supervisor;
    struct ppp_usb *adapter;
    struct ppp_peer peer;
    struct ppp_budgets budgets;
    size_t capacity;
    enum ppp_launch_mode launch;
    pthread_t owner;
    CFRunLoopRef loop;
    enum ppp_session_state state;
    enum ppp_session_error error;
    int busy, supervisor_valid, stop_requested, stop_delivered, cleanup_error;
};
static int session_enter(struct ppp_session *s) {
    if (!s || !pthread_equal(s->owner, pthread_self()) ||
        s->loop != CFRunLoopGetCurrent() || s->busy) return -1;
    s->busy = 1;
    return 0;
}
static void session_error(struct ppp_session *s, enum ppp_session_error error) {
    if (s->error == PPP_SESSION_ERROR_NONE) s->error = error;
}
static void session_supervisor_status(struct ppp_session *s) {
    if (s->supervisor.error != PPP_ERROR_NONE)
        session_error(s, PPP_SESSION_ERROR_SUPERVISOR);
    if (s->supervisor.error == PPP_ERROR_CLOSE ||
        s->supervisor.stream.error == OFFLINE_ERROR_CLOSE) s->cleanup_error = 1;
    if (s->supervisor.phase == PPP_CLOSED) s->state = PPP_SESSION_CLOSED;
}
/* IN may stay quiet for the whole CONNECT/SESSION/DRAIN span plus 1000 ms of
 * slack; 0 or a sum beyond uint32_t is rejected (each term is checked first so
 * the uint64_t sum cannot wrap). */
static uint32_t session_read_timeout(const struct ppp_budgets *b) {
    if (b->connect_ms > UINT32_MAX || b->session_ms > UINT32_MAX || b->drain_ms > UINT32_MAX) return 0;
    uint64_t total = b->connect_ms + b->session_ms + b->drain_ms + 1000;
    return total > UINT32_MAX ? 0 : (uint32_t)total;
}
int ppp_session_start(const struct ppp_session_request *r,
                      const struct ppp_metadata_backend *metadata_backend,
                      struct ppp_session **session) {
    uint32_t read_timeout = !r ? 0 : r->read_timeout_ms ? *r->read_timeout_ms :
        session_read_timeout(&r->budgets);
    if (!session || *session || !r || !read_timeout ||
        (r->launch != PPP_LAUNCH_MODE_ONCE && r->launch != PPP_LAUNCH_MODE_UP) ||
        /* A long-lived session must never fall back to the derived IN timeout. */
        (r->launch == PPP_LAUNCH_MODE_UP && !r->read_timeout_ms) || !r->service || !r->registry_id || !r->device || !*r->device ||
        !observer_config_allowed(r->config) || !r->capacity || r->capacity > PPP_SUPERVISOR_BUFFER ||
        !r->budgets.start_ms || !r->budgets.connect_ms || !r->budgets.session_ms ||
        !r->budgets.drain_ms || (r->timeout_ms && !*r->timeout_ms) ||
        (metadata_backend && (!metadata_backend->real_uid || !metadata_backend->home ||
                              !metadata_backend->metadata))) return -1;
    struct ppp_session *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->owner = pthread_self();
    s->loop = CFRunLoopGetCurrent();
    s->busy = 1; /* Before any external operation; handle stays private until done. */
    CFRetain(s->loop);
    s->budgets = r->budgets;
    s->capacity = r->capacity;
    s->launch = r->launch;
    s->state = PPP_SESSION_PREPARED;
    struct ppp_launch_preflight preflight;
    if (ppp_launch_preflight(&preflight, metadata_backend) || !preflight.clear) {
        session_error(s, PPP_SESSION_ERROR_METADATA);
        s->state = PPP_SESSION_CLOSED;
    } else if (ppp_usb_acquire(r->service, r->device, r->registry_id, r->config,
                               r->timeout_ms, &read_timeout, &s->adapter) != DJI_OK) {
        session_error(s, PPP_SESSION_ERROR_ACQUIRE);
        s->state = PPP_SESSION_CLOSED;
    } else if (ppp_usb_peer(s->adapter, &s->supervisor, &s->peer)) {
        session_error(s, PPP_SESSION_ERROR_BIND);
        s->state = PPP_SESSION_DISPOSING;
    }
    s->busy = 0;
    *session = s;
    return 0;
}
int ppp_session_step(struct ppp_session *s) {
    if (session_enter(s)) return -1;
    if (s->state == PPP_SESSION_PREPARED) {
        if (s->stop_requested) s->state = PPP_SESSION_DISPOSING;
        else {
            int started = s->launch == PPP_LAUNCH_MODE_UP ?
                ppp_supervisor_start_up(&s->supervisor, &s->peer, &s->budgets, s->capacity) :
                ppp_supervisor_start_fixed(&s->supervisor, &s->peer, &s->budgets, s->capacity);
            if (started) {
                /* Early validation/clock failures need not initialize supervisor. */
                session_error(s, PPP_SESSION_ERROR_START);
                s->state = PPP_SESSION_DISPOSING;
            } else {
                s->supervisor_valid = 1;
                s->state = PPP_SESSION_RUNNING;
                s->adapter = NULL;
                s->peer = (struct ppp_peer){0}; /* Ownership belongs only to supervisor. */
            }
            s->busy = 0;
            return 0;
        }
    }
    if (s->state == PPP_SESSION_DISPOSING) {
        struct ppp_usb_dispose_result result = ppp_usb_dispose_step(&s->adapter);
        if (result.error) {
            s->cleanup_error = 1;
            session_error(s, PPP_SESSION_ERROR_DISPOSE);
        }
        if (result.released) {
            s->peer = (struct ppp_peer){0};
            s->state = PPP_SESSION_CLOSED;
        }
    } else if (s->state == PPP_SESSION_RUNNING) {
        struct ppp_supervisor *supervisor = &s->supervisor;
        if (s->stop_requested && !s->stop_delivered && !supervisor->error &&
            !supervisor->stopped && (supervisor->phase == PPP_START ||
            supervisor->phase == PPP_CONNECT || supervisor->phase == PPP_SESSION)) {
            s->stop_delivered = 1; /* Before callbacks, including a failing stop. */
            if (ppp_supervisor_stop(supervisor) < 0)
                session_error(s, PPP_SESSION_ERROR_SUPERVISOR);
        } else {
            if (ppp_supervisor_step(supervisor) < 0)
                session_error(s, PPP_SESSION_ERROR_SUPERVISOR);
        }
        session_supervisor_status(s);
    }
    int done = s->state == PPP_SESSION_CLOSED;
    s->busy = 0;
    return done;
}
int ppp_session_stop(struct ppp_session *s) {
    if (session_enter(s)) return -1;
    s->stop_requested = 1;
    s->busy = 0;
    return 0;
}
int ppp_session_peer_done(struct ppp_session *s) {
    if (session_enter(s)) return -1;
    int result = -1;
    if (s->supervisor_valid && s->state == PPP_SESSION_RUNNING &&
        s->supervisor.phase == PPP_DRAIN) result = ppp_supervisor_peer_done(&s->supervisor);
    s->busy = 0;
    return result;
}
int ppp_session_snapshot(struct ppp_session *s, struct ppp_session_status *status) {
    if (!status || session_enter(s)) return -1;
    *status = (struct ppp_session_status){.state = s->state, .error = s->error,
        .local_done = s->state == PPP_SESSION_CLOSED, .cleanup_error = s->cleanup_error,
        .stop_requested = s->stop_requested, .supervisor_valid = s->supervisor_valid};
    if (s->supervisor_valid) {
        status->supervisor_phase = s->supervisor.phase;
        status->supervisor_error = s->supervisor.error;
    }
    s->busy = 0;
    return 0;
}
int ppp_session_observe(struct ppp_session *s, struct ppp_session_observation *o) {
    if (!o || session_enter(s)) return -1;
    *o = (struct ppp_session_observation){.stop_requested = s->stop_requested};
    if (s->supervisor_valid) {
        const struct ppp_child *c = &s->supervisor.child;
        o->pid = c->pid; o->started = c->started; o->reaped = c->reaped;
        o->ownership_lost = c->ownership_lost;
        o->stdout_eof = c->stdout_eof; o->stderr_eof = c->stderr_eof;
        o->exec_done = c->exec_done;
        o->term_signals = c->term_signals; o->kill_signals = c->kill_signals;
        o->peer_quiet = s->supervisor.peer_quiet;
        o->peer_closed = s->supervisor.peer_closed;
        o->exited = c->reap_count && WIFEXITED(c->wait_status);
        o->exit_code = o->exited ? WEXITSTATUS(c->wait_status) : 0;
        o->signaled = c->reap_count && WIFSIGNALED(c->wait_status);
        o->term_signal = o->signaled ? WTERMSIG(c->wait_status) : 0;
        o->exec_error = c->exec_error;
        o->connect_attempted = s->supervisor.connect_attempted;
        o->output_observed = s->supervisor.output_observed;
        const struct offline_stream *stream = &s->supervisor.stream;
        o->stream_state = stream->state; o->stream_error = stream->error;
        o->modem_result = stream->modem_result; o->modem_code = stream->modem_code;
        o->modem_code_valid = stream->modem_code_valid; o->echo_seen = stream->echo_seen;
        o->control_bytes = stream->control_bytes;
        o->child_output_bytes = s->supervisor.child_output_bytes;
        o->usb_input_bytes = s->supervisor.usb_input_bytes;
        o->usb_input_reads = s->supervisor.usb_input_reads;
        o->stderr_bytes = c->stderr_bytes;
        /* Supervisor-held copy only: the adapter may already be freed. */
        o->usb_error_kind = s->supervisor.usb_error_kind;
        o->usb_error_site = s->supervisor.usb_error_site;
        o->negotiation = s->supervisor.negotiation.summary; /* value copy, counters only */
    }
    s->busy = 0;
    return 0;
}
int ppp_session_release(struct ppp_session **session) {
    if (!session) return -1;
    struct ppp_session *s = *session;
    if (!s) return 0;
    if (session_enter(s)) return -1;
    if (s->state != PPP_SESSION_CLOSED) { s->busy = 0; return -1; }
    CFRelease(s->loop);
    /* Partial frame prefixes must not outlive the session in freed heap. */
    ppp_negotiation_wipe(&s->supervisor.negotiation);
    free(s);
    *session = NULL;
    return 0;
}
