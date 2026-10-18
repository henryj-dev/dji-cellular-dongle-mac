#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* User tool for the optional status and control of ppp_lte_upd. Reads the
 * daemon's status file, sends one fixed command line to its control socket and
 * samples the byte counters of ppp* interfaces. Never prints addresses. */
#define DD_STATUS_PATH "/var/run/dji-cellular-dongle-mac/status.json"
#define DD_CONTROL_PATH "/var/run/dji-cellular-dongle-mac/control.sock"
#define DD_STATUS_MAX 1024u
#define DD_REPLY_MAX 128u
#define DD_REPLY_WAIT_S 5
#define DD_COUNTERS 8u
#define DD_REASON 40u
#define DD_WATCH_MISSES 3u /* status gone this many seconds in a row ends watch */

enum { DD_WAITING, DD_CONNECTING, DD_CONNECTED, DD_RETRY_WAIT, DD_REPLUG, DD_PAUSED, DD_STOPPING, DD_BLOCKED };
static const char *const dd_states[] = {"waiting", "connecting", "connected", "retry-wait", "replug-required",
    "paused", "stopping", "blocked"};
static const char *const dd_qualities[] = {"ok", "partial", "unproven"};
static const char *const dd_ends[] = {"none", "stopped", "ended", "cleanup-required", "blocked"};

struct dd_counter { char name[IFNAMSIZ]; uint32_t rx, tx; };
/* OS boundary, replaced as a whole in tests. */
struct dd_backend {
    void *context;
    /* Bytes of the status file, -1 with *error (errno value, EINVAL: not a root-owned file). */
    long (*read_status)(void *context, char *buffer, size_t size, int *error);
    /* Sends command plus newline; 0 with one reply line (no newline, terminated), -1 with *error. */
    int (*request)(void *context, const char *command, char *reply, size_t size, int *error);
    /* Byte counters of ppp* point-to-point interfaces; the count, -1 on error. */
    int (*counters)(void *context, struct dd_counter *list, unsigned capacity);
    int64_t (*now_wall)(void *context);
    void (*sleep)(void *context, unsigned ms);
    int (*interrupted)(void *context);
};
/* Parsed status; -1 times and has_* = 0 mean null. */
struct dd_status {
    int state, devices, paused, quality, has_rssi, rssi, has_rsrp, rsrp, has_rsrq, rsrq_tenths, end_result;
    int64_t session_started, connected_since, retry_at, measured_at, end_at, updated;
    char end_reason[DD_REASON + 1];
};

