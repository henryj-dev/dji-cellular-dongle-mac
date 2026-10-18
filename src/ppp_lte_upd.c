#define _DARWIN_C_SOURCE
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Supervisor for launchd: waits for one dongle, runs each session as a fresh
 * `ppp_lte_up --up` child, reconnects with backoff while the dongle stays,
 * and stops the child cleanly when launchd stops it. It never opens the USB
 * device, starts pppd or writes SystemConfiguration itself. */
#ifndef UPD_CHILD_PATH
#define UPD_CHILD_PATH "/Library/PrivilegedHelperTools/dji-cellular-dongle-mac/ppp_lte_up"
#endif
#define UPD_LOG_PATH "/var/log/dji-cellular-dongle-mac.log"
#define UPD_VENDOR 0x2ca3
#define UPD_PRODUCT 0x4009
#define UPD_POLL_MS 1000u
#define UPD_SLICE_MS 200u
#define UPD_BACKOFF_MIN_MS 5000u
#define UPD_BACKOFF_MAX_MS 300000u
#define UPD_STABLE_MS 60000u
#define UPD_STOP_WAIT_MS 60000u
#define UPD_BLOCKED_WAIT_MS 60000u
#define UPD_EOF_WAIT_MS 5000u
#define UPD_CLEANUP_LIMIT_MS 30000u
#define UPD_CLEANUP_TRIES 3u
#define UPD_CLEANUP_RETRY_MS 2000u
/* A session-error end this fast without connecting, three times in a row,
 * means the modem is stuck (e.g. still in data mode after a killed pppd). */
#define UPD_FAST_FAIL_MS 5000u
#define UPD_FAST_FAILS 3u
#define UPD_LINE 512u

/* OS boundary, replaced as a whole in tests. */
struct upd_backend {
    void *context;
    int (*devices)(void *context);                 /* matching dongles, -1 unknown */
    int (*path_ok)(void *context, const char *path);
    /* Spawns path with {path, mode}; *fd reads its stdout+stderr. */
    int (*spawn)(void *context, const char *path, const char *mode, pid_t *pid, int *fd);
    /* 0 running, 1 ended (*status filled), -1 error. Never blocks. */
    int (*reap)(void *context, pid_t pid, int *status);
    /* Bytes read, 0 at EOF, -1 nothing yet (EAGAIN). Waits at most wait_ms. */
    long (*read)(void *context, int fd, char *buffer, size_t size, unsigned wait_ms);
    void (*close)(void *context, int fd);
    int (*signal)(void *context, pid_t pid, int number);
    uint64_t (*now)(void *context);                 /* monotonic ms */
    void (*sleep)(void *context, unsigned ms);
};
struct upd {
    const struct upd_backend *b;
    FILE *output;
    const char *log_path;  /* NULL: never reopened (tests) */
    volatile sig_atomic_t *stop;
    unsigned backoff_ms, fast_fails;
    int last_devices, replug;
};
struct upd_child {
    int ended, exit_code, signaled, connected, killed, forwarded, reap_error, blocked, spawn_error, session_error;
    uint64_t connected_ms, ended_ms, started_ms;
};

