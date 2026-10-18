#define _DARWIN_C_SOURCE
#include "ppp_auth_probe.h"
#include "ppp_child.h"
#include "ppp_launch.h"
#include "ppp_trial_observer.h"
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL auth-trial line=%d\n", __LINE__); } } while (0)
enum scenario {
    NORMAL, EXEC_FAIL, EOF_EARLY, SIGNAL_STOP, CLOCK_FAIL, CLOCK_BACKWARD,
    TIMEOUT, TERM_IGNORED, UNRESOLVED, OWNERSHIP_LOST, STDERR_RESERVE,
    READ_ERROR, WRITE_ERROR, RESTORE_FAIL, INSTALL_FAIL, SPAWN_FAIL,
    SNAPSHOT_UNKNOWN, SNAPSHOT_CHANGED, BAD_REJECT, STDERR_FULL,
    PENDING_EXEC, BAD_READ_COUNT, BAD_WRITE_COUNT, CHILD_IO_ERROR, START_CLOCK_FAIL
};
static struct fake_state {
    enum scenario mode;
    unsigned identity_calls, metadata_calls, preflight_calls, home_calls, spawn_calls;
    unsigned poll_calls, status_calls, read_calls, write_calls, term_calls, kill_calls;
    unsigned installed, restored, observer_calls, child_identity_calls, sample_calls;
    unsigned full_poll_after_reserve, host_calls, now_calls, wait_calls;
    uid_t uid, euid, owner;
    gid_t gid, egid;
    const char *bad_path, *home, *checked_home, *link_target;
    mode_t bad_mode, var_mode;
    uid_t var_owner;
    unsigned readlink_calls, common_metadata_calls, common_uid_calls, common_home_calls, leaf_mask;
    int metadata_error, home_error, preflight_error, invalid_preflight, readlink_fault, in_preflight, signal_number;
    int format_error, output_error, flush_error;
    uint64_t now;
    unsigned char incoming[512], written[128];
    size_t incoming_used, incoming_length, written_length;
    struct sigaction original[2], current[2];
    int initialized_peer, stopped, reserve_seen;
} f;
static uid_t fake_getuid(void) { f.identity_calls++; return f.uid; }
static uid_t fake_geteuid(void) { f.identity_calls++; return f.euid; }
static gid_t fake_getgid(void) { f.identity_calls++; return f.gid; }
static gid_t fake_getegid(void) { f.identity_calls++; return f.egid; }
static const char *const common_paths[] = {
    "/usr/sbin/pppd", "/private/etc/ppp/options", "/private/etc/ppp/postoptions", NULL,
    "/private/etc/ppp/pap-secrets", "/private/etc/ppp/chap-secrets", "/private/etc/ppp/srp-secrets",
    "/private/etc/ppp/ip-up", "/private/etc/ppp/ip-down", "/private/etc/ppp/auth-up",
    "/private/etc/ppp/auth-down", "/private/etc/ppp/ipv6-up", "/private/etc/ppp/ipv6-down"
};
static int fake_lstat(const char *path, struct stat *st) {
    f.metadata_calls++;
    int leaf = -1;
    if (f.in_preflight) {
        f.common_metadata_calls++;
        for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++) {
            char user[PATH_MAX];
            const char *candidate = common_paths[i];
            if (i == PPP_LAUNCH_USER_OPTIONS) {
                int n = snprintf(user, sizeof(user), "%s%s.ppprc", f.checked_home,
                                 !strcmp(f.checked_home, "/") ? "" : "/");
                CHECK(n > 0 && (size_t)n < sizeof(user));
                candidate = user;
            }
            if (!strcmp(path, candidate)) { leaf = (int)i; f.leaf_mask |= 1u << i; break; }
        }
    }
    *st = (struct stat){.st_uid = 0, .st_mode = !strcmp(path, "/usr/sbin/pppd") ? S_IFREG | 0755 : S_IFDIR | 0755};
    if (!strcmp(path, "/var")) { st->st_uid = f.var_owner; st->st_mode = f.var_mode; }
    if (f.bad_path && !strcmp(path, f.bad_path)) {
        st->st_uid = f.owner; st->st_mode = f.bad_mode;
        if (f.metadata_error) { errno = f.metadata_error; return -1; }
        return 0;
    }
    if (leaf > 0) { errno = ENOENT; return -1; }
    return 0;
}
static ssize_t fake_readlink(const char *path, char *buffer, size_t capacity) {
    CHECK(!strcmp(path, "/var")); f.readlink_calls++;
    if (f.readlink_fault == 1) { errno = EACCES; return -1; }
    if (f.readlink_fault == 2) { memset(buffer, 'x', capacity); return (ssize_t)capacity; }
    if (f.readlink_fault == 3) {
        CHECK(capacity >= sizeof("private/var"));
        memcpy(buffer, "private/var", sizeof("private/var"));
        return (ssize_t)sizeof("private/var"); /* Embedded NUL is not an exact target. */
    }
    size_t length = strlen(f.link_target);
    size_t count = length < capacity ? length : capacity;
    memset(buffer, 'x', capacity); /* No NUL, including valid targets. */
    memcpy(buffer, f.link_target, count);
    return (ssize_t)count;
}
static struct passwd *fake_getpwuid(uid_t uid) {
    CHECK(uid == f.uid); f.home_calls++;
    static struct passwd result;
    result.pw_dir = (char *)f.home;
    return f.home_error ? NULL : &result;
}
static uid_t fake_common_uid(void *context) {
    const struct ppp_metadata_backend *backend = context;
    f.common_uid_calls++;
    return backend->real_uid(backend->context);
}
static int fake_common_home(void *context, uid_t uid, const char **path) {
    const struct ppp_metadata_backend *backend = context;
    f.common_home_calls++;
    int error = backend->home(backend->context, uid, path);
    CHECK(!error && uid == f.uid && *path && !strcmp(*path, f.checked_home));
    return error;
}
static int fake_common_metadata(void *context, const char *path, struct stat *st) {
    const struct ppp_metadata_backend *backend = context;
    return backend->metadata(backend->context, path, st);
}
/* Defined before the macro: this calls the separately linked real checker.
 * Supplied trial callbacks resolve only to fake identity/metadata above. */
