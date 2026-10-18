#define _DARWIN_C_SOURCE
#include "ppp_child.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libproc.h>
#include <stdlib.h>
#include <sys/proc_info.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void close_fd(struct ppp_child *c, int *fd) {
    if (*fd < 0) return;
    int owned = *fd;
    *fd = -1; /* Never retry close(EINTR): the number may have been reused. */
    c->fd_closes++;
    if (close(owned)) c->io_error = 1;
}

void ppp_child_init(struct ppp_child *c) {
    if (c) *c = (struct ppp_child){.input_fd = -1, .output_fd = -1,
                                .error_fd = -1, .exec_fd = -1};
}

static int make_pipe(int fds[2]) {
    if (pipe(fds)) return -1;
    for (unsigned i = 0; i < 2; i++) {
        if (fds[i] < 3) {
            int copy = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
            if (copy < 0) goto failed;
            (void)close(fds[i]);
            fds[i] = copy;
        }
        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC)) goto failed;
    }
    return 0;
failed:
    (void)close(fds[0]); (void)close(fds[1]);
    fds[0] = fds[1] = -1;
    return -1;
}

static void exec_failed(int fd, int error) {
    const unsigned char *bytes = (const unsigned char *)&error;
    size_t offset = 0;
    while (offset < sizeof(error)) {
        ssize_t n = write(fd, bytes + offset, sizeof(error) - offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    _exit(127);
}

int ppp_child_start(struct ppp_child *c, const char *path, char *const argv[],
                    char *const envp[], const char *cwd) {
    if (!c || c->started || c->closed || !path || path[0] != '/' ||
        !argv || !argv[0] || !envp || !cwd || cwd[0] != '/') return -1;
    struct sigaction chld;
    if (sigaction(SIGCHLD, NULL, &chld) || chld.sa_handler == SIG_IGN ||
        (chld.sa_flags & SA_NOCLDWAIT)) return -1;
    int pipes[4][2] = {{-1,-1}, {-1,-1}, {-1,-1}, {-1,-1}};
    for (unsigned i = 0; i < 4; i++) {
        if (make_pipe(pipes[i])) goto failed;
        int parent = pipes[i][i == 0 ? 1 : 0];
        int flags = fcntl(parent, F_GETFL);
        if (flags < 0 || fcntl(parent, F_SETFL, flags | O_NONBLOCK)) goto failed;
    }
    /* Snapshot all open descriptors, including those above a lowered soft fd
     * limit. Caller serializes fd creation/closure until fork completes. */
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, NULL, 0);
    if (bytes <= 0 || bytes > 0x7fffffff - 8 * (int)sizeof(struct proc_fdinfo)) goto failed;
    size_t fd_bytes = (size_t)bytes + 8 * sizeof(struct proc_fdinfo);
    struct proc_fdinfo *open_fds = malloc(fd_bytes);
    if (!open_fds) goto failed;
    bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, open_fds, (int)fd_bytes);
    if (bytes <= 0 || (size_t)bytes >= fd_bytes || bytes % sizeof(*open_fds)) {
        free(open_fds); goto failed;
    }
    size_t fd_count = (size_t)bytes / sizeof(*open_fds);
    pid_t pid = fork();
    if (pid < 0) { free(open_fds); goto failed; }
    if (!pid) {
        /* Only async-signal-safe operations in the post-fork child. */
        int handshake = pipes[3][1];
        if (dup2(pipes[0][0], STDIN_FILENO) < 0 ||
            dup2(pipes[1][1], STDOUT_FILENO) < 0 ||
            dup2(pipes[2][1], STDERR_FILENO) < 0) exec_failed(handshake, errno);
        if (dup2(handshake, 3) < 0) exec_failed(handshake, errno);
        if (fcntl(3, F_SETFD, FD_CLOEXEC)) exec_failed(3, errno);
        for (size_t i = 0; i < fd_count; i++)
            if (open_fds[i].proc_fd >= 4 && close(open_fds[i].proc_fd))
                exec_failed(3, errno);
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGPIPE, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
            sigaction(SIGTERM, &action, NULL)) exec_failed(3, errno);
        sigset_t empty;
        sigemptyset(&empty);
        if (sigprocmask(SIG_SETMASK, &empty, NULL) || chdir(cwd)) exec_failed(3, errno);
        execve(path, argv, envp);
        exec_failed(3, errno);
    }
    free(open_fds);
    c->pid = pid;
    c->started = 1;
    c->input_fd = pipes[0][1]; c->output_fd = pipes[1][0];
    c->error_fd = pipes[2][0]; c->exec_fd = pipes[3][0];
    for (unsigned i = 0; i < 4; i++) (void)close(pipes[i][i == 0 ? 0 : 1]);
    return 0;
