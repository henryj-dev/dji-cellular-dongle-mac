#define _DARWIN_C_SOURCE
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <membership.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
/* Optional status and control (see docs/ppp-design.md). The status file holds
 * fixed labels and validated numbers only; the socket takes four fixed commands. */
/* Everything under /var/run lives in one verified root:wheel 0755 directory
 * and is reached relative to its open descriptor, never by path. */
#define UPD_RUN_DIR "/var/run/dji-cellular-dongle-mac"
#define UPD_STATUS_NAME "status.json"
#define UPD_STATUS_TMP_NAME "status.json.tmp"
#define UPD_CONTROL_NAME "control.sock"
#define UPD_PAUSE_DIR "/Library/Application Support/dji-cellular-dongle-mac"
#define UPD_PAUSE_NAME "paused"
#define UPD_STATUS_JSON 640u
#define UPD_STATUS_RETRY_MS 2000u
#define UPD_CONTROL_LINE 64u
#define UPD_CONTROL_WAIT_MS 200u /* all clients of one slice together */
#define UPD_CONTROL_CLIENTS 4u
#define UPD_REASON 40u

enum upd_state { UPD_WAITING, UPD_CONNECTING, UPD_CONNECTED, UPD_RETRY_WAIT, UPD_REPLUG, UPD_PAUSED, UPD_STOPPING,
    UPD_BLOCKED };
static const char *const upd_states[] = {"waiting", "connecting", "connected", "retry-wait", "replug-required",
    "paused", "stopping", "blocked"};