static int fake_preflight(struct ppp_launch_preflight *result, const struct ppp_metadata_backend *backend) {
    f.preflight_calls++;
    CHECK(backend && backend->real_uid && backend->home && backend->metadata);
    if (!backend || !backend->real_uid || !backend->home || !backend->metadata) return -1;
    const struct ppp_metadata_backend fake = {
        (void *)backend, fake_common_uid, fake_common_home, fake_common_metadata
    };
    f.in_preflight = 1;
    int error = ppp_launch_preflight(result, &fake);
    f.in_preflight = 0;
    if (f.invalid_preflight == 1) result->items[0] = (enum ppp_metadata_state)99;
    if (f.invalid_preflight == 2) result->clear = !result->clear;
    return f.preflight_error ? -1 : error;
}
static int fake_sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    unsigned i = number == SIGINT ? 0 : 1;
    CHECK(number == SIGINT || number == SIGTERM);
    f.host_calls++;
    if (f.mode == INSTALL_FAIL && i == 1 && old) return -1;
    if (old) { *old = f.current[i]; f.installed++; }
    else {
        f.restored++;
        if (f.mode == RESTORE_FAIL && i == 1) return -1;
    }
    f.current[i] = *action;
    return 0;
}
static int fake_clock(clockid_t clock, struct timespec *ts) {
    CHECK(clock == CLOCK_MONOTONIC); f.now_calls++;
    if ((f.mode == CLOCK_FAIL && f.now_calls == 4) ||
        (f.mode == START_CLOCK_FAIL && f.now_calls == 1)) return -1;
    f.now += 100;
    uint64_t now = f.mode == CLOCK_BACKWARD && f.now_calls == 4 ? 0 : f.now;
    ts->tv_sec = (time_t)(now / 1000); ts->tv_nsec = (long)(now % 1000) * 1000000;
    return 0;
}
static int fake_nanosleep(const struct timespec *delay, struct timespec *remaining) {
    CHECK(delay->tv_nsec == 10000000L && !remaining); f.wait_calls++;
    return 0;
}
static size_t encode(unsigned char *out, const unsigned char *packet, size_t length) {
    size_t n = 0; out[n++] = 0x7e;
    uint16_t crc = (uint16_t)~ppp_auth_fcs(0xffff, packet, length);
    for (size_t i = 0; i < length + 2; i++) {
        unsigned char b = i < length ? packet[i] : i == length ? (unsigned char)crc : (unsigned char)(crc >> 8);
        if (b < 32 || b == 0x7d || b == 0x7e) { out[n++] = 0x7d; b ^= 0x20; }
        out[n++] = b;
    }
    out[n++] = 0x7e;
    return n;
}
static int fake_start(struct ppp_child *c, const char *path, char *const argv[], char *const env[], const char *cwd) {
    f.spawn_calls++;
    CHECK(!strcmp(path, "/usr/sbin/pppd") && !strcmp(cwd, "/") && env && env[0] == NULL);
    struct ppp_launch_policy policy;
    CHECK(ppp_launch_prepare(&policy) == 0);
    unsigned i = 0;
    for (; argv[i] && policy.argv[i]; i++) CHECK(!strcmp(argv[i], policy.argv[i]));
    CHECK(i == 15 && !argv[i] && !policy.argv[i]);
    CHECK(f.installed == 2 && f.observer_calls == 1);
    if (f.mode == SPAWN_FAIL) return -1;
    c->started = 1; c->pid = 321;
    return 0;
}
static int fake_status(struct ppp_child *c) {
    f.status_calls++;
    CHECK(f.child_identity_calls == 1);
    if (f.mode == OWNERSHIP_LOST && f.poll_calls > 1) { c->ownership_lost = 1; c->io_error = 1; }
    if (f.mode == EXEC_FAIL) { c->exec_error = ENOENT; c->exec_done = 1; c->reaped = 1; }
    if (f.mode != PENDING_EXEC && f.mode != EXEC_FAIL) c->exec_done = 1;
    if (f.mode == CHILD_IO_ERROR) c->io_error = 1;
    if (f.stopped && f.mode != UNRESOLVED && !c->ownership_lost) c->reaped = 1;
    return c->io_error || c->exec_error ? -1 : 0;
}
static int fake_poll(struct ppp_child *c) {
    f.poll_calls++;
    if (f.reserve_seen) f.full_poll_after_reserve++;
    int result = fake_status(c);
    if (f.mode == SIGNAL_STOP && f.poll_calls == 2) f.current[0].sa_handler(SIGINT);
    if (f.mode == STDERR_RESERVE || f.mode == STDERR_FULL) {
        /* Real poll is allowed to consume at most 4096 and reads no more. */
        c->stderr_bytes += f.mode == STDERR_FULL ? 4096 : 4095;
        if (c->stderr_bytes > PPP_AUTH_BYTE_MAX - PPP_AUTH_SLICE) f.reserve_seen = 1;
    } else if (c->reaped || c->ownership_lost) c->stderr_eof = 1;
    return result;
}
static struct offline_io fake_read(struct ppp_child *c, unsigned char *bytes, size_t capacity) {
    f.read_calls++;
    CHECK(c->exec_done && !c->exec_error && capacity <= PPP_AUTH_SLICE);
    if (f.mode == READ_ERROR) return (struct offline_io){OFFLINE_IO_ERROR, 0};
    if (f.mode == BAD_READ_COUNT) return (struct offline_io){OFFLINE_IO_OK, capacity + 1};
    if (f.mode == EOF_EARLY || c->reaped || c->ownership_lost) {
        c->stdout_eof = 1; return (struct offline_io){OFFLINE_IO_EOF, 0};
    }
    if (!f.initialized_peer && f.mode != TIMEOUT && f.mode != PENDING_EXEC &&
        f.mode != STDERR_RESERVE && f.mode != STDERR_FULL) {
        const unsigned char packet[] = {255,3,0xc0,0x21,1,1,0,4};
        f.incoming_length = encode(f.incoming, packet, sizeof(packet)); f.initialized_peer = 1;
    }
    if (f.incoming_used == f.incoming_length) return (struct offline_io){OFFLINE_IO_AGAIN, 0};
    size_t n = f.incoming_length - f.incoming_used;
    if (n > capacity) n = capacity;
    if (n > 7) n = 7; /* arbitrary split, including escape sequences */
    memcpy(bytes, f.incoming + f.incoming_used, n); f.incoming_used += n;
    return (struct offline_io){OFFLINE_IO_OK, n};
}
static struct offline_io fake_write(struct ppp_child *c, const unsigned char *bytes, size_t length) {
    CHECK(c->exec_done && !f.stopped && length <= PPP_AUTH_SLICE); f.write_calls++;
    if (f.mode == WRITE_ERROR) return (struct offline_io){OFFLINE_IO_ERROR, 0};
    if (f.mode == BAD_WRITE_COUNT) return (struct offline_io){OFFLINE_IO_OK, length + 1};
    if (f.write_calls % 5 == 0) return (struct offline_io){OFFLINE_IO_AGAIN, 0};
    if (f.write_calls % 7 == 0) return (struct offline_io){OFFLINE_IO_INTR, 0};
    size_t n = length > 3 ? 3 : length;
    CHECK(f.written_length + n <= sizeof(f.written));
    memcpy(f.written + f.written_length, bytes, n); f.written_length += n;
    if (f.written_length > 1 && f.written[f.written_length - 1] == 0x7e) {
        unsigned char packet[64]; size_t used = 0; int escaped = 0;
        for (size_t i = 1; i + 1 < f.written_length; i++) {
            unsigned char b = f.written[i];
            if (!escaped && b == 0x7d) { escaped = 1; continue; }
            packet[used++] = escaped ? b ^ 0x20 : b; escaped = 0;
        }
        CHECK(ppp_auth_fcs(0xffff, packet, used) == 0xf0b8);
        packet[4] = f.mode == BAD_REJECT ? 2 : 4;
        f.incoming_length = encode(f.incoming, packet, used - 2);
        f.incoming_used = 0; f.written_length = 0;
    }
    return (struct offline_io){OFFLINE_IO_OK, n};
}
static int fake_signal_child(struct ppp_child *c, int number) {
    CHECK(!c->reaped && !c->ownership_lost && c->pid == 321);
    if (number == SIGTERM) {
        CHECK(!f.term_calls++); c->term_signals++;
        if (f.mode != TERM_IGNORED && f.mode != UNRESOLVED) f.stopped = 1;
    } else { CHECK(number == SIGKILL && !f.kill_calls++); c->kill_signals++; if (f.mode != UNRESOLVED) f.stopped = 1; }
    return 0;
}
static void fake_end(struct ppp_child *c) { (void)c; }
static int fake_close(struct ppp_child *c) {
    CHECK(c->reaped || c->ownership_lost || !c->started);
    c->closed = 1;
    return 0;
}
static void fake_observer_begin(struct ppp_trial_observer *o) {
    memset(o, 0, sizeof(*o)); f.observer_calls++;
}
static void fake_observer_child(struct ppp_trial_observer *o, pid_t pid) {
    (void)o; CHECK(pid == 321 && !f.poll_calls && !f.status_calls); f.child_identity_calls++;
}
static void fake_observer_sample(struct ppp_trial_observer *o) { (void)o; f.sample_calls++; }
static void fake_observer_finish(struct ppp_trial_observer *o) { (void)o; }
static int fake_observer_diagnostic(const struct ppp_trial_observer *o, char *buffer, size_t capacity) {
    (void)o;
    return snprintf(buffer, capacity, "attribution=unproven descendants=unproven sc-scope=dynamic-store-global-only");
}
static int fake_observer_result(const struct ppp_trial_observer *o) {
    (void)o;
    return f.mode == SNAPSHOT_UNKNOWN ? -1 : f.mode == SNAPSHOT_CHANGED ? 1 : 0;
}
static void fake_observer_dispose(struct ppp_trial_observer *o) { (void)o; }