/* ---- Status parsing: our own fixed keys; every value validated ---- */
static const char *dd_key(const char *text, const char *key) {
    char pattern[32];
    (void)snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(text, pattern);
    return p ? p + strlen(pattern) : NULL;
}
/* null, or -?[0-9]{1,12}; *end after the value. */
static int dd_int(const char *p, int64_t *value, const char **end) {
    if (!p) return 0;
    if (!strncmp(p, "null", 4)) { *value = -1; *end = p + 4; return 2; }
    int negative = *p == '-', digits = 0;
    int64_t v = 0;
    if (negative) p++;
    while (*p >= '0' && *p <= '9' && digits <= 12) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (!digits || digits > 12) return 0;
    *value = negative ? -v : v; *end = p;
    return 1;
}
static int dd_time(const char *text, const char *key, int64_t *value) {
    const char *end;
    int kind = dd_int(dd_key(text, key), value, &end);
    return kind && (*end == ',' || *end == '}') && (kind == 2 || *value >= 0);
}
static int dd_small(const char *text, const char *key, int *has, int *value) {
    const char *end;
    int64_t v;
    int kind = dd_int(dd_key(text, key), &v, &end);
    if (!kind || (*end != ',' && *end != '}') || (kind == 1 && (v < -999 || v > 999))) return 0;
    *has = kind == 1; *value = kind == 1 ? (int)v : 0;
    return 1;
}
/* "label" from the list (index), or null (-1) when allowed. */
static int dd_label(const char *text, const char *key, const char *const *list, int count, int null_ok, int *index) {
    const char *p = dd_key(text, key);
    if (!p) return 0;
    if (null_ok && !strncmp(p, "null", 4) && (p[4] == ',' || p[4] == '}')) { *index = -1; return 1; }
    for (int i = 0; i < count; i++) {
        size_t n = strlen(list[i]);
        if (p[0] == '"' && !strncmp(p + 1, list[i], n) && p[n + 1] == '"' && (p[n + 2] == ',' || p[n + 2] == '}')) {
            *index = i; return 1;
        }
    }
    return 0;
}
static int dd_parse(const char *text, size_t length, struct dd_status *s) {
    *s = (struct dd_status){0};
    if (length < 2 || length >= DD_STATUS_MAX || memchr(text, 0, length)) return -1;
    char copy[DD_STATUS_MAX];
    memcpy(copy, text, length); copy[length] = 0;
    if (copy[length - 1] == '\n') copy[--length] = 0;
    if (strncmp(copy, "{\"version\":1,", 13) || copy[length - 1] != '}') return -1;
    for (size_t i = 0; i < length; i++)
        if (!strchr("{}\":,.-_abcdefghijklmnopqrstuvwxyz0123456789", copy[i])) return -1;
    int64_t devices;
    const char *end, *p = dd_key(copy, "devices");
    int paused_true = 0, quality, has_rsrq = 0;
    if (!dd_label(copy, "state", dd_states, 8, 0, &s->state) || dd_int(p, &devices, &end) != 1 ||
        (*end != ',' && *end != '}') || devices < -1 || devices > 99) return -1;
    s->devices = (int)devices;
    p = dd_key(copy, "paused");
    if (p && !strncmp(p, "true,", 5)) paused_true = 1;
    else if (!p || strncmp(p, "false,", 6)) return -1;
    s->paused = paused_true;
    if (!dd_time(copy, "session_started", &s->session_started) || !dd_time(copy, "connected_since", &s->connected_since) ||
        !dd_time(copy, "retry_at", &s->retry_at) || !dd_time(copy, "measured_at", &s->measured_at) ||
        !dd_time(copy, "at", &s->end_at) || !dd_time(copy, "updated", &s->updated) ||
        !dd_label(copy, "quality", dd_qualities, 3, 1, &quality) ||
        !dd_small(copy, "rssi_dbm", &s->has_rssi, &s->rssi) || !dd_small(copy, "rsrp_dbm", &s->has_rsrp, &s->rsrp) ||
        !dd_label(copy, "result", dd_ends, 5, 0, &s->end_result)) return -1;
    s->quality = quality + 1; /* 0: never measured */
    /* rsrq_db: null or -?[0-9]{1,2}.[0-9] */
    p = dd_key(copy, "rsrq_db");
    if (!p) return -1;
    if (!strncmp(p, "null", 4)) p += 4;
    else {
        int negative = *p == '-', digits = 0, v = 0;
        if (negative) p++;
        while (*p >= '0' && *p <= '9' && digits <= 2) { v = v * 10 + (*p - '0'); p++; digits++; }
        if (!digits || digits > 2 || p[0] != '.' || p[1] < '0' || p[1] > '9') return -1;
        v = v * 10 + (p[1] - '0'); p += 2;
        has_rsrq = 1; s->rsrq_tenths = negative ? -v : v;
    }
    if (*p != ',' && *p != '}') return -1;
    s->has_rsrq = has_rsrq;
    /* reason: null or "[a-z0-9-]{1,40}" */
    p = dd_key(copy, "reason");
    if (!p) return -1;
    if (!strncmp(p, "null", 4)) p += 4;
    else {
        size_t n = 0;
        if (*p++ != '"') return -1;
        while (n <= DD_REASON && ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '-')) n++;
        if (!n || n > DD_REASON || p[n] != '"') return -1;
        memcpy(s->end_reason, p, n); s->end_reason[n] = 0;
        p += n + 1;
    }
    return *p == ',' || *p == '}' ? 0 : -1;
}

