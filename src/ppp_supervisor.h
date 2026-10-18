#ifndef DJI_PPP_SUPERVISOR_H
#define DJI_PPP_SUPERVISOR_H

#include "ppp_child.h"
#include "ppp_negotiation.h"

#define PPP_SUPERVISOR_BUFFER 4096u

enum ppp_phase { PPP_START, PPP_CONNECT, PPP_SESSION, PPP_DRAIN, PPP_CANCEL, PPP_CLEANUP, PPP_CLOSED };
enum ppp_error { PPP_ERROR_NONE, PPP_ERROR_CHILD, PPP_ERROR_USB,
                 PPP_ERROR_CORE, PPP_ERROR_TIMEOUT, PPP_ERROR_CLOCK, PPP_ERROR_CLOSE };
/* Fixed USB failure labels for diagnostics only; never raw IOReturn values. */
enum ppp_usb_error_kind {
    PPP_USB_ERROR_NONE, PPP_USB_ERROR_TIMEOUT, PPP_USB_ERROR_ABORTED, PPP_USB_ERROR_NO_DEVICE,
    PPP_USB_ERROR_NOT_OPEN, PPP_USB_ERROR_STALL, PPP_USB_ERROR_OVERRUN,
    PPP_USB_ERROR_ZERO_LENGTH, PPP_USB_ERROR_OVERSIZE, PPP_USB_ERROR_OTHER
};
enum ppp_usb_error_site {
    PPP_USB_SITE_NONE, PPP_USB_SITE_SUBMIT, PPP_USB_SITE_COMPLETION, PPP_USB_SITE_WRITE,
    PPP_USB_SITE_CONNECT, PPP_USB_SITE_ABORT, PPP_USB_SITE_VALIDATE
};
struct ppp_budgets { uint64_t start_ms, connect_ms, session_ms, drain_ms; };
struct ppp_quiet { int quiet, error; };
/* Serialized injected peer. AGAIN means no completed read, not a
 * failed USB transfer; any actual failed completion must return ERROR/INTR/EOF.
 * Successful read: 1..capacity. Failed results may contain poisoned count/data.
 * A write must complete the entire span; failures/partial are never retried.
 * now_ms is monotonic; UINT64_MAX denotes clock failure. close/release own one
 * peer; callbacks never reenter. quiesce/poll_quiet are an optional pair:
 * both NULL means no outstanding asynchronous work. quiesce stops submissions
 * and requests cancellation once; poll_quiet pumps completions, independently
 * reporting quiet and error. Only quiet proves every callback has returned. */
struct ppp_peer {
    void *context;
    struct offline_io (*read)(void *context, unsigned char *data, size_t capacity);
    struct offline_io (*write)(void *context, const unsigned char *data, size_t length);
    uint64_t (*now_ms)(void *context);
    int (*close)(void *context);
    void (*release)(void *context);
    int (*quiesce)(void *context);
    struct ppp_quiet (*poll_quiet)(void *context);
    /* Optional one-shot control request, before any peer read. 0 means the
     * request completed, not CONNECT or measured device reception. */
    int (*begin_connect)(void *context);
};
/* Caller serializes every call, outside signal handlers. Object/context live
 * through CLOSED; never mutate fields or reinitialize a live instance. Capacity
 * is 1..4096 for each core queue and pre-CONNECT TX. One retained USB read holds
 * its suffix until accepted, before the next read. start takes peer ownership
 * only on success. No application source: after stop caller/peer supplies only
 * opaque termination exchange (not interpreted as PPP/LCP or AT restoration).
 * EOF/reaped PID do not mean peer exchange complete. peer_done is explicit and
 * prohibits further USB reads; input closes only after accepted RX is delivered.
 * step is nonblocking except callbacks; deadlines cannot bound blocking peers.
 * SESSION stop sends SIGTERM once and keeps raw I/O alive. START/CONNECT stop
 * closes the I/O/readiness gate, marks recovery unknown through core stop, and
 * waits in CANCEL for child/pipe completion and peer quiet without peer_done or
 * drain_complete. Its return 0 accepts a request, not PPP success or restoration.
 * Repeated stop never extends the deadline. Failure/deadline sends SIGKILL
 * once, then caller must keep stepping until PID reaped/ownership_lost AND peer quiet (no
 * wall-clock promise for an uninterruptible process or missing callback).
 * Errors, Abort return and deadlines never imply quiet. stderr is discarded
 * only, never readiness.
 * Injected ready (legal during START/CONNECT) is NOT evidence that a production
 * pppd is ready. CONNECT and ready must both precede the core write gate. */