failed:
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 2; j++)
        if (pipes[i][j] >= 0) (void)close(pipes[i][j]);
    return -1;
}

static struct offline_io io_result(ssize_t n) {
    if (n > 0) return (struct offline_io){OFFLINE_IO_OK, (size_t)n};
    if (!n) return (struct offline_io){OFFLINE_IO_EOF, 0};
    return (struct offline_io){errno == EINTR ? OFFLINE_IO_INTR :
        (errno == EAGAIN || errno == EWOULDBLOCK) ? OFFLINE_IO_AGAIN : OFFLINE_IO_ERROR, 0};
}

struct offline_io ppp_child_read(struct ppp_child *c, unsigned char *data, size_t capacity) {
    if (!c || !c->started || !data || !capacity)
        return (struct offline_io){OFFLINE_IO_ERROR, 0};
    if (c->stdout_eof) return (struct offline_io){OFFLINE_IO_EOF, 0};
    if (c->closed || c->output_fd < 0) return (struct offline_io){OFFLINE_IO_ERROR, 0};
    struct offline_io result = io_result(read(c->output_fd, data, capacity));
    if (result.code == OFFLINE_IO_EOF) {
        c->stdout_eof = 1; close_fd(c, &c->output_fd);
    }
    if (result.code == OFFLINE_IO_ERROR) c->io_error = 1;
    return result;
}

struct offline_io ppp_child_write(struct ppp_child *c, const unsigned char *data, size_t length) {
    if (!c || !c->started || c->closed || c->input_fd < 0 || !data || !length)
        return (struct offline_io){OFFLINE_IO_ERROR, 0};
    /* Thread-local mask; do not change global SIGPIPE disposition or consume an
     * already pending signal. Only the SIGPIPE generated by our EPIPE is drained. */
    sigset_t set, old, pending;
    sigemptyset(&set); sigaddset(&set, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set, &old)) return (struct offline_io){OFFLINE_IO_ERROR, 0};
    if (sigpending(&pending)) {
        (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
        return (struct offline_io){OFFLINE_IO_ERROR, 0};
    }
    int was_pending = sigismember(&pending, SIGPIPE);
    ssize_t n = write(c->input_fd, data, length);
    int saved = errno;
    struct offline_io result = io_result(n);
    if (n < 0 && saved == EPIPE && !was_pending && !sigpending(&pending) &&
        sigismember(&pending, SIGPIPE)) {
        int caught;
        (void)sigwait(&set, &caught);
    }
    if (pthread_sigmask(SIG_SETMASK, &old, NULL)) result = (struct offline_io){OFFLINE_IO_ERROR, 0};
    if (result.code == OFFLINE_IO_ERROR || result.code == OFFLINE_IO_EOF) c->io_error = 1;
    return result;
}

