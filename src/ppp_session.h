#ifndef DJI_PPP_SESSION_H
#define DJI_PPP_SESSION_H

#include "ppp_usb.h"
#include "ppp_launch.h"

struct ppp_session;
enum ppp_session_state {
    PPP_SESSION_PREPARED, PPP_SESSION_RUNNING, PPP_SESSION_DISPOSING, PPP_SESSION_CLOSED
};
enum ppp_session_error {
    PPP_SESSION_ERROR_NONE, PPP_SESSION_ERROR_METADATA, PPP_SESSION_ERROR_ACQUIRE,
    PPP_SESSION_ERROR_BIND, PPP_SESSION_ERROR_START, PPP_SESSION_ERROR_SUPERVISOR,
    PPP_SESSION_ERROR_DISPOSE
};
/* Which fixed pppd policy the session launches. ONCE (0, the zero-initialized
 * default) keeps the single-shot ppp_supervisor_start_fixed() policy. */
enum ppp_launch_mode { PPP_LAUNCH_MODE_ONCE, PPP_LAUNCH_MODE_UP };
struct ppp_session_request {
    io_service_t service;
    IOUSBDeviceInterface **device;
    uint64_t registry_id;
    const struct descriptor_config *config;
    struct ppp_budgets budgets;
    size_t capacity;
    const uint32_t *timeout_ms;
    enum ppp_launch_mode launch;
    /* Explicit IN timeout. NULL derives it from the budgets (see start). */
    const uint32_t *read_timeout_ms;
};
struct ppp_session_status {
    enum ppp_session_state state;
    enum ppp_session_error error;
    int local_done, cleanup_error, stop_requested, supervisor_valid;
    /* Only meaningful when supervisor_valid; never read a failed start object. */
    enum ppp_phase supervisor_phase;
    enum ppp_error supervisor_error;
};
/* Copies only: no child/session pointer or file descriptor escapes. */
struct ppp_session_observation {
    pid_t pid;
    int started, reaped, ownership_lost, stdout_eof, stderr_eof, exec_done;
    int peer_quiet, peer_closed, stop_requested;
    unsigned term_signals, kill_signals;
    /* Diagnostics only (fixed labels/counts, never contents). exit_code is
     * meaningful when exited, term_signal when signaled. */
    int exited, exit_code, signaled, term_signal, exec_error;
    int connect_attempted, output_observed, echo_seen, modem_code_valid;
    enum offline_state stream_state;
    enum offline_error stream_error;
    enum offline_modem_result modem_result;
    unsigned modem_code;
    uint64_t child_output_bytes, usb_input_bytes, usb_input_reads, stderr_bytes, control_bytes;
    uint64_t usb_empty_polls, usb_validate_us, usb_write_us;
    /* First USB adapter error label as kept by the supervisor; NONE when no
     * error was recorded after binding. Diagnostics only. */
    enum ppp_usb_error_kind usb_error_kind;
    enum ppp_usb_error_site usb_error_site;
    /* Passive PPP negotiation counters and labels, copied by value; zero when
     * no valid supervisor existed. Never payload, options or addresses. */
    struct ppp_nego_summary negotiation;
};
int ppp_session_observe(struct ppp_session *session, struct ppp_session_observation *observation);
/* Internal single-use owner-thread/CFRunLoop API, serialized outside signal
 * handlers. *session must be NULL: a live output is rejected without mutation.
 * -1 rejects input/allocation; 0 accepts an object, NOT a connection or permit.
 * Metadata/acquire/bind run synchronously, without cancellation points or hard
 * SDK time bounds. Inputs are consumed before return; budgets/capacity copied.
 * NULL metadata/timeout use existing preflight/acquire defaults in their bodies.
 * timeout_ms is the OUT timeout only. With NULL read_timeout_ms the IN timeout
 * is derived here as connect_ms + session_ms + drain_ms + 1000; 0 or
 * > UINT32_MAX rejects (-1). A non-NULL read_timeout_ms is used as is and must
 * be nonzero. launch must be ONCE or UP; anything else rejects (-1). UP
 * requires an explicit read_timeout_ms (NULL rejects).
 * Operational failure returns an object: inspect status and step until CLOSED.
 * No discovery, restart, ready injection, signal loop or production CLI wiring. */
int ppp_session_start(const struct ppp_session_request *request,
                      const struct ppp_metadata_backend *metadata_backend,
                      struct ppp_session **session);
/* One start, stop delivery, supervisor step OR caller dispose per step.
 * 0 progressing, 1 locally CLOSED, -1 invalid invocation. Errors live in status,
 * independently of completion. Never abandon WAIT or free a live context. */
int ppp_session_step(struct ppp_session *session);
/* Latch only; repeated stop is harmless and never extends a deadline. */
int ppp_session_stop(struct ppp_session *session);
/* Explicit existing DRAIN notification only, not proof of LCP/AT restoration. */
int ppp_session_peer_done(struct ppp_session *session);
/* Same owner/reentry contract as mutations; rejected calls leave output intact. */
int ppp_session_snapshot(struct ppp_session *session, struct ppp_session_status *status);
/* CLOSED only; clears the same owning handle. NULL handle is a repeat no-op;
 * NULL pointer-to-pointer is invalid. Never reuse a copied stale handle. */
int ppp_session_release(struct ppp_session **session);

#endif