static void upd_log(struct upd *u, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* newsyslog renames the log without signalling us: follow the path to the
 * new file when its inode differs from the one being written. */
static void upd_follow_log(struct upd *u) {
    if (!u->log_path) return;
    struct stat now, open_file;
    if (!stat(u->log_path, &now) && !fstat(fileno(u->output), &open_file) &&
        now.st_dev == open_file.st_dev && now.st_ino == open_file.st_ino) return;
    (void)fflush(u->output);
    (void)freopen(u->log_path, "a", u->output);
}
static void upd_log(struct upd *u, const char *format, ...) {
    upd_follow_log(u);
    char stamp[32] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    va_list arguments;
    va_start(arguments, format);
    (void)fprintf(u->output, "%s ", stamp);
    (void)vfprintf(u->output, format, arguments);
    (void)fputc('\n', u->output);
    (void)fflush(u->output);
    va_end(arguments);
}
/* Waits in short slices so a stop request is seen within one slice. */
static void upd_wait(struct upd *u, unsigned ms) {
    uint64_t start = u->b->now(u->b->context);
    while (!*u->stop) {
        uint64_t now = u->b->now(u->b->context);
        /* A failed clock still waits one slice: never a busy loop. */
        if (now == UINT64_MAX || start == UINT64_MAX) { u->b->sleep(u->b->context, UPD_SLICE_MS); return; }
        if (now < start || now - start >= ms) return;
        uint64_t left = ms - (now - start);
        u->b->sleep(u->b->context, left < UPD_SLICE_MS ? (unsigned)left : UPD_SLICE_MS);
    }
}
/* Forwards one complete child line with a time stamp; marks the connection. */
static void upd_line(struct upd *u, struct upd_child *k, const char *line) {
    if (!strncmp(line, "up=blocked ", 11)) k->blocked = 1;
    if (!strncmp(line, "up-result=ended reason=session-error ", 37)) k->session_error = 1;
    if (!strncmp(line, "up=connected", 12) && !k->connected) {
        k->connected = 1; k->connected_ms = u->b->now(u->b->context);
    }
    upd_log(u, "child %s", line);
}
/* forward: pass a stop request on as SIGHUP, which ppp_lte_up latches for its
 * whole run (SIGTERM is only graceful while its runner is active); SIGKILL
 * UPD_STOP_WAIT_MS later. A cleanup run is not forwarded but is killed after
 * UPD_CLEANUP_LIMIT_MS. */
static void upd_child(struct upd *u, const char *mode, int forward, struct upd_child *k) {
    *k = (struct upd_child){.exit_code = -1};
    pid_t pid = 0;
    int fd = -1;
    if (u->b->spawn(u->b->context, UPD_CHILD_PATH, mode, &pid, &fd) || pid <= 0) {
        upd_log(u, "upd=spawn-error mode=%s", mode + 2);
        k->ended = 1; k->spawn_error = 1; k->ended_ms = u->b->now(u->b->context);
        return;
    }
    char line[UPD_LINE];
    size_t used = 0;
    int eof = fd < 0;
    uint64_t forwarded_ms = 0, started_ms = u->b->now(u->b->context), ended_wait_ms = 0;
    k->started_ms = started_ms;
    for (;;) {
        /* Never signal a pid already reaped: it may belong to another process now. */
        if (forward && *u->stop && !k->forwarded && !k->ended) {
            k->forwarded = 1; forwarded_ms = u->b->now(u->b->context);
            (void)u->b->signal(u->b->context, pid, SIGHUP);
            upd_log(u, "upd=stopping-session");
        }
        if (!k->killed && !k->ended) {
            uint64_t now = u->b->now(u->b->context), since = forward ? forwarded_ms : started_ms;
            uint64_t limit = forward ? UPD_STOP_WAIT_MS : UPD_CLEANUP_LIMIT_MS;
            if ((!forward || k->forwarded) && now != UINT64_MAX && since != UINT64_MAX &&
                now >= since && now - since >= limit) {
                k->killed = 1; (void)u->b->signal(u->b->context, pid, SIGKILL);
                upd_log(u, "upd=child-killed mode=%s", mode + 2);
            }
        }
        if (!eof) {
            char chunk[256];
            long count = u->b->read(u->b->context, fd, chunk, sizeof(chunk), UPD_SLICE_MS);
            if (count == 0) eof = 1;
            for (long i = 0; i < count; i++) {
                if (chunk[i] == '\n' || used == sizeof(line) - 1) {
                    line[used] = 0; upd_line(u, k, line); used = 0;
                    if (chunk[i] == '\n') continue;
                }
                line[used++] = chunk[i];
            }
        } else if (!k->ended) u->b->sleep(u->b->context, UPD_SLICE_MS);
        if (!k->ended) {
            int status = 0, reaped = u->b->reap(u->b->context, pid, &status);
            if (reaped < 0) { upd_log(u, "upd=reap-error"); k->ended = 1; k->reap_error = 1; }
            else if (reaped) {
                k->ended = 1;
                if (WIFEXITED(status)) k->exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) k->signaled = WTERMSIG(status);
            }
            if (k->ended) k->ended_ms = u->b->now(u->b->context);
        }
        if (k->ended && eof) break;
        if (k->ended) {
            /* Output still open after the exit (a descendant holds it): bounded. */
            uint64_t now = u->b->now(u->b->context);
            if (!ended_wait_ms) ended_wait_ms = now;
            else if (now == UINT64_MAX || now < ended_wait_ms || now - ended_wait_ms >= UPD_EOF_WAIT_MS) {
                upd_log(u, "upd=output-abandoned mode=%s", mode + 2);
                break;
            }
        }
    }
    if (used) { line[used] = 0; upd_line(u, k, line); }
    if (fd >= 0) u->b->close(u->b->context, fd);
    if (k->signaled) upd_log(u, "upd=session-ended mode=%s signal=%d", mode + 2, k->signaled);
    else upd_log(u, "upd=session-ended mode=%s exit=%d", mode + 2, k->exit_code);
}
/* The cleanup command, retried while it reports leftovers (exit 4), e.g. a
 * killed session's pppd still exiting. Bounded tries; a stop does not skip it. */