/* ---- Formatting ---- */
static void dd_duration(char *out, size_t size, int64_t seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 60) (void)snprintf(out, size, "%llds", (long long)seconds);
    else if (seconds < 3600) (void)snprintf(out, size, "%lldm %02llds", (long long)(seconds / 60), (long long)(seconds % 60));
    else (void)snprintf(out, size, "%lldh %02lldm", (long long)(seconds / 3600), (long long)(seconds % 3600 / 60));
}
static void dd_rate(char *out, size_t size, uint64_t bytes_per_second) {
    uint64_t bits = bytes_per_second * 8;
    if (bits < 1000) (void)snprintf(out, size, "%llu bit/s", (unsigned long long)bits);
    else if (bits < 1000000) (void)snprintf(out, size, "%.2f kbit/s", (double)bits / 1e3);
    else (void)snprintf(out, size, "%.2f Mbit/s", (double)bits / 1e6);
}
static void dd_signal(char *out, size_t size, const struct dd_status *s) {
    if (!s->quality) { (void)snprintf(out, size, "not measured yet"); return; }
    char rsrp[16] = "unknown", rsrq[16] = "unknown", rssi[16] = "unknown";
    if (s->has_rsrp) (void)snprintf(rsrp, sizeof(rsrp), "%d dBm", s->rsrp);
    if (s->has_rsrq) {
        int t = s->rsrq_tenths < 0 ? -s->rsrq_tenths : s->rsrq_tenths;
        (void)snprintf(rsrq, sizeof(rsrq), "%s%d.%d dB", s->rsrq_tenths < 0 ? "-" : "", t / 10, t % 10);
    }
    if (s->has_rssi) (void)snprintf(rssi, sizeof(rssi), "%d dBm", s->rssi);
    (void)snprintf(out, size, "RSRP %s, RSRQ %s, RSSI %s", rsrp, rsrq, rssi);
}
/* The state with what it means right now, e.g. "connected (12m 03s)". */
static void dd_state(char *out, size_t size, const struct dd_status *s, int64_t now) {
    char time_text[32];
    const char *name = dd_states[s->state];
    if (s->state == DD_CONNECTED && s->connected_since >= 0) {
        dd_duration(time_text, sizeof(time_text), now - s->connected_since);
        (void)snprintf(out, size, "%s (%s)", name, time_text);
    } else if (s->state == DD_CONNECTING && s->session_started >= 0) {
        dd_duration(time_text, sizeof(time_text), now - s->session_started);
        (void)snprintf(out, size, "%s (%s)", name, time_text);
    } else if (s->state == DD_RETRY_WAIT && s->retry_at >= 0) {
        dd_duration(time_text, sizeof(time_text), s->retry_at - now);
        (void)snprintf(out, size, "%s (next try in %s)", name, time_text);
    } else if (s->state == DD_WAITING)
        (void)snprintf(out, size, "%s (%s)", name, s->devices == 0 ? "no dongle" : s->devices > 1 ?
            "more than one dongle; only one is supported" : "dongle not checked yet");
    else if (s->state == DD_REPLUG) (void)snprintf(out, size, "%s (unplug the dongle and plug it in again)", name);
    else if (s->state == DD_PAUSED) (void)snprintf(out, size, "%s (dji-dongle resume to connect)", name);
    else if (s->state == DD_BLOCKED) (void)snprintf(out, size, "%s (see /var/log/dji-cellular-dongle-mac.log)", name);
    else (void)snprintf(out, size, "%s", name);
}
static void dd_print_status(FILE *out, const struct dd_status *s, int64_t now) {
    char line[160], signal_text[96], time_text[32];
    dd_state(line, sizeof(line), s, now);
    dd_signal(signal_text, sizeof(signal_text), s);
    (void)fprintf(out, "state: %s\n", line);
    (void)fprintf(out, "signal at connect: %s\n", signal_text);
    (void)fprintf(out, "paused: %s\n", s->paused ? "yes" : "no");
    if (s->end_result) {
        char reason[DD_REASON + 4] = "", ago[48] = "";
        if (s->end_reason[0]) (void)snprintf(reason, sizeof(reason), " (%s)", s->end_reason);
        if (s->end_at >= 0) {
            dd_duration(time_text, sizeof(time_text), now - s->end_at);
            (void)snprintf(ago, sizeof(ago), ", %s ago", time_text);
        }
        (void)fprintf(out, "last end: %s%s%s\n", dd_ends[s->end_result], reason, ago);
    }
}

