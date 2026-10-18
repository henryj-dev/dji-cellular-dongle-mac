#define _DARWIN_C_SOURCE
#include "../src/ppp_child.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL child line=%d\n", __LINE__); failures++; } } while (0)
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void wait_io(struct ppp_child *c) {
    struct pollfd fds[] = {{c->output_fd, POLLIN, 0}, {c->error_fd, POLLIN, 0},
                          {c->exec_fd, POLLIN, 0}};
    (void)poll(fds, 3, 1);
}
static int collect(struct ppp_child *c, unsigned char *data, size_t wanted) {
    uint64_t deadline = now() + 3000;
    size_t used = 0;
    while (used < wanted && now() < deadline) {
        (void)ppp_child_poll(c);
        struct offline_io r = ppp_child_read(c, data + used, wanted - used);
        if (r.code == OFFLINE_IO_OK) used += r.count;
        else if (r.code != OFFLINE_IO_AGAIN && r.code != OFFLINE_IO_INTR) break;
        wait_io(c);
    }
    return used == wanted;
}
static int reap(struct ppp_child *c) {
    uint64_t deadline = now() + 3000;
    while ((!c->reaped || c->exec_fd >= 0 || c->error_fd >= 0) && now() < deadline) {
        (void)ppp_child_poll(c); wait_io(c);
    }
    return c->reaped && c->exec_fd < 0 && c->error_fd < 0;
}
static int fd_count(void) {
    int count = 0;
    for (int i = 0; i < 1024; i++) if (fcntl(i, F_GETFD) >= 0) count++;
    return count;
}
static void start(struct ppp_child *c, const char *path, char *mode) {
    char *args[] = {(char *)path, mode, NULL};
    char *env[] = {"PPP_FIXTURE=explicit", NULL};
    ppp_child_init(c);
    CHECK(ppp_child_start(c, path, args, env, "/") == 0);
}
static void finish(struct ppp_child *c) {
    CHECK(ppp_child_signal(c, SIGKILL) == 0);
    CHECK(reap(c));
    CHECK(c->reap_count == 1);
    int status;
    errno = 0;
    CHECK(waitpid(c->pid, &status, WNOHANG) == -1 && errno == ECHILD);
    CHECK(ppp_child_close(c) == (c->io_error ? -1 : 0));
    unsigned closes = c->fd_closes, signals = c->signals;
    CHECK(ppp_child_close(c) == (c->io_error ? -1 : 0));
    CHECK(ppp_child_signal(c, SIGTERM) == -1 && c->signals == signals);
    CHECK(c->fd_closes == closes && closes == 4);
}
static void context_test(const char *path) {
    int baseline = fd_count(), extra = open("/dev/null", O_RDONLY);
    CHECK(extra >= 3);
    char number[32];
    (void)snprintf(number, sizeof(number), "%d", extra);
    char *args[] = {(char *)path, "context", "/", number, NULL};
    char *env[] = {"PPP_FIXTURE=explicit", NULL};
    struct ppp_child c;
    ppp_child_init(&c);
    CHECK(ppp_child_start(&c, path, args, env, "/") == 0);
    CHECK(fcntl(c.input_fd, F_GETFD) & FD_CLOEXEC);
    CHECK(fcntl(c.output_fd, F_GETFL) & O_NONBLOCK);
    unsigned char byte;
    CHECK(collect(&c, &byte, 1) && byte == 'R');
    CHECK(reap(&c) && c.exec_done && !c.exec_error && WEXITSTATUS(c.wait_status) == 0);
    finish(&c);
    CHECK(close(extra) == 0 && fd_count() == baseline);
}
static void failures_test(const char *path) {
    struct ppp_child c;
    ppp_child_init(&c);
    char *args[] = {(char *)path, "exit0", NULL}, *env[] = {NULL};
    CHECK(ppp_child_start(&c, path, args, env, "relative") == -1 && !c.started);
    CHECK(ppp_child_start(&c, path, args, NULL, "/") == -1 && !c.started);
    CHECK(ppp_child_start(&c, "/nonexistent-ppp-child-fixture", args, env, "/") == 0);
    CHECK(reap(&c) && c.exec_done && c.exec_error == ENOENT && WEXITSTATUS(c.wait_status) == 127);
    finish(&c);
    ppp_child_init(&c);
    CHECK(ppp_child_start(&c, path, args, env, "/nonexistent-ppp-child-cwd") == 0);
    CHECK(reap(&c) && c.exec_error == ENOENT);
    finish(&c);
    start(&c, path, "exit7");
    CHECK(reap(&c) && c.exec_done && !c.exec_error && WEXITSTATUS(c.wait_status) == 7);
    unsigned char bytes[8];
    CHECK(collect(&c, bytes, 3) && !memcmp(bytes, "RDY", 3)); /* Read AFTER reap. */
    CHECK(ppp_child_read(&c, bytes, sizeof(bytes)).code == OFFLINE_IO_EOF);
    finish(&c);
}
static void sigpipe_test(const char *path) {
    struct ppp_child c;
    start(&c, path, "noinput");
    unsigned char bytes[3];
    CHECK(collect(&c, bytes, 3)); /* Child closed stdin before its explicit RDY. */
    struct sigaction before, after;
    CHECK(sigaction(SIGPIPE, NULL, &before) == 0);
    errno = 0;
    struct offline_io first = ppp_child_write(&c, bytes, 3);
    int first_errno = errno;
    if (first.code != OFFLINE_IO_ERROR || !c.io_error)
        fprintf(stderr, "sigpipe first code=%d count=%zu errno=%d io_error=%d input_fd=%d\n",
                first.code, first.count, first_errno, c.io_error, c.input_fd);
    CHECK(first.code == OFFLINE_IO_ERROR && c.io_error);
    CHECK(sigaction(SIGPIPE, NULL, &after) == 0 && before.sa_handler == after.sa_handler);
    sigset_t block, old, pending;
    sigemptyset(&block); sigaddset(&block, SIGPIPE);
    CHECK(sigprocmask(SIG_BLOCK, &block, &old) == 0);
    CHECK(raise(SIGPIPE) == 0);
    CHECK(ppp_child_write(&c, bytes, 3).code == OFFLINE_IO_ERROR);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE));
    int caught;
    CHECK(sigwait(&block, &caught) == 0 && caught == SIGPIPE);
    CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
    finish(&c);
}
static void backpressure_test(const char *path) {
    struct ppp_child c;
    start(&c, path, "ignore");
    unsigned char ready[4];
    static unsigned char bytes[1048576];
    memset(bytes, 0x7e, sizeof(bytes));
    CHECK(collect(&c, ready, sizeof(ready)) && !memcmp(ready, "RDYI", 4));
    int partial = 0, blocked = 0;
    uint64_t deadline = now() + 3000;
    while (!blocked && now() < deadline) {
        struct offline_io r = ppp_child_write(&c, bytes, sizeof(bytes));
        if (r.code == OFFLINE_IO_OK) partial |= r.count < sizeof(bytes);
        else if (r.code == OFFLINE_IO_AGAIN) blocked = 1;
        else break;
    }
    CHECK(partial && blocked && !c.io_error);
    CHECK(ppp_child_close(&c) == -1 && !c.closed);
    finish(&c);
}
static void echo_test(const char *path) {
    struct ppp_child c;
    start(&c, path, "split");
    unsigned char ready[3], source[4096], received[4096];
    CHECK(collect(&c, ready, 3) && !memcmp(ready, "RDY", 3));
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (unsigned char)i;
    size_t sent = 0, got = 0;
    uint64_t deadline = now() + 3000;
    int eof = 0;
    while (!eof && now() < deadline) {
        if (sent < sizeof(source)) {
            struct offline_io r = ppp_child_write(&c, source + sent, sizeof(source) - sent);
            if (r.code == OFFLINE_IO_OK) sent += r.count;
            else if (r.code != OFFLINE_IO_AGAIN && r.code != OFFLINE_IO_INTR) break;
        } else ppp_child_end_input(&c);
        unsigned char chunk[113];
        struct offline_io r = ppp_child_read(&c, chunk, sizeof(chunk));
        if (r.code == OFFLINE_IO_OK) {
            if (r.count > sizeof(received) - got) break;
            memcpy(received + got, chunk, r.count); got += r.count;
        } else if (r.code == OFFLINE_IO_EOF) eof = 1;
        else if (r.code != OFFLINE_IO_AGAIN && r.code != OFFLINE_IO_INTR) break;
        (void)ppp_child_poll(&c); wait_io(&c);
    }
    CHECK(eof && got == sizeof(source) && sent == got && !memcmp(source, received, got));
    CHECK(reap(&c) && WIFEXITED(c.wait_status) && WEXITSTATUS(c.wait_status) == 0);
    finish(&c);
}
static void burst_test(const char *path) {
    struct ppp_child c;
    start(&c, path, "burst");
    size_t got = 0;
    int valid = 1, eof = 0;
    uint64_t deadline = now() + 3000;
    while (!eof && now() < deadline) {
        unsigned char data[991];
        struct offline_io r = ppp_child_read(&c, data, sizeof(data));
        if (r.code == OFFLINE_IO_OK) {
            for (size_t i = 0; i < r.count; i++) if (data[i] != (unsigned char)(got + i)) valid = 0;
            got += r.count;
        } else if (r.code == OFFLINE_IO_EOF) eof = 1;
        else if (r.code != OFFLINE_IO_AGAIN && r.code != OFFLINE_IO_INTR) break;
        (void)ppp_child_poll(&c); wait_io(&c);
    }
    CHECK(eof && valid && got == 65536);
    finish(&c);
    start(&c, path, "diagnostic");
    unsigned char ready[3];
    CHECK(collect(&c, ready, 3) && !memcmp(ready, "RDY", 3));
    uint64_t stderr_deadline = now() + 3000;
    while (c.stderr_bytes < 131072 && now() < stderr_deadline) {
        (void)ppp_child_poll(&c); wait_io(&c);
    }
    CHECK(c.stderr_bytes == 131072);
    uint64_t discarded = c.stderr_bytes;
    CHECK(ppp_child_signal(&c, SIGTERM) == 0 && c.stderr_bytes == discarded);
    char diagnostic[512], tiny[2];
    int length = ppp_child_diagnostic(&c, diagnostic, sizeof(diagnostic));
    CHECK(length > 0 && !strstr(diagnostic, "private-child-marker") && !strstr(diagnostic, "RDY"));
    CHECK(ppp_child_diagnostic(&c, tiny, sizeof(tiny)) == length && tiny[1] == 0);
    CHECK(ppp_child_diagnostic(&c, NULL, 0) == length);
    CHECK(ppp_child_diagnostic(&c, NULL, 1) == -1);
    finish(&c);
}
static void eof_close_test(const char *path) {
    struct ppp_child c;
    unsigned char bytes[8];
    start(&c, path, "exit0");
    CHECK(reap(&c));
    CHECK(!c.stdout_eof && c.stderr_eof && !c.transport_abandoned);
    CHECK(ppp_child_close(&c) == 0 && c.transport_abandoned && !c.stdout_eof);
    CHECK(ppp_child_read(&c, bytes, sizeof(bytes)).code == OFFLINE_IO_ERROR);
    start(&c, path, "exit0");
    CHECK(collect(&c, bytes, 3));
    uint64_t deadline = now() + 3000;
    struct offline_io r = {OFFLINE_IO_AGAIN, 0};
    while (r.code == OFFLINE_IO_AGAIN && now() < deadline) {
        r = ppp_child_read(&c, bytes, sizeof(bytes)); wait_io(&c);
    }
    CHECK(r.code == OFFLINE_IO_EOF && c.stdout_eof);
    CHECK(reap(&c) && c.stderr_eof);
    CHECK(ppp_child_close(&c) == 0 && !c.transport_abandoned);
    CHECK(ppp_child_read(&c, bytes, sizeof(bytes)).code == OFFLINE_IO_EOF);
}
static void ownership_test(const char *path) {
    struct ppp_child c;
    start(&c, path, "exit0");
    /* Deliberate caller-contract violation: reap ONLY our direct child outside
     * the transport to exercise ECHILD, never a grandchild waitpid claim. */
    int status = 0;
    pid_t result;
    do { result = waitpid(c.pid, &status, 0); } while (result < 0 && errno == EINTR);
    CHECK(result == c.pid && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(ppp_child_poll(&c) == -1 && c.ownership_lost && !c.reaped && !c.reap_count);
    unsigned signals = c.signals;
    CHECK(ppp_child_signal(&c, SIGKILL) == 0 && c.signals == signals);
    ppp_child_end_input(&c);
    CHECK(ppp_child_close(&c) == -1 && c.closed && c.transport_abandoned);
}
static void holder_test(const char *path) {
    const char *modes[] = {"holder-stderr", "holder-stdin", "holder-ignore", "holder-burst"};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        struct ppp_child c;
        start(&c, path, (char *)modes[i]);
        unsigned char ready[3];
        CHECK(collect(&c, ready, sizeof(ready)) && !memcmp(ready, "RDY", 3));
        uint64_t deadline = now() + 3000;
        int eof = 0;
        size_t output = 0;
        if (i != 3) {
            while (!eof && now() < deadline) {
                unsigned char bytes[128];
                struct offline_io r = ppp_child_read(&c, bytes, sizeof(bytes));
                eof = r.code == OFFLINE_IO_EOF;
                (void)ppp_child_poll(&c); wait_io(&c);
            }
            CHECK(eof && !c.reaped);
            if (i != 1) CHECK(!c.stderr_eof); /* stdout closed, stderr still held. */
        }
        /* EOF on this control input releases the main-owned holder; the main
         * waits for it, so a zero main status also checks watchdog did NOT fire. */
        ppp_child_end_input(&c);
        while ((!c.reaped || !c.stdout_eof || !c.stderr_eof) && now() < deadline) {
            unsigned char bytes[4096];
            struct offline_io r = ppp_child_read(&c, bytes, sizeof(bytes));
            if (r.code == OFFLINE_IO_OK) output += r.count;
            uint64_t before = c.stderr_bytes;
            (void)ppp_child_poll(&c);
            CHECK(c.stderr_bytes - before <= 4096);
            wait_io(&c);
        }
        CHECK(c.reaped && c.stdout_eof && c.stderr_eof && c.exec_done);
        CHECK(WIFEXITED(c.wait_status) && WEXITSTATUS(c.wait_status) == 0);
        if (i == 3) CHECK(output == 131072 && c.stderr_bytes == 131072);
        if (!c.reaped) { /* Failure paths still close control before direct kill. */
            CHECK(ppp_child_signal(&c, SIGKILL) == 0); CHECK(reap(&c));
        }
        CHECK(ppp_child_close(&c) == 0 && !c.transport_abandoned);
    }
}
int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') return 2;
    int baseline = fd_count();
    context_test(argv[1]); failures_test(argv[1]); sigpipe_test(argv[1]);
    backpressure_test(argv[1]); echo_test(argv[1]); burst_test(argv[1]);
    eof_close_test(argv[1]); ownership_test(argv[1]); holder_test(argv[1]);
    for (unsigned i = 0; i < 8; i++) {
        struct ppp_child c;
        start(&c, argv[1], "exit0"); CHECK(reap(&c)); finish(&c);
    }
    CHECK(fd_count() == baseline);
    printf("ppp_child real pipe/fixture checks=%u failures=%u (no device/network/pppd)\n", checks, failures);
    return failures ? 1 : 0;
}