static void upd_cleanup(struct upd *u) {
    /* While stopping, launchd's exit budget allows one run only. */
    unsigned tries = *u->stop ? 1 : UPD_CLEANUP_TRIES;
    for (unsigned i = 0; i < tries; i++) {
        struct upd_child cleanup;
        /* Counted in slices, independent of the clock. */
        for (unsigned slice = 0; i && slice < UPD_CLEANUP_RETRY_MS / UPD_SLICE_MS; slice++)
            u->b->sleep(u->b->context, UPD_SLICE_MS);
        upd_child(u, "--cleanup", 0, &cleanup);
        if (cleanup.exit_code != 4) return;
    }
}
/* Private fake-TU entry. Returns 0 after a stop request, 1 on output failure. */
static int upd_run(const struct upd_backend *b, FILE *output, const char *log_path, volatile sig_atomic_t *stop) {
    struct upd u = {.b = b, .output = output, .log_path = log_path, .stop = stop, .backoff_ms = UPD_BACKOFF_MIN_MS, .last_devices = -2};
    upd_log(&u, "upd=starting");
    /* Keys left by a previous run that was killed (shutdown, crash) must not
     * keep a dead primary while no dongle is plugged in. */
    if (b->path_ok(b->context, UPD_CHILD_PATH)) upd_cleanup(&u);
    while (!*stop) {
        int devices = b->devices(b->context);
        if (devices != 1) {
            if (devices != u.last_devices) {
                if (devices < 0) upd_log(&u, "upd=waiting devices=unknown");
                else upd_log(&u, "upd=waiting devices=%d", devices);
            }
            u.last_devices = devices;
            if (devices == 0) u.replug = 0; /* the replug the modem needed */
            upd_wait(&u, UPD_POLL_MS);
            continue;
        }
        u.last_devices = devices;
        if (u.replug) { upd_wait(&u, UPD_POLL_MS); continue; }
        if (!b->path_ok(b->context, UPD_CHILD_PATH)) {
            upd_log(&u, "upd=blocked gate=child-path");
            upd_wait(&u, UPD_BLOCKED_WAIT_MS);
            continue;
        }
        upd_log(&u, "upd=session-start");
        struct upd_child k;
        upd_child(&u, "--up", 1, &k);
        /* Leftover keys or a killed child: the cleanup command, once. */
        if (k.exit_code == 4 || k.killed || k.signaled || k.reap_error) upd_cleanup(&u);
        if (*stop) break;
        if (k.connected && k.ended_ms >= k.connected_ms && k.ended_ms - k.connected_ms >= UPD_STABLE_MS)
            u.backoff_ms = UPD_BACKOFF_MIN_MS;
        if (b->devices(b->context) == 0) {
            /* Unplugged (an unreadable count is not): next plug, no backoff. */
            u.backoff_ms = UPD_BACKOFF_MIN_MS; u.fast_fails = 0;
            continue;
        }
        int fast = k.session_error && !k.connected && !k.blocked && !k.spawn_error &&
            k.started_ms != UINT64_MAX && k.ended_ms != UINT64_MAX &&
            k.ended_ms >= k.started_ms && k.ended_ms - k.started_ms < UPD_FAST_FAIL_MS;
        u.fast_fails = fast ? u.fast_fails + 1 : 0;
        if (u.fast_fails >= UPD_FAST_FAILS) {
            /* Retrying cannot help; a fresh plug resets the modem. */
            upd_log(&u, "upd=replug-required");
            u.replug = 1; u.fast_fails = 0; u.backoff_ms = UPD_BACKOFF_MIN_MS;
            continue;
        }
        upd_log(&u, "upd=retry-in-ms=%u", u.backoff_ms);
        upd_wait(&u, u.backoff_ms);
        u.backoff_ms = u.backoff_ms >= UPD_BACKOFF_MAX_MS / 2 ? UPD_BACKOFF_MAX_MS : u.backoff_ms * 2;
    }
    upd_log(&u, "upd=stopped");
    return ferror(output) ? 1 : 0;
}

/* ---- Native backend ---- */
static int native_devices(void *context) {
    (void)context;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return -1;
    int32_t vid_value = UPD_VENDOR, pid_value = UPD_PRODUCT;
    CFNumberRef vid = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid_value);
    CFNumberRef pid = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid_value);
    if (!vid || !pid) {
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        CFRelease(match);
        return -1;
    }
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), vid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), pid);
    CFRelease(vid); CFRelease(pid);
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator) != KERN_SUCCESS) return -1;
    /* No match comes back as success with no iterator at all: zero dongles. */
    if (iterator == IO_OBJECT_NULL) return 0;
    int count = 0;
    io_service_t next;
    while ((next = IOIteratorNext(iterator)) != IO_OBJECT_NULL) { count++; IOObjectRelease(next); }
    int valid = IOIteratorIsValid(iterator);
    IOObjectRelease(iterator);
    return valid ? count : -1;
}
/* Every path component: root-owned, no group/other write, no symlink; the
 * last one a non-setuid executable regular file. */