/* ---- Commands ---- */
static void dd_usage(FILE *out) {
    (void)fputs("usage: dji-dongle status [--json] | pause | resume | reconnect | watch\n"
                "  status     state, connection time, signal at connect (no privileges needed)\n"
                "  pause      end the LTE session and stay off until resume (admin)\n"
                "  resume     connect again (admin)\n"
                "  reconnect  end the session and start a fresh one at once (admin)\n"
                "  watch      state and live RX/TX rate every second until Ctrl+C\n", out);
}
/* err NULL: quiet (watch retries). */
static int dd_load(const struct dd_backend *b, FILE *err, struct dd_status *s, char *raw, size_t size, size_t *length) {
    int error = 0;
    long count = b->read_status(b->context, raw, size, &error);
    if (!err) return count < 0 || dd_parse(raw, (size_t)count, s) ? 1 : (*length = (size_t)count, 0);
    if (count < 0) {
        if (error == ENOENT) (void)fputs("dji-dongle: no status (daemon not installed or not running)\n", err);
        else (void)fputs("dji-dongle: cannot read the status file\n", err);
        return 1;
    }
    if (dd_parse(raw, (size_t)count, s)) { (void)fputs("dji-dongle: the status file is not valid\n", err); return 1; }
    *length = (size_t)count;
    return 0;
}
static int dd_status_command(const struct dd_backend *b, int json, FILE *out, FILE *err) {
    char raw[DD_STATUS_MAX];
    size_t length = 0;
    struct dd_status s;
    if (dd_load(b, err, &s, raw, sizeof(raw), &length)) return 1;
    if (json) {
        if (raw[length - 1] == '\n') length--;
        (void)fprintf(out, "%.*s\n", (int)length, raw);
    } else dd_print_status(out, &s, b->now_wall(b->context));
    return 0;
}
static int dd_control(const struct dd_backend *b, const char *command, FILE *out, FILE *err) {
    char reply[DD_REPLY_MAX];
    int error = 0;
    if (b->request(b->context, command, reply, sizeof(reply), &error)) {
        if (error == EACCES || error == EPERM) (void)fprintf(err, "need an admin account (try: sudo dji-dongle %s)\n", command);
        else if (error == ENOENT || error == ECONNREFUSED)
            (void)fputs("dji-dongle: daemon not installed or not running (no control socket)\n", err);
        else if (error == ETIMEDOUT) (void)fputs("dji-dongle: no reply from the daemon\n", err);
        else (void)fputs("dji-dongle: control socket error\n", err);
        return 1;
    }
    static const struct { const char *reply, *pause, *resume, *reconnect; int code; } map[] = {
        {"ok", "paused", "resumed", "reconnecting", 0},
        {"ok ending-session", "pausing (current session is ending; up to 60 s)", NULL, NULL, 0},
        {"ok already-paused", "already paused", NULL, NULL, 0},
        {"ok already-running", NULL, "already running", NULL, 0},
        {"ok not-saved", "paused (not saved: a daemon restart resumes)", "resumed (not saved: a daemon restart pauses again)",
            NULL, 0},
        {"error=denied", NULL, NULL, NULL, 3},
        {"error=paused", NULL, NULL, "paused: run dji-dongle resume first", 1},
        {"error=replug-required", NULL, NULL, "unplug the dongle and plug it in again first", 1},
        {"error=no-device", NULL, NULL, "no dongle plugged in", 1},
        {"error=stopping", "the daemon is stopping", "the daemon is stopping", "the daemon is stopping", 1},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(reply, map[i].reply)) continue;
        if (map[i].code == 3) {
            (void)fprintf(err, "need an admin account (try: sudo dji-dongle %s)\n", command);
            return 1;
        }
        const char *text = !strcmp(command, "pause") ? map[i].pause : !strcmp(command, "resume") ? map[i].resume :
            map[i].reconnect;
        if (!text) break;
        (void)fprintf(map[i].code ? err : out, "%s\n", text);
        return map[i].code;
    }
    (void)fputs("dji-dongle: unexpected reply from the daemon\n", err); /* never echoed */
    return 1;
}
/* Bytes per second over one interval for interfaces present in both samples;
 * 32-bit counters may wrap once. 1 rates, 0 no ppp interface, -1 a jump of
 * 2^31 bytes or more (the interface was recreated): no rate. */