static int fake_snprintf(char *buffer, size_t capacity, const char *format, ...) {
    if (f.format_error) return -1;
    va_list args; va_start(args, format);
    int result = vsnprintf(buffer, capacity, format, args);
    va_end(args);
    return result;
}
static int fake_fprintf(FILE *output, const char *format, ...) {
    if (f.output_error) return -1;
    va_list args; va_start(args, format);
    int result = vfprintf(output, format, args);
    va_end(args);
    return result;
}
static int fake_fflush(FILE *output) { return f.flush_error ? EOF : fflush(output); }
#undef snprintf
#define snprintf fake_snprintf
#define fprintf fake_fprintf
#define fflush fake_fflush
#define getuid fake_getuid
#define geteuid fake_geteuid
#define getgid fake_getgid
#define getegid fake_getegid
#define getpwuid fake_getpwuid
#define lstat fake_lstat
#define readlink fake_readlink
#define ppp_launch_preflight(...) fake_preflight(__VA_ARGS__)
#define sigaction(...) fake_sigaction(__VA_ARGS__)
#define clock_gettime fake_clock
#define nanosleep fake_nanosleep
#define ppp_child_start fake_start
#define ppp_child_poll fake_poll
#define ppp_child_poll_status fake_status
#define ppp_child_read fake_read
#define ppp_child_write fake_write
#define ppp_child_signal fake_signal_child
#define ppp_child_end_input fake_end
#define ppp_child_close fake_close
#define ppp_trial_observer_begin fake_observer_begin
#define ppp_trial_observer_child fake_observer_child
#define ppp_trial_observer_sample fake_observer_sample
#define ppp_trial_observer_finish fake_observer_finish
#define ppp_trial_observer_diagnostic fake_observer_diagnostic
#define ppp_trial_observer_result fake_observer_result
#define ppp_trial_observer_dispose fake_observer_dispose
#define main trial_main
#include "../src/ppp_trial_preflight.c"
#include "../src/ppp_auth_trial.c"
#undef main
#undef snprintf
#undef fprintf
#undef fflush

