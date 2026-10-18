#ifndef DJI_PPP_RUNNER_H
#define DJI_PPP_RUNNER_H

#include "ppp_session.h"
#include <signal.h>

struct ppp_runner;
enum ppp_runner_error {
    PPP_RUNNER_ERROR_NONE, PPP_RUNNER_ERROR_INSTALL, PPP_RUNNER_ERROR_ROLLBACK,
    PPP_RUNNER_ERROR_RESTORE, PPP_RUNNER_ERROR_WAIT
};
enum ppp_runner_action {
    PPP_RUNNER_SIGINT = 1u, PPP_RUNNER_SIGTERM = 2u
};
struct ppp_runner_status {
    struct ppp_session_status session;
    enum ppp_runner_error error;
    /* Index 0 = SIGINT, 1 = SIGTERM. First errno per operation/item is sticky. */
    int install_errno[2], rollback_errno[2], restore_errno[2], wait_errno;
    int signal_number, stop_requested;
    unsigned installed_actions, unrestored_actions;
};
/* Internal owning API; not wired to the production CLI. Caller serializes ALL
 * process-wide SIGINT/SIGTERM disposition changes (including observer users),
 * blocks these signals on other threads and delivers them on the session owner.
 * No thread mask API or detection of every precondition violation is provided.
 * All calls run outside handlers, on the session's pthread and CFRunLoop, without
 * reentry. One process-wide lease lasts through release, even after restore.
 * begin requires *runner == NULL. Rejection/allocation failure returns -1 without
 * changing handles. Acceptance returns 0 and consumes *session (sets it NULL),
 * even on signal installation/rollback failure: retain the runner and clean up.
 * CLOSED input skips installation. Session start's prior synchronous work is NOT
 * cancellation protected. Do not retain/use copied session or runner aliases. */
int ppp_runner_begin(struct ppp_session **session, struct ppp_runner **runner);
/* One session step at most and one nanosleep at most; no outer RunLoop dispatch.
 * NULL stop_flag adds no request; NULL wait_ms selects 10 ms in the body; explicit
 * waits must be 1..50 ms. EINTR is not re-slept. Requests are checked before step,
 * after step and after wait; already running synchronous work is not interrupted
 * by observing a flag later. 0 progressing, 1 local CLOSED, -1 invalid invocation
 * with status unchanged. status is required. CLOSED does not mean restored or
 * successful. Errors are separate/sticky; WAIT retains all resources. */
int ppp_runner_slice(struct ppp_runner *runner, const volatile sig_atomic_t *stop_flag,
                      const uint32_t *wait_ms, struct ppp_runner_status *status);
/* Latch stop BEFORE restoring a live session's saved actions, in reverse order.
 * Attempt every remaining item; retry failures only, never reinstall. 0 means
 * all actions restored, 1 means items remain, -1 rejects without output changes.
 * Does not step/free: continue slices even in WAIT. After restoration, subsequent
 * signals follow the old action (possibly default termination), not graceful
 * runner stop. Earlier error records remain even after successful retry. */
int ppp_runner_restore(struct ppp_runner *runner, struct ppp_runner_status *status);
/* CLOSED AND fully restored only. Release session then lease/object and clear
 * the same owning handle. NULL handle is a repeat no-op; NULL ** is invalid.
 * No timeout, Abort result or signal count permits early free. */
int ppp_runner_release(struct ppp_runner **runner);
/* Process pending signal flags without stepping/polling the child. Copy only.
 * Use immediately after first start slice, before any subsequent slice. */
int ppp_runner_observe(struct ppp_runner *runner, struct ppp_runner_status *status,
                       struct ppp_session_observation *observation);

#endif