static int dd_rates(const struct dd_counter *a, int na, const struct dd_counter *z, int nz, uint64_t *rx, uint64_t *tx) {
    int found = 0;
    *rx = *tx = 0;
    for (int i = 0; i < nz; i++)
        for (int j = 0; j < na; j++) {
            if (strncmp(z[i].name, a[j].name, sizeof(z[i].name))) continue;
            uint32_t in = z[i].rx - a[j].rx, out = z[i].tx - a[j].tx;
            if (in >= 0x80000000u || out >= 0x80000000u) return -1;
            *rx += in; *tx += out;
            found = 1;
        }
    return found;
}
static int dd_watch(const struct dd_backend *b, FILE *out, FILE *err) {
    unsigned misses = 0;
    while (!b->interrupted(b->context)) {
        struct dd_counter first[DD_COUNTERS], second[DD_COUNTERS];
        int na = b->counters(b->context, first, DD_COUNTERS);
        b->sleep(b->context, 1000);
        if (b->interrupted(b->context)) break;
        int nz = b->counters(b->context, second, DD_COUNTERS);
        char raw[DD_STATUS_MAX], line[160], rate_rx[24], rate_tx[24], signal_text[96];
        size_t length;
        struct dd_status s;
        /* The file is replaced by rename and gone briefly across a daemon
         * restart: retry a few intervals before giving up. */
        if (dd_load(b, ++misses >= DD_WATCH_MISSES ? err : NULL, &s, raw, sizeof(raw), &length)) {
            if (misses >= DD_WATCH_MISSES) return 1;
            (void)fputs("state: unknown (no status; retrying)\n", out);
            (void)fflush(out);
            continue;
        }
        misses = 0;
        dd_state(line, sizeof(line), &s, b->now_wall(b->context));
        uint64_t rx, tx;
        if (na >= 0 && nz >= 0 && dd_rates(first, na, second, nz, &rx, &tx) > 0) {
            dd_rate(rate_rx, sizeof(rate_rx), rx); dd_rate(rate_tx, sizeof(rate_tx), tx);
        } else {
            (void)snprintf(rate_rx, sizeof(rate_rx), "-"); (void)snprintf(rate_tx, sizeof(rate_tx), "-");
        }
        dd_signal(signal_text, sizeof(signal_text), &s);
        (void)fprintf(out, "%s | %s | RX %s TX %s\n", line, signal_text, rate_rx, rate_tx);
        (void)fflush(out);
    }
    return 0;
}
/* Private fake-TU entry: the exit code (0 done, 1 failed, 2 usage). */
static int dd_run(const struct dd_backend *b, int argc, char **argv, FILE *out, FILE *err) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { dd_usage(out); return 0; }
    if (argc == 2 && !strcmp(argv[1], "status")) return dd_status_command(b, 0, out, err);
    if (argc == 3 && !strcmp(argv[1], "status") && !strcmp(argv[2], "--json")) return dd_status_command(b, 1, out, err);
    if (argc == 2 && !strcmp(argv[1], "watch")) return dd_watch(b, out, err);
    if (argc == 2 && (!strcmp(argv[1], "pause") || !strcmp(argv[1], "resume") || !strcmp(argv[1], "reconnect")))
        return dd_control(b, argv[1], out, err);
    dd_usage(err);
    return 2;
}