static void fresh(enum scenario mode) {
    memset(&f, 0, sizeof(f)); f.mode = mode;
    f.home = f.checked_home = "/private-root-home-marker";
    f.link_target = "private/var"; f.var_mode = S_IFLNK | 0777;
    f.current[0].sa_handler = SIG_IGN; f.current[1].sa_handler = SIG_DFL;
    memcpy(f.original, f.current, sizeof(f.current));
}
static void invoke(int argc, char **argv, int expected, const char *label) {
    FILE *output = tmpfile(); CHECK(output != NULL);
    if (!output) exit(1);
    CHECK(auth_trial(argc, argv, output, NULL) == expected);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    char text[4096] = {0};
    size_t count = fread(text, 1, sizeof(text) - 1, output);
    CHECK(count < sizeof(text) - 1 && !ferror(output));
    CHECK(strstr(text, label));
    if (strstr(text, "execution=blocked")) {
        CHECK(!f.spawn_calls && !f.observer_calls && !f.host_calls && !f.poll_calls &&
              !f.status_calls && !f.read_calls && !f.write_calls && !f.term_calls && !f.kill_calls);
        CHECK(strstr(text, "auth-reject=unverified child-cleanup=not-started"));
        if (!f.preflight_calls) {
            CHECK(strstr(text, "preflight=not-run"));
            CHECK(!strstr(text, "options=absent") && !strstr(text, "executable=absent"));
        } else if (f.preflight_error || f.invalid_preflight) {
            CHECK(strstr(text, f.preflight_error ? "preflight=error" : "preflight=invalid"));
            CHECK(!strstr(text, "options=absent") && !strstr(text, "executable=present"));
        } else CHECK(strstr(text, "preflight=complete executable=present"));
    }
    if (f.preflight_calls) {
        CHECK(f.common_uid_calls == 1 && f.common_home_calls == 1);
        CHECK(f.common_metadata_calls && f.leaf_mask == (1u << PPP_LAUNCH_ITEMS) - 1);
    }
    if (f.mode == INSTALL_FAIL || f.mode == SPAWN_FAIL || f.mode == START_CLOCK_FAIL) {
        CHECK(strstr(text, "child-cleanup=not-started transport=not-started"));
        CHECK(!strstr(text, "transport-abandoned"));
    }
    const char *private[] = {"private-root-home-marker", "password-private-marker", "321", "765432", "IMSI", "/usr/",
        "/var", "/private", "/safe", "private/var", "denied-private-link-marker", "EACCES", "errno="};
    for (unsigned i = 0; i < sizeof(private) / sizeof(private[0]); i++) CHECK(!strstr(text, private[i]));
    CHECK(fclose(output) == 0);
    if (f.mode != RESTORE_FAIL) {
        CHECK(f.current[0].sa_handler == f.original[0].sa_handler);
        CHECK(f.current[1].sa_handler == f.original[1].sa_handler);
    }
}
int main(void) {
    char *run[] = {"private-program-marker", "--local-auth-once", NULL};
    char *help[] = {"private-program-marker", "--help", NULL};
    char *invalid[][4] = {
        {"trial", NULL}, {"trial", "--check-host", NULL}, {"trial", "--version", NULL},
        {"trial", "--local-auth-once", "--local-auth-once", NULL},
        {"trial", "--help", "--local-auth-once", NULL},
        {"trial", "--path=password-private-marker", NULL},
        {"trial", "--local-auth-once", "private-path-marker", NULL}
    };
    fresh(NORMAL); invoke(2, help, 0, "--local-auth-once");
    CHECK(!f.host_calls && !f.identity_calls && !f.metadata_calls && !f.spawn_calls && !f.observer_calls);
    CHECK(!f.home_calls && !f.readlink_calls && !f.preflight_calls);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        fresh(NORMAL);
        invoke(invalid[i][2] ? 3 : invalid[i][1] ? 2 : 1, invalid[i], 2, "--local-auth-once");
        CHECK(!f.host_calls && !f.identity_calls && !f.metadata_calls && !f.spawn_calls && !f.observer_calls);
        CHECK(!f.home_calls && !f.readlink_calls && !f.preflight_calls);
    }
    fresh(NORMAL); invoke(0, NULL, 2, "--local-auth-once");
    CHECK(!f.host_calls && !f.spawn_calls);
    for (unsigned i = 0; i < 3; i++) {
        fresh(NORMAL);
        if (i == 0) f.uid = 765432;
        if (i == 1) f.euid = 765432;
        if (i == 2) f.egid = 7;
        invoke(2, run, 3, "gate=root-identity");
        CHECK(!f.metadata_calls && !f.spawn_calls && !f.home_calls);
    }
    const char *paths[] = {"/", "/usr", "/usr/sbin", "/usr/sbin/pppd"};
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned j = 0; j < 8; j++) {
            fresh(NORMAL); f.bad_path = paths[i]; f.bad_mode = (i == 3 ? S_IFREG : S_IFDIR) | 0755;
            if (j == 0) f.metadata_error = EACCES;
            if (j == 1) f.bad_mode = S_IFLNK | 0777;
            if (j == 2) f.bad_mode = S_IFIFO | 0600;
            if (j == 3) f.owner = 765432;
            if (j == 4) f.bad_mode |= S_IWGRP;
            if (j == 5) f.bad_mode |= S_IWOTH;
            if (j >= 6 && i < 3) continue;
            if (j == 6) f.bad_mode |= S_ISUID;
            if (j == 7) f.bad_mode = S_IFREG | 0644;
            invoke(2, run, 3, "execution=blocked");
            CHECK(!f.spawn_calls && !f.home_calls);
        }
    }
    fresh(NORMAL); f.home_error = 1;
    invoke(2, run, 3, "reason=home-lookup-error"); CHECK(!f.preflight_calls);
    fresh(NORMAL); f.home = NULL;
    invoke(2, run, 3, "reason=home-lookup-error"); CHECK(!f.preflight_calls);
    const char *malformed[] = {"", "relative", "//", "//var/root", "/var//root", "/var/root/",
        "/var/./root", "/var/../root", "/./root", "/../root", "/safe/\nmarker"};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        fresh(NORMAL); f.home = malformed[i];
        invoke(2, run, 3, "reason=home-path-error");
        CHECK(!f.readlink_calls && !f.preflight_calls);
    }
    const char *homes[] = {"/var/root", "/var/root", "/private/var/root", "/safe/root", "/various/root", "/var", "/"};
    const char *mapped[] = {"/private/var/root", "/private/var/root", "/private/var/root",
        "/safe/root", "/various/root", "/private/var", "/"};
    for (unsigned i = 0; i < sizeof(homes) / sizeof(homes[0]); i++) {
        fresh(NORMAL); f.home = homes[i]; f.checked_home = mapped[i];
        if (i == 1) f.link_target = "/private/var";
        invoke(2, run, 0, "auth-reject=observed-all");
        CHECK(f.readlink_calls == (i < 2 || i == 5 ? 1u : 0u));
        CHECK(f.home_calls == 1 && f.preflight_calls == 1);
    }
    const char *targets[] = {"", "private/var/", "/private/var/", "./private/var", "/tmp",
        "denied-private-link-marker", "private/varX", "/private/varX"};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        fresh(NORMAL); f.home = "/var/root"; f.link_target = targets[i];
        invoke(2, run, 3, "reason=alias-target-error"); CHECK(!f.preflight_calls);
    }
    for (unsigned i = 0; i < 7; i++) {
        fresh(NORMAL); f.home = "/var/root";
        const char *reason = "reason=alias-type-error";
        if (i == 0) { f.bad_path = "/var"; f.metadata_error = ENOENT; reason = "reason=alias-metadata-error"; }
        if (i == 1) f.var_mode = S_IFDIR | 0755;
        if (i == 2) { f.var_owner = 765432; reason = "reason=alias-owner-error"; }
        if (i == 3) { f.readlink_fault = 1; reason = "reason=alias-readlink-error"; }
        if (i == 4) { f.readlink_fault = 2; reason = "reason=alias-target-error"; }
        if (i == 5) f.var_mode = S_IFREG | 0644;
        if (i == 6) { f.readlink_fault = 3; reason = "reason=alias-target-error"; }
        invoke(2, run, 3, reason); CHECK(!f.preflight_calls);
    }
    /* / is checked again in home validation, independently of executable checks. */
    fresh(NORMAL); f.bad_path = "/"; f.bad_mode = S_IFDIR | 0755;
    CHECK(trial_preflight(NULL, &(struct trial_preflight){0}) == 0);
    for (unsigned i = 0; i < 4; i++) {
        fresh(NORMAL); f.bad_path = "/"; f.bad_mode = S_IFDIR | 0755;
        if (i == 0) f.metadata_error = ENOENT;
        if (i == 1) f.bad_mode = S_IFLNK | 0777;
        if (i == 2) f.owner = 765432;
        if (i == 3) f.bad_mode |= S_IWOTH;
        struct trial_preflight result;
        CHECK(trial_preflight(NULL, &result) == -1 && result.status == TRIAL_PREFLIGHT_NOT_RUN);
        CHECK(!f.preflight_calls);
    }
    const char *parents[] = {"/private", "/private/var", "/private/var/root"};
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned j = 0; j < 8; j++) {
            fresh(NORMAL); f.home = "/var/root"; f.checked_home = "/private/var/root";
            f.bad_path = parents[i]; f.bad_mode = S_IFDIR | 0755;
            const char *reason = "reason=home-metadata-error";
            if (j == 0) f.metadata_error = ENOENT;
            if (j == 1) f.metadata_error = EACCES;
            if (j == 2) f.metadata_error = ENOTDIR;
            if (j == 3) { f.bad_mode = S_IFLNK | 0777; reason = "reason=home-type-error"; }
            if (j == 4) { f.bad_mode = S_IFREG | 0644; reason = "reason=home-type-error"; }
            if (j == 5) { f.owner = 765432; reason = "reason=home-owner-mode-error"; }
            if (j == 6) { f.bad_mode |= S_IWGRP; reason = "reason=home-owner-mode-error"; }
            if (j == 7) { f.bad_mode |= S_IWOTH; reason = "reason=home-owner-mode-error"; }
            invoke(2, run, 3, reason); CHECK(!f.preflight_calls);
        }
    }
    const char *other_aliases[] = {"/safe", "/safe/root", "/various"};
    for (unsigned i = 0; i < 3; i++) {
        fresh(NORMAL); f.home = i == 2 ? "/various/root" : "/safe/root";
        f.bad_path = other_aliases[i]; f.bad_mode = S_IFLNK | 0777;
        invoke(2, run, 3, "reason=home-type-error");
        CHECK(!f.readlink_calls && !f.preflight_calls);
    }
    char long_home[PATH_MAX + 1];
    memset(long_home, 'a', sizeof(long_home)); long_home[0] = '/';
    for (size_t i = 100; i < sizeof(long_home); i += 100) long_home[i] = '/';
    long_home[sizeof(long_home) - 1] = '\0';
    fresh(NORMAL); f.home = long_home;
    invoke(2, run, 3, "reason=home-path-error"); CHECK(!f.preflight_calls);
    long_home[PATH_MAX - 1] = '\0';
    fresh(NORMAL); f.home = long_home;
    invoke(2, run, 3, "reason=home-path-error"); CHECK(!f.preflight_calls);
    memcpy(long_home, "/var", 4); /* Expansion fits, but appended .ppprc does not. */
    long_home[4] = '/'; long_home[PATH_MAX - 12] = '\0';
    fresh(NORMAL); f.home = long_home;
    invoke(2, run, 3, "reason=home-path-error"); CHECK(!f.preflight_calls);
    long_home[PATH_MAX - 5] = '\0'; long_home[PATH_MAX - 12] = 'a';
    fresh(NORMAL); f.home = long_home;
    invoke(2, run, 3, "reason=home-path-error"); CHECK(!f.preflight_calls);
    memset(long_home, 'a', sizeof(long_home)); long_home[0] = '/';
    for (size_t i = 100; i < sizeof(long_home); i += 100) long_home[i] = '/';
    long_home[PATH_MAX - sizeof("/.ppprc")] = '\0';
    fresh(NORMAL); f.home = f.checked_home = long_home;
    invoke(2, run, 0, "auth-reject=observed-all");
    long_home[PATH_MAX - sizeof("/.ppprc")] = 'a';
    long_home[PATH_MAX - sizeof("/.ppprc") + 1] = '\0';
    fresh(NORMAL); f.home = long_home;
    invoke(2, run, 3, "reason=home-path-error"); CHECK(!f.preflight_calls);
    memset(long_home, 'a', NAME_MAX + 2); long_home[0] = '/'; long_home[NAME_MAX + 2] = '\0';
    fresh(NORMAL); f.home = long_home;
    invoke(2, run, 3, "reason=home-path-error"); CHECK(!f.preflight_calls);
    long_home[NAME_MAX + 1] = '\0';
    fresh(NORMAL); f.home = f.checked_home = long_home;
    invoke(2, run, 0, "auth-reject=observed-all");
    /* The real checker must inspect mapped .ppprc, not the original alias. */
    for (unsigned i = 1; i < PPP_LAUNCH_ITEMS; i++) {
        for (unsigned j = 0; j < 3; j++) {
            fresh(NORMAL); f.home = "/var/root"; f.checked_home = "/private/var/root";
            f.bad_path = i == PPP_LAUNCH_USER_OPTIONS ? "/private/var/root/.ppprc" : common_paths[i];
            f.bad_mode = j == 1 ? S_IFLNK | 0777 : S_IFREG | 0600;
            if (j == 2) f.metadata_error = EACCES;
            char label[96];
            static const char *const names[] = {"executable", "options", "postoptions", "user-options",
                "pap-secrets", "chap-secrets", "srp-secrets", "ip-up", "ip-down", "auth-up", "auth-down",
                "ipv6-up", "ipv6-down"};
            snprintf(label, sizeof(label), "%s=%s", names[i], j == 0 ? "present" : j == 1 ? "symlink" : "access-error");
            invoke(2, run, 3, label);
            CHECK(f.preflight_calls == 1 && f.readlink_calls == 1);
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        fresh(NORMAL);
        if (!i) f.preflight_error = 1;
        else f.invalid_preflight = (int)i;
        invoke(2, run, 3, i ? "reason=ppp-preflight-invalid" : "reason=ppp-preflight-error");
    }
    fresh(NORMAL);
    struct trial_preflight diagnostic;
    CHECK(trial_preflight(NULL, &diagnostic) == 0);
    char full[1024], tiny[8];
    int diagnostic_size = trial_preflight_diagnostic(&diagnostic, full, sizeof(full));
    CHECK(diagnostic_size > 0 && (size_t)diagnostic_size == strlen(full));
    CHECK(trial_preflight_diagnostic(&diagnostic, NULL, 0) == diagnostic_size);
    CHECK(trial_preflight_diagnostic(&diagnostic, tiny, sizeof(tiny)) == diagnostic_size && tiny[7] == '\0');
    CHECK(trial_preflight_diagnostic(&diagnostic, NULL, 1) == -1);
    CHECK(trial_preflight_diagnostic(NULL, full, sizeof(full)) == -1);
    diagnostic.metadata.items[0] = (enum ppp_metadata_state)-1;
    CHECK(trial_preflight_diagnostic(&diagnostic, full, sizeof(full)) == -1);
    diagnostic = (struct trial_preflight){0};
    CHECK(trial_preflight_diagnostic(&diagnostic, full, sizeof(full)) == (int)strlen("preflight=not-run"));
    CHECK(!strcmp(full, "preflight=not-run"));
    CHECK(trial_preflight(NULL, &diagnostic) == 0);
    diagnostic.metadata.clear = 0;
    CHECK(trial_preflight_diagnostic(&diagnostic, full, sizeof(full)) == -1);
    diagnostic.status = -1;
    CHECK(trial_preflight_diagnostic(&diagnostic, full, sizeof(full)) == -1);
    diagnostic = (struct trial_preflight){0}; f.format_error = 1;
    CHECK(trial_preflight_diagnostic(&diagnostic, full, sizeof(full)) == -1);
    for (unsigned i = 0; i < 3; i++) {
        fresh(NORMAL); f.home_error = 1;
        if (i == 0) f.output_error = 1;
        if (i == 1) f.flush_error = 1;
        if (i == 2) f.format_error = 1;
        FILE *output = tmpfile(); CHECK(output != NULL);
        if (!output) exit(1);
        CHECK(auth_trial(2, run, output, NULL) == 1);
        CHECK(!f.spawn_calls && !f.observer_calls && !f.host_calls);
        CHECK(fclose(output) == 0);
    }
    fresh(NORMAL);
    const struct trial_backend defaults = {NULL, trial_identity, trial_metadata, trial_preflight};
    FILE *explicit_output = tmpfile(); CHECK(explicit_output != NULL);
    if (!explicit_output) exit(1);
    CHECK(auth_trial(2, run, explicit_output, &defaults) == 0);
    CHECK(f.spawn_calls == 1 && f.preflight_calls == 1);
    CHECK(fclose(explicit_output) == 0);
    const char *labels[] = {
        "auth-reject=observed-all", "reason=exec-error", "reason=stdout-eof", "reason=signal",
        "reason=clock-error", "reason=clock-error", "reason=probe-timeout", "kill-count=1",
        "child-cleanup=unresolved transport=transport-abandoned", "child-cleanup=ownership-lost",
        "reason=stderr-reserve-limit", "reason=stdout-error", "reason=stdin-error", "handlers=restore-failed",
        "reason=handler-install", "reason=spawn-error", "auth-reject=observed-all", "auth-reject=observed-all",
        "reason=configure-ack", "reason=stderr-reserve-limit", "reason=probe-timeout", "reason=read-count",
        "reason=write-count", "reason=child-error", "reason=clock-error"
    };
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        fresh((enum scenario)i);
        invoke(2, run, i == NORMAL || i == TERM_IGNORED ? 0 : 3, labels[i]);
        CHECK(f.spawn_calls == (i == INSTALL_FAIL || i == START_CLOCK_FAIL ? 0u : 1u));
        CHECK(f.term_calls <= 1 && f.kill_calls <= 1);
        CHECK(!f.full_poll_after_reserve);
        if (i == UNRESOLVED || i == TERM_IGNORED) CHECK(f.term_calls == 1 && f.kill_calls == 1);
        if (i == OWNERSHIP_LOST) CHECK(!f.term_calls && !f.kill_calls);
        if (i == STDERR_RESERVE || i == STDERR_FULL) CHECK(f.reserve_seen && f.status_calls > f.poll_calls);
        CHECK(f.wait_calls < 400); /* fake monotonic clock bounds every scenario */
    }
    printf("ppp_auth_trial fake-only checks=%u failures=%u (no host, pppd or root execution)\n", checks, failures);
    return failures ? 1 : 0;
}
