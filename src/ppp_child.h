#ifndef DJI_PPP_CHILD_H
#define DJI_PPP_CHILD_H

#include "offline_stream.h"
#include <sys/types.h>

/* Internal local transport, NOT a pppd launcher. All calls are serialized, not
 * signal-handler safe. Caller must not reap this PID or ignore SIGCHLD, and must
 * serialize process creation/fd changes with start (pipe+fcntl is not atomic).
 * init once, start once; poll until reaped or ownership_lost, then close.
 * No shell or inherited env.
 * argv/envp need explicit NULL terminators; path and cwd must be absolute.
 * Visible fields permit stack allocation/observation, never caller mutation. */
struct ppp_child {
    pid_t pid;
    int input_fd, output_fd, error_fd, exec_fd;
    int started, exec_done, exec_error, reaped, ownership_lost, wait_status, io_error, closed;
    int stdout_eof, stderr_eof, transport_abandoned;
    unsigned char exec_bytes[sizeof(int)];
    size_t exec_used;
    uint64_t stderr_bytes, stdout_discarded;
    unsigned fd_closes, reap_count, signals, term_signals, kill_signals;
};
void ppp_child_init(struct ppp_child *child);
int ppp_child_start(struct ppp_child *child, const char *path, char *const argv[],
                    char *const envp[], const char *cwd);
/* Nonblocking: poll handshake/reap and discard at most 4096 stderr bytes/call.
 * Reaping does NOT close stdout: queued output must still be drained to EOF. */
int ppp_child_poll(struct ppp_child *child);
/* Handshake/wait only: signal and repeated readiness checks do not spend a
 * second stderr discard allowance in the same supervisor step. */
int ppp_child_poll_status(struct ppp_child *child);
struct offline_io ppp_child_read(struct ppp_child *child, unsigned char *data, size_t capacity);
struct offline_io ppp_child_write(struct ppp_child *child, const unsigned char *data, size_t length);
/* Counters advance only after kill succeeds, not a reaped/ownership-lost no-op. */
int ppp_child_signal(struct ppp_child *child, int signal_number);
void ppp_child_end_input(struct ppp_child *child);
/* Idempotent fd cleanup, never kills or waits; fails while PID is still owned.
 * Closing without observed EOF marks transport_abandoned, never synthesizes EOF. */
int ppp_child_close(struct ppp_child *child);
int ppp_child_diagnostic(const struct ppp_child *child, char *buffer, size_t capacity);

#endif