int ppp_child_poll_status(struct ppp_child *c) {
    if (!c || !c->started) return -1;
    if (c->closed) return c->io_error || c->exec_error ? -1 : 0;
    if (c->exec_fd >= 0) {
        unsigned char byte;
        unsigned char *target = c->exec_used < sizeof(int) ? c->exec_bytes + c->exec_used : &byte;
        size_t capacity = c->exec_used < sizeof(int) ? sizeof(int) - c->exec_used : 1;
        ssize_t n = read(c->exec_fd, target, capacity);
        if (n > 0) {
            c->exec_used += (size_t)n;
            if (c->exec_used == sizeof(int)) {
                memcpy(&c->exec_error, c->exec_bytes, sizeof(int));
                if (!c->exec_error) c->exec_error = EIO;
                c->exec_done = 1; close_fd(c, &c->exec_fd);
            }
        } else if (!n) {
            c->exec_done = 1;
            if (c->exec_used) c->exec_error = EIO;
            close_fd(c, &c->exec_fd);
        } else if (errno != EAGAIN && errno != EINTR) {
            c->io_error = 1; close_fd(c, &c->exec_fd);
        }
    }
    if (!c->reaped && !c->ownership_lost) {
        int status;
        pid_t result = waitpid(c->pid, &status, WNOHANG);
        if (result == c->pid) {
            c->reaped = 1; c->wait_status = status; c->reap_count++;
        } else if (result < 0 && errno != EINTR) {
            /* Only ECHILD ends local PID ownership; other errors prove no reap. */
            if (errno == ECHILD) c->ownership_lost = 1;
            c->io_error = 1;
        }
    }
    return c->io_error || c->exec_error ? -1 : 0;
}

int ppp_child_poll(struct ppp_child *c) {
    if (!c || !c->started) return -1;
    (void)ppp_child_poll_status(c);
    if (!c->closed && c->error_fd >= 0) {
        unsigned char discard[1024];
        for (unsigned i = 0; i < 4; i++) {
            ssize_t n = read(c->error_fd, discard, sizeof(discard));
            if (n > 0) {
                if ((uint64_t)n > UINT64_MAX - c->stderr_bytes) c->io_error = 1;
                else c->stderr_bytes += (uint64_t)n;
                memset(discard, 0, sizeof(discard));
            } else {
                if (!n) { c->stderr_eof = 1; close_fd(c, &c->error_fd); }
                else if (errno != EAGAIN && errno != EINTR) {
                    c->io_error = 1; close_fd(c, &c->error_fd);
                }
                break;
            }
        }
    }
    return c->io_error || c->exec_error ? -1 : 0;
}

int ppp_child_signal(struct ppp_child *c, int number) {
    if (!c || !c->started || c->closed || (number != SIGINT && number != SIGTERM && number != SIGKILL)) return -1;
    (void)ppp_child_poll_status(c);
    if (c->reaped || c->ownership_lost) return 0;
    if (kill(c->pid, number)) {
        if (errno == ESRCH) return ppp_child_poll_status(c);
        c->io_error = 1; return -1;
    }
    c->signals++;
    if (number == SIGTERM) c->term_signals++;
    if (number == SIGKILL) c->kill_signals++;
    return 0;
}

void ppp_child_end_input(struct ppp_child *c) { if (c) close_fd(c, &c->input_fd); }

int ppp_child_close(struct ppp_child *c) {
    if (!c || (c->started && !c->reaped && !c->ownership_lost)) return -1;
    if (!c->closed) {
        if (c->started && (!c->stdout_eof || !c->stderr_eof || !c->exec_done))
            c->transport_abandoned = 1;
        close_fd(c, &c->input_fd); close_fd(c, &c->output_fd);
        close_fd(c, &c->error_fd); close_fd(c, &c->exec_fd);
        c->closed = 1;
    }
    return c->io_error ? -1 : 0;
}

int ppp_child_diagnostic(const struct ppp_child *c, char *buffer, size_t capacity) {
    if (!c || (!buffer && capacity)) return -1;
    int exited = c->reap_count && WIFEXITED(c->wait_status);
    int signaled = c->reap_count && WIFSIGNALED(c->wait_status);
    return snprintf(buffer, capacity,
        "child exec=%s reaped=%d ownership_lost=%d exit=%d signal=%d stdout_eof=%d stderr_eof=%d transport_abandoned=%d stdout_discarded=%" PRIu64 " stderr_discarded=%" PRIu64 " io_error=%d closed=%d descendants=unproven",
        !c->exec_done ? "pending" : c->exec_error ? "failed" : "ended",
        c->reaped, c->ownership_lost, exited ? WEXITSTATUS(c->wait_status) : -1,
        signaled ? WTERMSIG(c->wait_status) : 0, c->stdout_eof, c->stderr_eof,
        c->transport_abandoned, c->stdout_discarded, c->stderr_bytes, c->io_error, c->closed);
}