static int native_path_ok(void *context, const char *path) {
    (void)context;
    char part[1024];
    size_t length = strlen(path);
    if (!length || path[0] != '/' || length >= sizeof(part)) return 0;
    memcpy(part, path, length + 1);
    for (size_t i = 1; i <= length; i++) {
        if (part[i] != '/' && i != length) continue;
        char saved = part[i]; part[i] = 0;
        struct stat st;
        int bad = lstat(part, &st) || S_ISLNK(st.st_mode) || st.st_uid != 0 ||
            (st.st_mode & (S_IWGRP | S_IWOTH));
        part[i] = saved;
        if (bad) return 0;
        if (i == length && (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR) || (st.st_mode & (S_ISUID | S_ISGID))))
            return 0;
    }
    struct stat root;
    return !lstat("/", &root) && root.st_uid == 0 && !(root.st_mode & (S_IWGRP | S_IWOTH));
}
static int native_spawn(void *context, const char *path, const char *mode, pid_t *pid, int *fd) {
    (void)context;
    int pipes[2];
    if (pipe(pipes)) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions)) { close(pipes[0]); close(pipes[1]); return -1; }
    if (posix_spawnattr_init(&attributes)) {
        posix_spawn_file_actions_destroy(&actions); close(pipes[0]); close(pipes[1]); return -1;
    }
    sigset_t none, all;
    sigemptyset(&none); sigfillset(&all); sigdelset(&all, SIGKILL); sigdelset(&all, SIGSTOP);
    /* Only stdin (/dev/null), stdout and stderr (the pipe) reach the child. */
    int failed = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) ||
        posix_spawn_file_actions_adddup2(&actions, pipes[1], 1) ||
        posix_spawn_file_actions_adddup2(&actions, pipes[1], 2) ||
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF |
                                 POSIX_SPAWN_SETSIGMASK) ||
        posix_spawnattr_setsigdefault(&attributes, &all) || posix_spawnattr_setsigmask(&attributes, &none);
    char *argv[] = {(char *)path, (char *)mode, NULL};
    char *envp[] = {NULL};
    if (!failed) failed = posix_spawn(pid, path, &actions, &attributes, argv, envp) != 0;
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipes[1]);
    if (failed) { close(pipes[0]); return -1; }
    (void)fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    *fd = pipes[0];
    return 0;
}
static int native_reap(void *context, pid_t pid, int *status) {
    (void)context;
    pid_t result;
    do result = waitpid(pid, status, WNOHANG); while (result < 0 && errno == EINTR);
    return result == pid ? 1 : result == 0 ? 0 : -1;
}
static long native_read(void *context, int fd, char *buffer, size_t size, unsigned wait_ms) {
    (void)context;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int ready = poll(&p, 1, (int)wait_ms);
    if (ready <= 0) return -1;
    ssize_t count = read(fd, buffer, size);
    if (count < 0) return errno == EAGAIN || errno == EINTR ? -1 : 0;
    return (long)count;
}
static void native_close(void *context, int fd) { (void)context; (void)close(fd); }
static int native_signal(void *context, pid_t pid, int number) { (void)context; return kill(pid, number); }
static uint64_t native_now(void *context) {
    (void)context;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0) return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void native_sleep(void *context, unsigned ms) {
    (void)context;
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&delay, NULL);
}
static volatile sig_atomic_t upd_stop_request;
static void upd_on_stop(int number) { (void)number; upd_stop_request = 1; }
int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { fputs("usage: ppp_lte_upd (run by launchd; no arguments)\n", stderr); return 2; }
    if (getuid() != 0 || geteuid() != 0) { fputs("upd=blocked gate=root-identity\n", stdout); return 3; }
    struct sigaction stop = {.sa_handler = upd_on_stop}, ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&stop.sa_mask); sigemptyset(&ignore.sa_mask);
    (void)sigaction(SIGTERM, &stop, NULL); (void)sigaction(SIGINT, &stop, NULL);
    (void)sigaction(SIGHUP, &stop, NULL); (void)sigaction(SIGPIPE, &ignore, NULL);
    const struct upd_backend native = {NULL, native_devices, native_path_ok, native_spawn, native_reap,
        native_read, native_close, native_signal, native_now, native_sleep};
    (void)umask(027); /* a reopened log is created 0640 */
    return upd_run(&native, stdout, UPD_LOG_PATH, &upd_stop_request);
}
