#include "ppp_runner.h"

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct ppp_runner {
    struct ppp_session *session;
    pthread_t owner;
    CFRunLoopRef loop;
    struct sigaction saved[2];
    struct ppp_runner_status status;
    int busy;
};
/* Disposition ownership is process-wide, not a thread-local or signal mask API.
 * The caller serializes competing disposition owners and blocks other threads. */
static struct ppp_runner *runner_lease;
static int runner_process_busy;
static volatile sig_atomic_t runner_signal;
static const int runner_signals[] = {SIGINT, SIGTERM};

static void runner_handler(int signal_number) {
    runner_signal = signal_number;
}
static void runner_error(struct ppp_runner *r, enum ppp_runner_error error) {
    if (r->status.error == PPP_RUNNER_ERROR_NONE) r->status.error = error;
}
static int runner_enter(struct ppp_runner *r) {
    if (!r || !pthread_equal(r->owner, pthread_self()) ||
        r->loop != CFRunLoopGetCurrent() || r->busy || runner_process_busy ||
        runner_lease != r) return -1;
    r->busy = runner_process_busy = 1;
    struct ppp_session_status status;
    if (ppp_session_snapshot(r->session, &status)) {
        r->busy = runner_process_busy = 0;
        return -1;
    }
    return 0;
}
static void runner_leave(struct ppp_runner *r) {
    r->busy = runner_process_busy = 0;
}
static void runner_stop(struct ppp_runner *r) {
    if (!r->status.stop_requested && !r->status.session.local_done) {
        r->status.stop_requested = 1;
        (void)ppp_session_stop(r->session);
    }
}
static void runner_requests(struct ppp_runner *r, const volatile sig_atomic_t *stop_flag) {
    if (r->status.installed_actions && runner_signal)
        r->status.signal_number = runner_signal;
    if (r->status.signal_number || (stop_flag && *stop_flag)) runner_stop(r);
}
static void runner_restore_actions(struct ppp_runner *r, int rollback) {
    for (int i = 1; i >= 0; i--) {
        unsigned bit = 1u << i;
        if (!(r->status.unrestored_actions & bit)) continue;
        if (sigaction(runner_signals[i], &r->saved[i], NULL)) {
            int error = errno;
            int *record = rollback ? &r->status.rollback_errno[i] : &r->status.restore_errno[i];
            if (!*record) *record = error;
            runner_error(r, rollback ? PPP_RUNNER_ERROR_ROLLBACK : PPP_RUNNER_ERROR_RESTORE);
        } else r->status.unrestored_actions &= ~bit;
    }
}
int ppp_runner_begin(struct ppp_session **session, struct ppp_runner **runner) {
    if (!session || !*session || !runner || *runner || runner_lease || runner_process_busy) return -1;
    runner_process_busy = 1;
    struct ppp_session_status status;
    if (ppp_session_snapshot(*session, &status)) { runner_process_busy = 0; return -1; }
    struct ppp_runner *r = calloc(1, sizeof(*r));
    if (!r) { runner_process_busy = 0; return -1; }
    r->owner = pthread_self();
    r->loop = CFRunLoopGetCurrent();
    r->busy = 1;
    CFRetain(r->loop);
    r->session = *session;
    r->status.session = status;
    runner_lease = r;
    *session = NULL;
    /* Accepted object remains owned even when installation or rollback fails. */
    if (!status.local_done) {
        struct sigaction action = {0};
        action.sa_handler = runner_handler;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGINT);
        sigaddset(&action.sa_mask, SIGTERM);
        action.sa_flags = 0;
        runner_signal = 0; /* Once, before the first installation only. */
        for (unsigned i = 0; i < 2; i++) {
            if (sigaction(runner_signals[i], &action, &r->saved[i])) {
                r->status.install_errno[i] = errno;
                runner_error(r, PPP_RUNNER_ERROR_INSTALL);
                runner_stop(r);
                runner_restore_actions(r, 1);
                break;
            }
            r->status.installed_actions |= 1u << i;
            r->status.unrestored_actions |= 1u << i;
        }
    }
    *runner = r;
    runner_leave(r);
    return 0;
}
int ppp_runner_slice(struct ppp_runner *r, const volatile sig_atomic_t *stop_flag,
                      const uint32_t *wait_ms, struct ppp_runner_status *status) {
    uint32_t delay = wait_ms ? *wait_ms : 10;
    if (!status || !delay || delay > 50 || runner_enter(r)) return -1;
    runner_requests(r, stop_flag);
    if (!r->status.session.local_done) {
        (void)ppp_session_step(r->session);
        (void)ppp_session_snapshot(r->session, &r->status.session);
    }
    runner_requests(r, stop_flag);
    if (!r->status.session.local_done) {
        struct timespec wait = {.tv_sec = 0, .tv_nsec = (long)delay * 1000000};
        if (nanosleep(&wait, NULL) && errno != EINTR) {
            if (!r->status.wait_errno) r->status.wait_errno = errno;
            runner_error(r, PPP_RUNNER_ERROR_WAIT);
            runner_stop(r);
        }
    }
    runner_requests(r, stop_flag);
    (void)ppp_session_snapshot(r->session, &r->status.session);
    *status = r->status;
    int done = status->session.local_done;
    runner_leave(r);
    return done;
}
int ppp_runner_restore(struct ppp_runner *r, struct ppp_runner_status *status) {
    if (!status || runner_enter(r)) return -1;
    runner_stop(r); /* Before any disposition change, including failed restore. */
    runner_requests(r, NULL);
    runner_restore_actions(r, 0);
    (void)ppp_session_snapshot(r->session, &r->status.session);
    *status = r->status;
    int remaining = !!status->unrestored_actions;
    runner_leave(r);
    return remaining;
}
int ppp_runner_observe(struct ppp_runner *r, struct ppp_runner_status *status,
                       struct ppp_session_observation *observation) {
    if (!status || !observation || runner_enter(r)) return -1;
    runner_requests(r, NULL);
    struct ppp_session_observation copy;
    int error = ppp_session_observe(r->session, &copy);
    if (!error) {
        (void)ppp_session_snapshot(r->session, &r->status.session);
        *status = r->status; *observation = copy;
    }
    runner_leave(r);
    return error;
}
int ppp_runner_release(struct ppp_runner **runner) {
    if (!runner) return -1;
    struct ppp_runner *r = *runner;
    if (!r) return 0;
    if (runner_enter(r)) return -1;
    if (!r->status.session.local_done || r->status.unrestored_actions ||
        ppp_session_release(&r->session)) { runner_leave(r); return -1; }
    CFRelease(r->loop);
    runner_lease = NULL;
    free(r);
    *runner = NULL;
    runner_process_busy = 0;
    return 0;
}