enum { UPD_QUALITY_NONE, UPD_QUALITY_OK, UPD_QUALITY_PARTIAL, UPD_QUALITY_UNPROVEN };
static const char *const upd_qualities[] = {NULL, "ok", "partial", "unproven"};
enum { UPD_END_NONE, UPD_END_STOPPED, UPD_END_ENDED, UPD_END_CLEANUP, UPD_END_BLOCKED };
static const char *const upd_ends[] = {"none", "stopped", "ended", "cleanup-required", "blocked"};
enum { UPD_PEER_DENIED, UPD_PEER_ROOT, UPD_PEER_ADMIN };

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
    int64_t (*now_wall)(void *context);             /* unix seconds, -1 unknown */
    /* Replaces the status file: 0 written, 1 refused (unsafe directory), -1 failed. */
    int (*write_status)(void *context, const char *json, size_t length);
    void (*remove_status)(void *context);
    /* Persisted pause: 1 paused, 0 not, -1 refused (unsafe owner or type). */
    int (*paused_read)(void *context);
    int (*paused_write)(void *context, int paused); /* 0 done, -1 failed */
    /* Listening socket, -1 refused (unsafe existing path), -2 failed.
     * *root_only: no admin group, only root may connect. */
    int (*control_open)(void *context, int *root_only);
    /* 1 one client (*peer: UPD_PEER_*), 0 none waiting, -1 error. Never blocks. */
    int (*control_accept)(void *context, int listener, int *client, int *peer);
    /* As read: bytes, 0 at EOF, -1 nothing yet. Waits at most wait_ms. */
    long (*client_read)(void *context, int client, char *buffer, size_t size, unsigned wait_ms);
    void (*client_reply)(void *context, int client, const char *text, size_t length);
    void (*client_close)(void *context, int client);
    void (*control_close)(void *context, int listener);
};
/* Snapshot published as status.json; -1 times mean null. */
struct upd_status {
    int state, devices;
    int64_t session_started, connected_since, retry_at, updated;
    int quality, has_rssi, rssi, has_rsrp, rsrp, has_rsrq, rsrq_tenths;
    int64_t measured_at;
    int end_result;
    char end_reason[UPD_REASON + 1];
    int64_t end_at;
    char last[UPD_STATUS_JSON]; /* last published body, "updated" left out */
    int disabled, write_failed;
    uint64_t retry_due;         /* monotonic ms of the next retry after a failed write */
};
struct upd {
    const struct upd_backend *b;
    FILE *output;
    const char *log_path;  /* NULL: never reopened (tests) */
    volatile sig_atomic_t *stop;
    unsigned backoff_ms, fast_fails;
    int last_devices, replug;
    struct upd_status s;
    /* Control: listener (-1 none); session_running while a --up child runs;
     * end_session asks that child to end (pause or reconnect); wake cuts a wait. */
    int control, control_error, paused, session_running, end_session, reconnect, wake;
};
struct upd_child {
    int ended, exit_code, signaled, connected, killed, forwarded, reap_error, blocked, spawn_error, session_error;
    int session, result_seen;
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
/* ---- Status snapshot ---- */
static int64_t upd_wall(struct upd *u) {
    int64_t wall = u->b->now_wall(u->b->context);
    return wall < 0 ? -1 : wall;
}
static void upd_time_text(char *out, size_t size, int64_t value) {
    if (value < 0) (void)snprintf(out, size, "null");
    else (void)snprintf(out, size, "%lld", (long long)value);
}
/* Flat JSON line of fixed labels and validated numbers; 0 if it does not fit. */
static size_t upd_render(const struct upd_status *s, int paused, int64_t updated, char *out, size_t size) {
    char started[24], since[24], retry[24], measured[24], at[24], stamp[24];
    char quality[16] = "null", rssi[16] = "null", rsrp[16] = "null", rsrq[16] = "null", reason[UPD_REASON + 3] = "null";
    upd_time_text(started, sizeof(started), s->session_started);
    upd_time_text(since, sizeof(since), s->connected_since);
    upd_time_text(retry, sizeof(retry), s->retry_at);
    upd_time_text(measured, sizeof(measured), s->measured_at);
    upd_time_text(at, sizeof(at), s->end_at);
    upd_time_text(stamp, sizeof(stamp), updated);
    if (s->quality > UPD_QUALITY_NONE && s->quality <= UPD_QUALITY_UNPROVEN)
        (void)snprintf(quality, sizeof(quality), "\"%s\"", upd_qualities[s->quality]);
    if (s->has_rssi) (void)snprintf(rssi, sizeof(rssi), "%d", s->rssi);
    if (s->has_rsrp) (void)snprintf(rsrp, sizeof(rsrp), "%d", s->rsrp);
    if (s->has_rsrq) {
        int tenths = s->rsrq_tenths < 0 ? -s->rsrq_tenths : s->rsrq_tenths;
        (void)snprintf(rsrq, sizeof(rsrq), "%s%d.%d", s->rsrq_tenths < 0 ? "-" : "", tenths / 10, tenths % 10);
    }
    if (s->end_reason[0]) (void)snprintf(reason, sizeof(reason), "\"%s\"", s->end_reason);
    int state = s->state >= 0 && s->state <= UPD_BLOCKED ? s->state : UPD_WAITING;
    int end = s->end_result >= 0 && s->end_result <= UPD_END_BLOCKED ? s->end_result : UPD_END_NONE;
    int n = snprintf(out, size, "{\"version\":1,\"state\":\"%s\",\"devices\":%d,\"paused\":%s,\"session_started\":%s,"
        "\"connected_since\":%s,\"retry_at\":%s,\"signal\":{\"quality\":%s,\"rssi_dbm\":%s,\"rsrp_dbm\":%s,"
        "\"rsrq_db\":%s,\"measured_at\":%s},\"last_end\":{\"result\":\"%s\",\"reason\":%s,\"at\":%s},\"updated\":%s}\n",
        upd_states[state], s->devices < 0 ? -1 : s->devices > 99 ? 99 : s->devices, paused ? "true" : "false", started, since, retry,
        quality, rssi, rsrp, rsrq, measured, upd_ends[end], reason, at, stamp);
    return n < 0 || (size_t)n >= size ? 0 : (size_t)n;
}
/* Writes the file when anything but "updated" changed. A failed write is
 * retried from the wait slices; an unsafe directory disables the file. */
static void upd_publish(struct upd *u) {
    char body[UPD_STATUS_JSON], json[UPD_STATUS_JSON];
    if (!upd_render(&u->s, u->paused, -1, body, sizeof(body)) || !strcmp(body, u->s.last)) return;
    memcpy(u->s.last, body, sizeof(body));
    int64_t wall = upd_wall(u);
    if (wall > u->s.updated) u->s.updated = wall; /* never goes back */
    if (u->s.disabled) return;
    size_t length = upd_render(&u->s, u->paused, u->s.updated, json, sizeof(json));
    int written = length ? u->b->write_status(u->b->context, json, length) : -1;
    if (written > 0) { u->s.disabled = 1; upd_log(u, "upd=status-blocked"); return; }
    if (written < 0) {
        if (!u->s.write_failed) upd_log(u, "upd=status-error");
        u->s.write_failed = 1; u->s.last[0] = 0;
        uint64_t now = u->b->now(u->b->context);
        u->s.retry_due = now == UINT64_MAX ? UINT64_MAX : now + UPD_STATUS_RETRY_MS;
        return;
    }
    u->s.write_failed = 0;
}
static void upd_state(struct upd *u, int state) { u->s.state = state; upd_publish(u); }

/* ---- Strict child-line parsing: only allowlisted labels and bounded numbers
 * reach the status; anything else stays null. ---- */
/* The value after " key=" (or key= at the line start) up to the next space. */
static int upd_value(const char *line, const char *key, char *out, size_t size) {
    size_t length = strlen(key);
    for (const char *p = line; (p = strstr(p, key)) != NULL; p++) {
        if ((p != line && p[-1] != ' ') || p[length] != '=') continue;
        const char *value = p + length + 1;
        size_t n = strcspn(value, " ");
        if (!n || n >= size) return 0;
        memcpy(out, value, n); out[n] = 0;
        return 1;
    }
    return 0;
}
static int upd_label(const char *text) {
    size_t n = strlen(text);
    if (!n || n > UPD_REASON) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') || text[i] == '-')) return 0;
    return 1;
}
/* -?[0-9]{1,3}, or with tenths -?[0-9]{1,2}.[0-9] scaled by ten. */
static int upd_number(const char *text, int tenths, int *value) {
    const char *p = text;
    int negative = *p == '-', v = 0, digits = 0;
    if (negative) p++;
    while (*p >= '0' && *p <= '9' && digits <= 3) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (!digits || digits > (tenths ? 2 : 3)) return 0;
    if (tenths) {
        if (p[0] != '.' || p[1] < '0' || p[1] > '9') return 0;
        v = v * 10 + (p[1] - '0'); p += 2;
    }
    if (*p) return 0;
    *value = negative ? -v : v;
    return 1;
}
static void upd_parse(struct upd *u, struct upd_child *k, const char *line) {
    struct upd_status *s = &u->s;
    char value[48];
    if (!strncmp(line, "up-signal=", 10)) {
        if (!upd_value(line, "up-signal", value, sizeof(value))) return;
        int quality = !strcmp(value, "ok") ? UPD_QUALITY_OK : !strcmp(value, "partial") ? UPD_QUALITY_PARTIAL :
            !strcmp(value, "unproven") ? UPD_QUALITY_UNPROVEN : UPD_QUALITY_NONE;
        if (!quality) return;
        s->quality = quality;
        s->has_rssi = upd_value(line, "rssi-dbm", value, sizeof(value)) && upd_number(value, 0, &s->rssi);
        s->has_rsrp = upd_value(line, "rsrp-dbm", value, sizeof(value)) && upd_number(value, 0, &s->rsrp);
        s->has_rsrq = upd_value(line, "rsrq-db", value, sizeof(value)) && upd_number(value, 1, &s->rsrq_tenths);
        s->measured_at = upd_wall(u);
        upd_publish(u);
    } else if (!strncmp(line, "up=connected", 12)) {
        if (s->connected_since < 0) s->connected_since = upd_wall(u);
        upd_state(u, k->forwarded ? UPD_STOPPING : UPD_CONNECTED);
    } else if (!strncmp(line, "up-result=", 10) || !strncmp(line, "up=blocked ", 11)) {
        int blocked = line[2] == '=';
        if (!upd_value(line, blocked ? "up" : "up-result", value, sizeof(value))) return;
        int result = blocked ? UPD_END_BLOCKED : !strcmp(value, "stopped") ? UPD_END_STOPPED :
            !strcmp(value, "ended") ? UPD_END_ENDED : !strcmp(value, "cleanup-required") ? UPD_END_CLEANUP : UPD_END_NONE;
        if (!result) return;
        k->result_seen = 1;
        s->end_result = result; s->end_at = upd_wall(u); s->end_reason[0] = 0;
        if (upd_value(line, blocked ? "gate" : "reason", value, sizeof(value)) && upd_label(value))
            memcpy(s->end_reason, value, strlen(value) + 1);
        upd_publish(u);
    }
}

/* ---- Control socket: one line in, one line out, handled within a slice ---- */
static const char *const upd_commands[] = {"status", "pause", "resume", "reconnect"};
static void upd_reply(struct upd *u, int client, const char *text) {
    u->b->client_reply(u->b->context, client, text, strlen(text));
}
static void upd_save_pause(struct upd *u, int client, int paused, const char *saved_reply) {
    if (!u->b->paused_write(u->b->context, paused)) { upd_reply(u, client, saved_reply); return; }
    upd_log(u, "upd=pause-save-error");
    upd_reply(u, client, "ok not-saved\n");
}
static void upd_command(struct upd *u, int client, int command) {
    if (command == 0) {
        char json[UPD_STATUS_JSON];
        size_t length = upd_render(&u->s, u->paused, u->s.updated, json, sizeof(json));
        if (length) u->b->client_reply(u->b->context, client, json, length);
        return;
    }
    if (*u->stop) { upd_reply(u, client, "error=stopping\n"); return; } /* stop wins */
    if (command == 1) {
        if (u->paused) { upd_reply(u, client, "ok already-paused\n"); return; }
        u->paused = 1;
        /* A running session ends like a stop request; otherwise the wait is cut. */
        if (u->session_running) { u->end_session = 1; u->reconnect = 0; } else u->wake = 1;
        upd_publish(u);
        upd_save_pause(u, client, 1, u->session_running ? "ok ending-session\n" : "ok\n");
    } else if (command == 2) {
        if (!u->paused) { upd_reply(u, client, "ok already-running\n"); return; }
        /* A required replug stays required: only a fresh plug resets the modem. */
        u->paused = 0; u->backoff_ms = UPD_BACKOFF_MIN_MS; u->fast_fails = 0; u->wake = 1;
        if (u->end_session) u->reconnect = 1; /* the pause is still ending the session */
        upd_publish(u);
        upd_save_pause(u, client, 0, "ok\n");
    } else if (u->paused) upd_reply(u, client, "error=paused\n");
    else if (u->replug) upd_reply(u, client, "error=replug-required\n");
    else if (u->session_running) {
        u->end_session = 1; u->reconnect = 1;
        upd_reply(u, client, "ok\n");
    } else if (u->s.devices == 0) upd_reply(u, client, "error=no-device\n"); /* unknown count: try */
    else { u->wake = 1; upd_reply(u, client, "ok\n"); }
}
/* Reads one line within budget_ms; returns the time it took. */
static uint64_t upd_client(struct upd *u, int client, int peer, uint64_t budget_ms) {
    if (peer != UPD_PEER_ROOT && peer != UPD_PEER_ADMIN) {
        upd_log(u, "upd=control-denied");
        upd_reply(u, client, "error=denied\n");
        return 0;
    }
    char line[UPD_CONTROL_LINE + 1];
    size_t used = 0;
    const char *end = NULL;
    uint64_t start = u->b->now(u->b->context), now = start;
    while (!end && used < UPD_CONTROL_LINE) {
        now = u->b->now(u->b->context);
        if (now == UINT64_MAX || start == UINT64_MAX || now < start || now - start >= budget_ms) break;
        long count = u->b->client_read(u->b->context, client, line + used, UPD_CONTROL_LINE - used,
                                       (unsigned)(budget_ms - (now - start)));
        if (count == 0 || count > (long)(UPD_CONTROL_LINE - used)) break;
        if (count < 0) continue;
        used += (size_t)count;
        end = memchr(line, '\n', used);
    }
    now = u->b->now(u->b->context);
    /* Exactly one line: nothing after the newline, no NUL inside. */
    int command = -1, complete = end && (size_t)(end - line) == used - 1 && !memchr(line, 0, used - 1);
    if (complete) {
        line[used - 1] = 0;
        for (int i = 0; i < 4; i++) if (!strcmp(line, upd_commands[i])) command = i;
    }
    upd_log(u, "upd=control command=%s uid-class=%s", command < 0 ? "invalid" : upd_commands[command],
            peer == UPD_PEER_ROOT ? "root" : "admin");
    if (command < 0) upd_reply(u, client, complete ? "error=unknown-command\n" : "error=bad-request\n");
    else upd_command(u, client, command);
    return now == UINT64_MAX || start == UINT64_MAX || now < start ? budget_ms : now - start;
}
/* At most a few clients per slice, sharing one UPD_CONTROL_WAIT_MS budget. */
static void upd_control(struct upd *u) {
    uint64_t spent = 0;
    for (unsigned i = 0; u->control >= 0 && i < UPD_CONTROL_CLIENTS && spent < UPD_CONTROL_WAIT_MS; i++) {
        int client = -1, peer = UPD_PEER_DENIED;
        int accepted = u->b->control_accept(u->b->context, u->control, &client, &peer);
        if (accepted < 0 && !u->control_error) upd_log(u, "upd=control-error");
        if (accepted < 0) u->control_error = 1;
        if (accepted <= 0) return;
        u->control_error = 0;
        spent += upd_client(u, client, peer, UPD_CONTROL_WAIT_MS - spent);
        u->b->client_close(u->b->context, client);
    }
}

/* Waits in short slices so a stop request or a control command is seen within
 * one slice. Returns 1 when a command cut the wait. */
static int upd_wait(struct upd *u, unsigned ms) {
    uint64_t start = u->b->now(u->b->context);
    while (!*u->stop) {
        upd_control(u);
        if (u->s.write_failed) {
            uint64_t now = u->b->now(u->b->context);
            if (now != UINT64_MAX && now >= u->s.retry_due) upd_publish(u);
        }
        if (u->wake) { u->wake = 0; return 1; }
        uint64_t now = u->b->now(u->b->context);
        /* A failed clock still waits one slice: never a busy loop. */
        if (now == UINT64_MAX || start == UINT64_MAX) { u->b->sleep(u->b->context, UPD_SLICE_MS); return 0; }
        if (now < start || now - start >= ms) return 0;
        uint64_t left = ms - (now - start);
        u->b->sleep(u->b->context, left < UPD_SLICE_MS ? (unsigned)left : UPD_SLICE_MS);
    }
    return 0;
}
/* Forwards one complete child line with a time stamp; marks the connection. */
static void upd_line(struct upd *u, struct upd_child *k, const char *line) {
    if (!strncmp(line, "up=blocked ", 11)) k->blocked = 1;
    if (!strncmp(line, "up-result=ended reason=session-error ", 37)) k->session_error = 1;
    if (!strncmp(line, "up=connected", 12) && !k->connected) {
        k->connected = 1; k->connected_ms = u->b->now(u->b->context);
    }
    if (k->session) upd_parse(u, k, line);
    upd_log(u, "child %s", line);
}
/* forward: pass a stop request on as SIGHUP, which ppp_lte_up latches for its
 * whole run (SIGTERM is only graceful while its runner is active); SIGKILL
 * UPD_STOP_WAIT_MS later. A cleanup run is not forwarded but is killed after
 * UPD_CLEANUP_LIMIT_MS. */
static void upd_child(struct upd *u, const char *mode, int forward, struct upd_child *k) {
    *k = (struct upd_child){.exit_code = -1, .session = forward};
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
        upd_control(u);
        /* Never signal a pid already reaped: it may belong to another process now.
         * A pause or reconnect ends the session the same way; the daemon stays. */
        if (forward && (*u->stop || u->end_session) && !k->forwarded && !k->ended) {
            k->forwarded = 1; forwarded_ms = u->b->now(u->b->context);
            (void)u->b->signal(u->b->context, pid, SIGHUP);
            if (*u->stop) upd_log(u, "upd=stopping-session");
            else upd_log(u, "upd=ending-session for=%s", u->reconnect ? "reconnect" : "pause");
            upd_state(u, UPD_STOPPING);
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
/* A session without a result line: the daemon names the end itself. The
 * next state change publishes it. */
static void upd_session_ended(struct upd *u, const struct upd_child *k) {
    if (!k->result_seen) {
        const char *reason = k->spawn_error ? "spawn-error" : k->killed ? "killed" : k->reap_error ? "reap-error" :
            k->signaled ? "signaled" : "no-result";
        u->s.end_result = UPD_END_ENDED; u->s.end_at = upd_wall(u);
        (void)snprintf(u->s.end_reason, sizeof(u->s.end_reason), "%s", reason);
    }
    u->s.session_started = -1; u->s.connected_since = -1;
}
/* Private fake-TU entry. Returns 0 after a stop request, 1 on output failure. */
static int upd_run(const struct upd_backend *b, FILE *output, const char *log_path, volatile sig_atomic_t *stop) {
    struct upd u = {.b = b, .output = output, .log_path = log_path, .stop = stop, .backoff_ms = UPD_BACKOFF_MIN_MS,
        .last_devices = -2, .control = -1};
    u.s = (struct upd_status){.state = UPD_WAITING, .devices = -1, .session_started = -1, .connected_since = -1,
        .retry_at = -1, .updated = -1, .measured_at = -1, .end_at = -1};
    upd_log(&u, "upd=starting");
    int root_only = 0, control = b->control_open(b->context, &root_only);
    if (control == -1) upd_log(&u, "upd=control-blocked");
    else if (control < 0) upd_log(&u, "upd=control-error");
    else {
        u.control = control;
        if (root_only) upd_log(&u, "upd=control-root-only");
    }
    int saved = b->paused_read(b->context);
    /* An unverifiable pause file fails safe: paused, so no unexpected LTE use. */
    if (saved < 0) { u.paused = 1; upd_log(&u, "upd=pause-file-blocked paused=yes"); }
    else if (saved) { u.paused = 1; upd_log(&u, "upd=paused source=file"); }
    upd_publish(&u);
    /* Keys left by a previous run that was killed (shutdown, crash) must not
     * keep a dead primary while no dongle is plugged in. */
    if (b->path_ok(b->context, UPD_CHILD_PATH)) upd_cleanup(&u);
    while (!*stop) {
        int devices = b->devices(b->context);
        u.s.devices = devices < 0 ? -1 : devices;
        if (devices == 0) u.replug = 0; /* the replug the modem needed */
        if (u.paused) {
            /* No sessions; the waiting line is logged again after a resume. */
            u.last_devices = -2;
            upd_state(&u, UPD_PAUSED);
            upd_wait(&u, UPD_POLL_MS);
            continue;
        }
        if (devices != 1) {
            if (devices != u.last_devices) {
                if (devices < 0) upd_log(&u, "upd=waiting devices=unknown");
                else upd_log(&u, "upd=waiting devices=%d", devices);
            }
            u.last_devices = devices;
            upd_state(&u, UPD_WAITING);
            upd_wait(&u, UPD_POLL_MS);
            continue;
        }
        u.last_devices = devices;
        if (u.replug) { upd_state(&u, UPD_REPLUG); upd_wait(&u, UPD_POLL_MS); continue; }
        if (!b->path_ok(b->context, UPD_CHILD_PATH)) {
            upd_log(&u, "upd=blocked gate=child-path");
            upd_state(&u, UPD_BLOCKED);
            upd_wait(&u, UPD_BLOCKED_WAIT_MS);
            continue;
        }
        upd_log(&u, "upd=session-start");
        u.s.session_started = upd_wall(&u); u.s.connected_since = -1;
        u.end_session = u.reconnect = u.wake = 0;
        u.session_running = 1;
        upd_state(&u, UPD_CONNECTING);
        struct upd_child k;
        upd_child(&u, "--up", 1, &k);
        u.session_running = 0;
        upd_session_ended(&u, &k);
        /* Leftover keys or a killed child: the cleanup command, once. */
        if (k.exit_code == 4 || k.killed || k.signaled || k.reap_error) { upd_state(&u, UPD_STOPPING); upd_cleanup(&u); }
        if (*stop) break;
        if (k.connected && k.ended_ms >= k.connected_ms && k.ended_ms - k.connected_ms >= UPD_STABLE_MS)
            u.backoff_ms = UPD_BACKOFF_MIN_MS;
        int requested = u.end_session && k.forwarded, again = u.reconnect, late = 0;
        if (u.end_session) {
            u.end_session = u.reconnect = 0;
            /* Ended on request, not a failure: no backoff. A pause shows at the top. */
            if (requested) {
                if (again) upd_log(&u, "upd=reconnecting");
                continue;
            }
            /* The child had already ended on its own: normal accounting (fast
             * failures, backoff growth) below; only the following wait is cut. */
            u.wake = late = 1;
        }
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
        int64_t wall = upd_wall(&u);
        u.s.retry_at = wall < 0 ? -1 : wall + (u.backoff_ms + 999) / 1000;
        upd_state(&u, UPD_RETRY_WAIT);
        int cut = upd_wait(&u, u.backoff_ms);
        u.s.retry_at = -1;
        /* A wait cut by a command (reconnect, pause) keeps the backoff where it was. */
        if (!cut || late) u.backoff_ms = u.backoff_ms >= UPD_BACKOFF_MAX_MS / 2 ? UPD_BACKOFF_MAX_MS : u.backoff_ms * 2;
    }
    upd_state(&u, UPD_STOPPING);
    /* A clean exit leaves no status file: a stale "connected" is never shown. */
    if (!u.s.disabled) b->remove_status(b->context);
    if (u.control >= 0) b->control_close(b->context, u.control);
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
static int64_t native_now_wall(void *context) {
    (void)context;
    time_t now = time(NULL);
    return now < 0 ? -1 : (int64_t)now;
}
/* Run-time locations and expected owners; tests point them at a temporary
 * tree under build/ and their own ids. */
static const char *native_run_dir = UPD_RUN_DIR;
static const char *native_pause_dir = UPD_PAUSE_DIR;
static const char *native_admin_name = "admin";
static uid_t native_owner = 0;
static gid_t native_group = 0; /* wheel */
static int native_run_fd = -1;
static int native_dir_ok(const struct stat *st) {
    return S_ISDIR(st->st_mode) && st->st_uid == native_owner && !(st->st_mode & (S_IWGRP | S_IWOTH));
}
/* Opens path as a directory without following a symlink and checks it from
 * the descriptor: 0 with *fd, 1 refused, 2 missing (create == 0), -1 failed.
 * A directory made here gets owner:group 0755; one found existing is only checked. */
static int native_open_dir(const char *path, int create, int *fd) {
    int made = 0, d = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (d < 0 && errno == ENOENT) {
        if (!create) return 2;
        if (!mkdir(path, 0700)) made = 1;
        else if (errno != EEXIST) return -1;
        d = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (d < 0) return errno == ELOOP || errno == ENOTDIR ? 1 : -1;
    if (made && (fchown(d, native_owner, native_group) || fchmod(d, 0755))) { (void)close(d); return -1; }
    struct stat st;
    if (fstat(d, &st) || !native_dir_ok(&st)) { (void)close(d); return 1; }
    *fd = d;
    return 0;
}
/* The verified run directory, kept open; re-opened (and re-checked) only when
 * the held one no longer passes or was removed. */
static int native_run_dirfd(int create, int *fd) {
    struct stat st;
    if (native_run_fd >= 0 && !fstat(native_run_fd, &st) && st.st_nlink > 0 && native_dir_ok(&st)) {
        *fd = native_run_fd;
        return 0;
    }
    if (native_run_fd >= 0) { (void)close(native_run_fd); native_run_fd = -1; }
    int opened = native_open_dir(native_run_dir, create, &native_run_fd);
    if (opened) native_run_fd = -1;
    else *fd = native_run_fd;
    return opened;
}
static int native_write_status(void *context, const char *json, size_t length) {
    (void)context;
    int dir, opened = native_run_dirfd(1, &dir);
    if (opened) return opened == 1 ? 1 : -1;
    (void)unlinkat(dir, UPD_STATUS_TMP_NAME, 0); /* a planted entry is removed, never followed */
    int fd = openat(dir, UPD_STATUS_TMP_NAME, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int bad = fchmod(fd, 0644) != 0; /* the daemon's umask is 027 */
    for (size_t done = 0; !bad && done < length;) {
        ssize_t count = write(fd, json + done, length - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) bad = 1;
        else done += (size_t)count;
    }
    if (close(fd)) bad = 1;
    if (bad || renameat(dir, UPD_STATUS_TMP_NAME, dir, UPD_STATUS_NAME)) {
        (void)unlinkat(dir, UPD_STATUS_TMP_NAME, 0);
        return -1;
    }
    return 0;
}
static void native_remove_status(void *context) {
    (void)context;
    int dir;
    if (native_run_dirfd(0, &dir)) return;
    (void)unlinkat(dir, UPD_STATUS_NAME, 0);
    (void)unlinkat(dir, UPD_STATUS_TMP_NAME, 0);
}
static int native_paused_read(void *context) {
    (void)context;
    int dir, opened = native_open_dir(native_pause_dir, 0, &dir);
    if (opened == 2) return 0;
    if (opened) return -1;
    struct stat st;
    int result = fstatat(dir, UPD_PAUSE_NAME, &st, AT_SYMLINK_NOFOLLOW) ? (errno == ENOENT ? 0 : -1) :
        S_ISREG(st.st_mode) && st.st_uid == native_owner ? 1 : -1;
    (void)close(dir);
    return result;
}
static int native_paused_write(void *context, int paused) {
    (void)context;
    int dir, opened = native_open_dir(native_pause_dir, paused, &dir);
    if (!paused && opened == 2) return 0;
    if (opened) return -1;
    int bad;
    if (!paused) bad = unlinkat(dir, UPD_PAUSE_NAME, 0) && errno != ENOENT;
    else {
        int fd = openat(dir, UPD_PAUSE_NAME, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
        struct stat st;
        bad = fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || fchown(fd, native_owner, native_group) ||
            fchmod(fd, 0644);
        if (fd >= 0 && close(fd)) bad = 1;
    }
    (void)close(dir);
    return bad ? -1 : 0;
}
/* Admin group for peer checks; looked up once when the socket is made. */
static int native_admin_known;
static gid_t native_admin_gid;
static int native_control_open(void *context, int *root_only) {
    (void)context;
    int dir, opened = native_run_dirfd(1, &dir);
    if (opened) return opened == 1 ? -1 : -2;
    struct stat st;
    /* Only a stale socket of the owner is replaced; anything else is left alone. */
    if (!fstatat(dir, UPD_CONTROL_NAME, &st, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != native_owner) return -1;
        if (unlinkat(dir, UPD_CONTROL_NAME, 0)) return -2;
    } else if (errno != ENOENT) return -2;
    struct group *admin = getgrnam(native_admin_name);
    native_admin_known = admin != NULL;
    native_admin_gid = admin ? admin->gr_gid : native_group;
    *root_only = !native_admin_known;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    _Static_assert(sizeof(UPD_CONTROL_NAME) <= sizeof(address.sun_path), "control name too long");
    memcpy(address.sun_path, UPD_CONTROL_NAME, sizeof(UPD_CONTROL_NAME));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -2;
    /* bind() takes a path: bind the bare name with the verified directory as
     * the working directory (single-threaded), then return to the old one. */
    int cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC), bound = 0, back = 1;
    if (cwd >= 0 && !fchdir(dir)) {
        mode_t saved = umask(077);
        bound = !bind(fd, (const struct sockaddr *)&address, sizeof(address));
        (void)umask(saved);
        back = !fchdir(cwd);
    }
    if (cwd >= 0) (void)close(cwd);
    int flags = fcntl(fd, F_GETFL);
    int bad = !bound || !back || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC) ||
        fchownat(dir, UPD_CONTROL_NAME, native_owner, native_admin_gid, AT_SYMLINK_NOFOLLOW) ||
        fchmodat(dir, UPD_CONTROL_NAME, native_admin_known ? 0660 : 0600, AT_SYMLINK_NOFOLLOW) || listen(fd, 8);
    if (bad) {
        (void)close(fd);
        if (bound) (void)unlinkat(dir, UPD_CONTROL_NAME, 0);
        return -2;
    }
    return fd;
}
static int native_control_accept(void *context, int listener, int *client, int *peer) {
    (void)context;
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED ? 0 : -1;
    int one = 1, flags = fcntl(fd, F_GETFL);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    uid_t uid;
    gid_t gid;
    *peer = UPD_PEER_DENIED;
    if (!getpeereid(fd, &uid, &gid)) {
        if (uid == 0) *peer = UPD_PEER_ROOT;
        else if (native_admin_known) {
            uuid_t user, group;
            int member = 0;
            if (!mbr_uid_to_uuid(uid, user) && !mbr_gid_to_uuid(native_admin_gid, group) &&
                !mbr_check_membership(user, group, &member) && member)
                *peer = UPD_PEER_ADMIN;
        }
    }
    *client = fd;
    return 1;
}
static void native_client_reply(void *context, int client, const char *text, size_t length) {
    (void)context;
    (void)send(client, text, length, 0); /* one short line into an empty buffer; best effort */
}
static void native_control_close(void *context, int listener) {
    (void)context;
    (void)close(listener);
    int dir;
    struct stat st;
    if (!native_run_dirfd(0, &dir) && !fstatat(dir, UPD_CONTROL_NAME, &st, AT_SYMLINK_NOFOLLOW) &&
        S_ISSOCK(st.st_mode) && st.st_uid == native_owner)
        (void)unlinkat(dir, UPD_CONTROL_NAME, 0);
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
        native_read, native_close, native_signal, native_now, native_sleep, native_now_wall, native_write_status,
        native_remove_status, native_paused_read, native_paused_write, native_control_open, native_control_accept,
        native_read, native_client_reply, native_close, native_control_close};
    (void)umask(027); /* a reopened log is created 0640 */
    return upd_run(&native, stdout, UPD_LOG_PATH, &upd_stop_request);
}