struct ppp_supervisor {
    struct ppp_child child;
    struct offline_stream stream;
    struct offline_queue pending_tx;
    struct ppp_peer peer;
    struct ppp_budgets budgets;
    enum ppp_phase phase;
    enum ppp_error error;
    uint64_t deadline_ms, last_ms, transport_deadline_ms;
    int error_cleanup_started, clock_failed;
    unsigned char rx_storage[PPP_SUPERVISOR_BUFFER], tx_storage[PPP_SUPERVISOR_BUFFER];
    unsigned char pending_storage[PPP_SUPERVISOR_BUFFER], usb_input[PPP_SUPERVISOR_BUFFER];
    size_t usb_length, usb_offset, stdout_remaining;
    int ready, connected, stdout_eof, peer_done, stopped, kill_sent, peer_closed;
    int quiesce_started, peer_quiet;
    int observed_start, output_observed, connect_attempted;
    int cancel_requested, cancel_core_expected, term_sent;
    /* Diagnostics only, never contents: bytes read from the child's stdout and
     * bytes of completed USB reads, counted at read time whether or not the
     * queue/stream later accepts them. */
    uint64_t child_output_bytes, usb_input_bytes;
    /* Diagnostics only: completed USB reads counted with usb_input_bytes. */
    uint64_t usb_input_reads;
    /* Diagnostics only, never used for any decision: the first USB adapter
     * error label, written by the bound adapter. Kept here because the adapter
     * is freed before the supervisor and must not be read afterwards. */
    enum ppp_usb_error_kind usb_error_kind;
    enum ppp_usb_error_site usb_error_site;
    /* Diagnostics only, never used for any decision: a passive classifier fed
     * copies of the RX bytes the core queued after CONNECT and of the TX bytes
     * the core accepted from pending_tx. It never alters, answers or delays
     * the relay; control text before CONNECT is never fed to it. */
    struct ppp_negotiation negotiation;
};
int ppp_supervisor_start(struct ppp_supervisor *supervisor, const struct ppp_peer *peer,
                         const struct ppp_budgets *budgets, size_t capacity,
                         const char *path, char *const argv[], char *const envp[], const char *cwd);
/* Same explicit transport contract, but readiness requires the first successful
 * stdout read accepted into pending_tx AND exec completion. begin_connect is
 * required; injected ready cannot bypass this gate. Not installation approval. */
int ppp_supervisor_start_observed(struct ppp_supervisor *supervisor, const struct ppp_peer *peer,
                                  const struct ppp_budgets *budgets, size_t capacity,
                                  const char *path, char *const argv[], char *const envp[], const char *cwd);
/* Observed start with fixed ppp_launch_prepare() policy (path /usr/sbin/pppd,
 * cwd /, envp {NULL}, fixed argv). No caller path/argv/env/cwd. begin_connect
 * is required; a missing begin_connect returns -1 without creating a child or
 * taking peer ownership. Not an execution permit; ppp_launch_preflight is not
 * called here — the default backend hits host lstat/getpwuid. */
int ppp_supervisor_start_fixed(struct ppp_supervisor *supervisor, const struct ppp_peer *peer,
                                const struct ppp_budgets *budgets, size_t capacity);
/* Same observed start, with the fixed ppp_launch_prepare_up() policy instead.
 * Identical begin_connect requirement; no caller path/argv/env/cwd. */
int ppp_supervisor_start_up(struct ppp_supervisor *supervisor, const struct ppp_peer *peer,
                            const struct ppp_budgets *budgets, size_t capacity);
int ppp_supervisor_ready(struct ppp_supervisor *supervisor);
int ppp_supervisor_stop(struct ppp_supervisor *supervisor);
int ppp_supervisor_peer_done(struct ppp_supervisor *supervisor);
/* Returns 0 while progressing, 1 at CLOSED, -1 on invalid invocation.
 * Terminal error remains in error; cleanup must finish even after failure. */
int ppp_supervisor_step(struct ppp_supervisor *supervisor);
int ppp_supervisor_diagnostic(const struct ppp_supervisor *supervisor, char *buffer, size_t capacity);

#endif
