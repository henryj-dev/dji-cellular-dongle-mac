#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_handler(int number) { (void)number; stopping = 1; }
static int output(int fd, const unsigned char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}
/* Every fork fixture has an independent alarm, including while write blocks or
 * TERM is ignored. The sole main-first mode intentionally reparents its holder;
 * only the surviving main in the other modes waits for its own direct child. */
#define HOLDER_HOLD_MS 2000u
#define HOLDER_WATCHDOG_SECONDS 8u
#define HOLDER_PARENT_HOLD_MS 50u
static void watchdog(int number) { (void)number; _exit(100); }
static uint64_t monotonic_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(101);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void finite_hold(unsigned milliseconds) {
    uint64_t deadline = monotonic_ms() + milliseconds;
    while (monotonic_ms() < deadline) (void)poll(NULL, 0, 1);
}
static int holder_fixture(const char *mode) {
    int ready[2], control[2];
    if (pipe(ready) || pipe(control)) return 102;
    struct sigaction action = {.sa_handler = watchdog};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) return 103;
    alarm(HOLDER_WATCHDOG_SECONDS);
    pid_t holder = fork();
    if (holder < 0) return 104;
    if (!holder) {
        alarm(HOLDER_WATCHDOG_SECONDS); /* alarm is not inherited across fork. */
        (void)close(ready[0]); (void)close(control[1]);
        int early = !strcmp(mode, "holder-exit");
        if (!strcmp(mode, "holder-stdin")) { (void)close(1); (void)close(2); }
        else if (!early && strcmp(mode, "holder-burst")) (void)close(1);
        if (!strcmp(mode, "holder-ignore")) {
            action.sa_handler = SIG_IGN;
            if (sigaction(SIGTERM, &action, NULL)) _exit(105);
        }
        /* Internal readiness proves stdio possession and installed watchdog. */
        if (output(ready[1], (const unsigned char *)"H", 1)) _exit(106);
        (void)close(ready[1]);
        if (!strcmp(mode, "holder-burst")) {
            unsigned char go;
            ssize_t n;
            do { n = read(control[0], &go, 1); } while (n < 0 && errno == EINTR);
            if (n != 1 || go != 'B') _exit(107);
            unsigned char bytes[1024]; memset(bytes, 0xa5, sizeof(bytes));
            for (unsigned i = 0; i < 128; i++)
                if (output(1, bytes, sizeof(bytes)) || output(2, bytes, sizeof(bytes))) _exit(107);
        }
        unsigned char byte;
        /* Only the main reads X. The holder starts reading stdin after the
         * main has closed control, so it cannot steal the exit request. */
        if (early) for (;;) {
            ssize_t n = read(control[0], &byte, 1);
            if (!n) break;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) _exit(108);
        }
        int fd = early ? 0 : control[0];
        for (;;) {
            ssize_t n = read(fd, &byte, 1);
            if (!n) break;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) _exit(108);
        }
        /* The main-first holder cannot race stdin close: EOF STARTS the hold. */
        finite_hold(early ? HOLDER_HOLD_MS : HOLDER_PARENT_HOLD_MS);
        _exit(0);
    }
    (void)close(ready[1]); (void)close(control[0]);
    unsigned char byte;
    ssize_t n;
    do { n = read(ready[0], &byte, 1); } while (n < 0 && errno == EINTR);
    (void)close(ready[0]);
    int result = n == 1 && byte == 'H' ? 0 : 109;
    if (!result && output(1, (const unsigned char *)"RDY", 3)) result = 110;
    if (!result && !strcmp(mode, "holder-burst") &&
        output(control[1], (const unsigned char *)"B", 1)) result = 110;
    int early = !strcmp(mode, "holder-exit");
    if (!early) (void)close(1);
    if (!result) for (;;) {
        do { n = read(0, &byte, 1); } while (n < 0 && errno == EINTR);
        if (early && n == 1 && byte == 'X') {
            (void)close(control[1]);
            return 0; /* The only intentional main-before-holder exit. */
        }
        if (!n) break;
        if (n < 0) { result = 111; break; }
    }
    (void)close(control[1]); /* Also releases holder on every ordinary failure. */
    int status;
    do { n = waitpid(holder, &status, 0); } while (n < 0 && errno == EINTR);
    if (n != holder || !WIFEXITED(status) || WEXITSTATUS(status)) return 112;
    return result;
}
int main(int argc, char **argv) {
    if (argc < 2) return 90;
    struct sigaction action = {.sa_handler = stop_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL)) return 91;
    if (!strncmp(argv[1], "holder-", 7)) return holder_fixture(argv[1]);
    if (!strcmp(argv[1], "term-default") || !strcmp(argv[1], "ignore")) {
        action.sa_handler = !strcmp(argv[1], "ignore") ? SIG_IGN : SIG_DFL;
        if (sigaction(SIGTERM, &action, NULL)) return 96;
    }
    if (!strcmp(argv[1], "context")) {
        char cwd[1024];
        if (argc != 4 || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, argv[2]) ||
            !getenv("PPP_FIXTURE") || strcmp(getenv("PPP_FIXTURE"), "explicit") || getenv("HOME") ||
            fcntl(atoi(argv[3]), F_GETFD) != -1 || errno != EBADF) return 92;
        return output(1, (const unsigned char *)"R", 1) ? 93 : 0;
    }
    if (!strcmp(argv[1], "diagnostic")) {
        unsigned char private_data[1024];
        memset(private_data, 'x', sizeof(private_data));
        memcpy(private_data, "private-child-marker", 20);
        for (unsigned i = 0; i < 128; i++) if (output(2, private_data, sizeof(private_data))) return 93;
    }
    if (!strcmp(argv[1], "burst")) {
        unsigned char data[1024];
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char)i;
        for (unsigned i = 0; i < 64; i++) if (output(1, data, sizeof(data))) return 94;
        return 0;
    }
    int binary = !strcmp(argv[1], "binary") || !strcmp(argv[1], "binarysplit") ||
                 !strcmp(argv[1], "binaryexit") || !strcmp(argv[1], "binaryeof");
    int silent = !strcmp(argv[1], "silent") || !strcmp(argv[1], "stderronly");
    if (!strcmp(argv[1], "stderronly") && output(2, (const unsigned char *)"private-stderr-marker", 21)) return 93;
    if (!strcmp(argv[1], "noinput")) (void)close(0);
    if (binary) {
        const unsigned char first[] = {0x00, 0xff, 0x7e, 0x7d, 0x03, 0x01};
        if (!strcmp(argv[1], "binarysplit")) {
            if (output(1, first, 1) || output(1, first + 1, sizeof(first) - 1)) return 95;
        } else if (output(1, first, sizeof(first))) return 95;
    } else if (!silent && output(1, (const unsigned char *)"RDY", 3)) return 95;
    if (!strcmp(argv[1], "binaryexit")) return 0;
    if (!strcmp(argv[1], "binaryeof")) (void)close(1);
    if (!strcmp(argv[1], "exit0")) return 0;
    if (!strcmp(argv[1], "exit7")) return 7;
    if (!strcmp(argv[1], "afterinput7")) {
        unsigned char byte;
        if (read(0, &byte, 1) != 1) return 98;
        if (output(1, &byte, 1)) return 99;
        return 7;
    }
    if (!strcmp(argv[1], "rxaftereof")) {
        (void)close(1);
        for (;;) {
            unsigned char byte;
            ssize_t got = read(0, &byte, 1);
            if (got < 0 && errno == EINTR) continue;
            return got == 1 && byte == 'Z' ? 0 : 98;
        }
    }
    if (!strcmp(argv[1], "nooutput")) (void)close(1);
    if (!strcmp(argv[1], "ignore")) {
        action.sa_handler = SIG_IGN;
        if (sigaction(SIGTERM, &action, NULL)) return 96;
        if (output(1, (const unsigned char *)"I", 1)) return 95;
    }
    while (!stopping) {
        struct pollfd fd = {0, POLLIN, 0};
        int n = poll(!strcmp(argv[1], "noinput") || !strcmp(argv[1], "ignore") ? NULL : &fd,
                     !strcmp(argv[1], "noinput") || !strcmp(argv[1], "ignore") ? 0 : 1, 5000);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || !n) return 97;
        if (fd.revents & (POLLIN | POLLHUP)) {
            unsigned char data[257];
            ssize_t got = read(0, data, !strcmp(argv[1], "split") ? 1 : sizeof(data));
            if (!got) break;
            if (got < 0) { if (errno == EINTR) continue; return 98; }
            if (output(1, data, (size_t)got)) return 99;
        }
    }
    if (stopping && !strcmp(argv[1], "term-exit7")) return 7;
    if (stopping && !strcmp(argv[1], "term-other")) {
        if (raise(SIGUSR1)) return 96;
        return 96;
    }
    /* pppd on a graceful signal ends with EXIT_USER_REQUEST (5). pppd-term is
     * a pppd whose LCP Terminate-Request reaches its stdout: it sends one and
     * waits up to 3 s for the Terminate-Ack (escaped code 06) on stdin.
     * pppd-shunt models pppd 'notty': the same signal first kills the
     * character shunt between pppd and stdin/stdout, so the Terminate-Request
     * pppd then sends never reaches stdout; pppd gives up and exits. */
    if (stopping && !strcmp(argv[1], "pppd-term")) {
        static const unsigned char request[] = {0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x25, 0x7d, 0x22,
                                                0x7d, 0x20, 0x7d, 0x24, 0x59, 0x28, 0x7e};
        if (output(1, request, sizeof(request))) return 99;
        uint64_t deadline = monotonic_ms() + 3000;
        unsigned char previous = 0;
        int acked = 0;
        while (!acked && monotonic_ms() < deadline) {
            struct pollfd fd = {0, POLLIN, 0};
            int n = poll(&fd, 1, (int)(deadline - monotonic_ms()));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            unsigned char data[256];
            ssize_t got = read(0, data, sizeof(data));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            for (ssize_t i = 0; i < got; i++) {
                if (previous == 0x7d && data[i] == 0x26) acked = 1;
                previous = data[i];
            }
        }
        return acked ? 5 : 6;
    }
    if (stopping && !strcmp(argv[1], "pppd-shunt")) {
        finite_hold(200);
        return 5;
    }
    if (stopping && !strcmp(argv[1], "exchange")) {
        const unsigned char end[] = {0x7e, 0, 0xff};
        if (output(1, end, sizeof(end))) return 99;
    }
    return 0;
}