/* ---- Native backend ---- */
/* Fixed locations and owner; tests point them at a temporary tree. */
static const char *dd_status_path = DD_STATUS_PATH;
static const char *dd_control_path = DD_CONTROL_PATH;
static uid_t dd_status_owner = 0;
static int dd_reply_wait_s = DD_REPLY_WAIT_S;
static long native_read_status(void *context, char *buffer, size_t size, int *error) {
    (void)context;
    int fd = open(dd_status_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { *error = errno; return -1; }
    struct stat st;
    /* Only the daemon's own file: root-owned regular, small. */
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != dd_status_owner || st.st_size <= 0 || (size_t)st.st_size >= size) {
        (void)close(fd); *error = EINVAL; return -1;
    }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t count = read(fd, buffer + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += (size_t)count;
    }
    (void)close(fd);
    if (!done) { *error = EINVAL; return -1; }
    return (long)done;
}
static int native_request(void *context, const char *command, char *reply, size_t size, int *error) {
    (void)context;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t path_length = strlen(dd_control_path);
    if (path_length >= sizeof(address.sun_path)) { *error = ENAMETOOLONG; return -1; }
    memcpy(address.sun_path, dd_control_path, path_length + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *error = errno; return -1; }
    int one = 1;
    struct timeval wait = {.tv_sec = dd_reply_wait_s};
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));
    char line[32];
    int length = snprintf(line, sizeof(line), "%s\n", command);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) ||
        length < 0 || write(fd, line, (size_t)length) != length) {
        *error = errno; (void)close(fd); return -1;
    }
    size_t used = 0;
    while (used + 1 < size) {
        ssize_t count = read(fd, reply + used, size - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { *error = errno == EAGAIN || errno == EWOULDBLOCK ? ETIMEDOUT : errno; (void)close(fd); return -1; }
        if (count == 0) break;
        used += (size_t)count;
        if (memchr(reply, '\n', used)) break;
    }
    (void)close(fd);
    reply[used] = 0;
    char *newline = strchr(reply, '\n');
    if (!newline) { *error = ETIMEDOUT; return -1; }
    *newline = 0;
    return 0;
}
static int native_counters(void *context, struct dd_counter *list, unsigned capacity) {
    (void)context;
    struct ifaddrs *all;
    if (getifaddrs(&all)) return -1;
    unsigned count = 0;
    for (struct ifaddrs *i = all; i && count < capacity; i = i->ifa_next) {
        /* Link-level entries only: counters, never addresses. */
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_LINK || !i->ifa_data || !(i->ifa_flags & IFF_POINTOPOINT) ||
            strncmp(i->ifa_name, "ppp", 3) || strlen(i->ifa_name) >= sizeof(list[count].name)) continue;
        const struct if_data *data = i->ifa_data;
        memcpy(list[count].name, i->ifa_name, strlen(i->ifa_name) + 1);
        list[count].rx = data->ifi_ibytes; list[count].tx = data->ifi_obytes;
        count++;
    }
    freeifaddrs(all);
    return (int)count;
}
static int64_t native_now_wall(void *context) { (void)context; time_t now = time(NULL); return now < 0 ? 0 : now; }
static volatile sig_atomic_t dd_interrupt;
static void native_sleep(void *context, unsigned ms) {
    (void)context;
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&delay, NULL); /* Ctrl+C cuts it short */
}
static int native_interrupted(void *context) { (void)context; return dd_interrupt != 0; }
static void dd_on_interrupt(int number) { (void)number; dd_interrupt = 1; }
int main(int argc, char **argv) {
    struct sigaction interrupt = {.sa_handler = dd_on_interrupt}, ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&interrupt.sa_mask); sigemptyset(&ignore.sa_mask);
    (void)sigaction(SIGINT, &interrupt, NULL); (void)sigaction(SIGTERM, &interrupt, NULL);
    (void)sigaction(SIGPIPE, &ignore, NULL);
    const struct dd_backend native = {NULL, native_read_status, native_request, native_counters, native_now_wall,
        native_sleep, native_interrupted};
    return dd_run(&native, argc, argv, stdout, stderr);
}
