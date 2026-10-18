#define _DARWIN_C_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <grp.h>
#include <poll.h>

#define main upd_main
#include "../src/ppp_lte_upd.c"
#undef main

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-upd scenario=%d line=%d\n", scenario, __LINE__); } } while (0)
/* Virtual clock fixture: devices by time, scripted children. */
struct script {
    const char *mode;            /* expected mode */
    const char *output;          /* lines the child prints at once */
    uint64_t connect_after;      /* print "up=connected" after this, 0 = never */
    uint64_t lifetime;           /* exits on its own after this */
    int exit_code, ignores_term, spawn_fails;
    uint64_t term_exit_after;    /* after SIGHUP, exits 0 this long later */
    int output_after_end, no_eof, stop_with_end, reap_fails;
};
/* Every run starts with one cleanup of a previous run's leftovers. */
#define START_CLEAN {"--cleanup", "cleanup=clean\n", 0, 100, 0, 0, 0, 0, 0, 0, 0, 0}
static int scenario;
/* One control client: connects at `at` with a peer class and sends text. */
struct command { uint64_t at; const char *text; int peer; size_t length; /* 0: strlen */ };
#define WALL_BASE 1700000000
static struct {
    uint64_t now, plug_at, unplug_at, replug_at, stop_at;
    int devices_when_plugged, path_bad;
    const struct script *scripts;
    unsigned script_count, spawned, terms, kills, closes, reaps;
    uint64_t spawn_at[8];
    /* Running child. */
    int running, ended, status, connected_printed, eof_given, reaped_now;
    size_t output_offset;
    uint64_t started, term_at;
    const struct script *current;
    volatile sig_atomic_t stop;
    /* Status, pause file and control socket. */
    int status_result, status_writes, status_removed, status_bad, paused_file, pause_writes, pause_write_fails;
    int control_blocked, control_closed, client_open, root_only;
    unsigned client_closes;
    char status_json[UPD_STATUS_JSON];
    uint64_t last_write_at, max_write_gap, heartbeat_writes;
    const struct command *commands;
    unsigned command_count, command_next;
    size_t client_offset;
    char replies[4096];
    uint64_t reply_at[16];
    unsigned reply_count;
} f;
static int fake_devices(void *context) {
    (void)context;
    if (f.now < f.plug_at || (f.unplug_at && f.now >= f.unplug_at && (!f.replug_at || f.now < f.replug_at))) return 0;
    return f.devices_when_plugged;
}
static int fake_path_ok(void *context, const char *path) {
    (void)context; CHECK(!strcmp(path, UPD_CHILD_PATH)); return !f.path_bad;
}
static void advance(uint64_t ms) {
    f.now += ms;
    if (f.stop_at && f.now >= f.stop_at) f.stop = 1;
    if (!f.running || f.ended) return;
    const struct script *s = f.current;
    uint64_t age = f.now - f.started;
    if (age >= s->lifetime) { f.ended = 1; f.status = s->exit_code << 8; }
    if (f.term_at && !s->ignores_term && f.now - f.term_at >= s->term_exit_after) { f.ended = 1; f.status = 0; }
}
static int fake_spawn(void *context, const char *path, const char *mode, pid_t *pid, int *fd) {
    (void)context; CHECK(!strcmp(path, UPD_CHILD_PATH));
    CHECK(f.spawned < f.script_count);
    if (f.spawned >= f.script_count) { f.stop = 1; return -1; }
    const struct script *s = &f.scripts[f.spawned++];
    CHECK(!strcmp(mode, s->mode));
    if (s->spawn_fails) return -1;
    CHECK(!f.running);
    if (f.spawned <= 8) f.spawn_at[f.spawned - 1] = f.now;
    f.current = s; f.running = 1; f.ended = 0; f.reaped_now = 0; f.status = 0; f.started = f.now;
    f.connected_printed = 0; f.output_offset = 0; f.eof_given = 0; f.term_at = 0;
    if (s->stop_with_end) f.stop_at = f.now + s->lifetime;
    *pid = 4321; *fd = 55;
    return 0;
}
static int fake_reap(void *context, pid_t pid, int *status) {
    (void)context; CHECK(pid == 4321 && f.running);
    if (f.current->reap_fails && f.ended) { f.reaped_now = 1; return -1; }
    if (!f.ended) return 0;
    *status = f.status; f.reaps++; f.reaped_now = 1; return 1;
}
static long fake_read(void *context, int fd, char *buffer, size_t size, unsigned wait_ms) {
    (void)context; CHECK(fd == 55 && f.running);
    const struct script *s = f.current;
    if (s->output && f.output_offset < strlen(s->output) && (!s->output_after_end || f.ended)) {
        /* Delivered in reader-sized pieces, as a pipe would. */
        size_t length = strlen(s->output) - f.output_offset;
        if (length > size) length = size;
        memcpy(buffer, s->output + f.output_offset, length); f.output_offset += length;
        return (long)length;
    }
    if (s->connect_after && !f.connected_printed && f.now - f.started >= s->connect_after) {
        f.connected_printed = 1;
        const char *line = "up=connected elapsed-ms=6000 primary-ipv4=this-service\n";
        memcpy(buffer, line, strlen(line)); return (long)strlen(line);
    }
    if (f.ended && !s->no_eof) { f.eof_given = 1; return 0; }
    advance(wait_ms);
    return -1;
}
static void fake_close(void *context, int fd) {
    (void)context; CHECK(fd == 55 && f.ended && (f.eof_given || f.current->no_eof)); f.closes++; f.running = 0;
}
static int fake_signal(void *context, pid_t pid, int number) {
    (void)context; CHECK(pid == 4321);
    CHECK(f.running && !f.reaped_now);
    CHECK(number == SIGHUP || number == SIGKILL);
    if (number == SIGHUP) { f.terms++; f.term_at = f.now; }
    if (number == SIGKILL) { f.kills++; f.ended = 1; f.status = SIGKILL; }
    return 0;
}
static uint64_t fake_now(void *context) { (void)context; return f.now; }
static void fake_sleep(void *context, unsigned ms) { (void)context; CHECK(ms <= UPD_SLICE_MS && ms); advance(ms); }
static int64_t fake_now_wall(void *context) { (void)context; return WALL_BASE + (int64_t)(f.now / 1000); }
/* Every written status: one line, fixed shape, only plain JSON characters. */
static int fake_write_status(void *context, const char *json, size_t length) {
    (void)context;
    if (f.status_writes && f.now - f.last_write_at > f.max_write_gap) f.max_write_gap = f.now - f.last_write_at;
    /* Same body apart from "updated": a heartbeat. */
    const char *stamp = strstr(json, "\"updated\":");
    if (f.status_writes && stamp && !strncmp(json, f.status_json, (size_t)(stamp - json))) f.heartbeat_writes++;
    f.status_writes++; f.last_write_at = f.now;
    int bad = !length || length >= sizeof(f.status_json) || strlen(json) != length || json[length - 1] != '\n' ||
        strncmp(json, "{\"version\":1,\"state\":\"", 22) || strstr(json, "4321") || strstr(json, "/");
    for (size_t i = 0; i + 1 < length; i++)
        if (!strchr("{}[]\":,.-_abcdefghijklmnopqrstuvwxyz0123456789", json[i])) bad = 1;
    if (bad) f.status_bad++;
    if (length < sizeof(f.status_json)) { memcpy(f.status_json, json, length); f.status_json[length] = 0; }
    return f.status_result;
}
static void fake_remove_status(void *context) { (void)context; f.status_removed++; }
static int fake_paused_read(void *context) { (void)context; return f.paused_file; }
static int fake_paused_write(void *context, int paused) {
    (void)context; f.pause_writes++;
    if (f.pause_write_fails) return -1;
    f.paused_file = paused; return 0;
}
static int fake_control_open(void *context, int *root_only) {
    (void)context; *root_only = f.root_only; return f.control_blocked ? -1 : 77;
}
static int fake_control_accept(void *context, int listener, int *client, int *peer) {
    (void)context; CHECK(listener == 77 && !f.client_open);
    if (f.command_next >= f.command_count || f.now < f.commands[f.command_next].at) return 0;
    *peer = f.commands[f.command_next].peer; *client = 88;
    f.client_open = 1; f.client_offset = 0;
    return 1;
}
/* Text arrives at once; a client that sends no newline just goes quiet. */
static long fake_client_read(void *context, int client, char *buffer, size_t size, unsigned wait_ms) {
    (void)context; CHECK(client == 88 && f.client_open && wait_ms <= UPD_CONTROL_WAIT_MS);
    const struct command *c = &f.commands[f.command_next];
    const char *text = c->text;
    size_t length = (c->length ? c->length : strlen(text)) - f.client_offset;
    if (!length) { advance(wait_ms ? wait_ms : 1); return -1; }
    if (length > size) length = size;
    memcpy(buffer, text + f.client_offset, length); f.client_offset += length;
    return (long)length;
}
static void fake_client_reply(void *context, int client, const char *text, size_t length) {
    (void)context; CHECK(client == 88 && f.client_open);
    if (f.reply_count < 16) f.reply_at[f.reply_count++] = f.now;
    size_t used = strlen(f.replies);
    if (used + length < sizeof(f.replies)) { memcpy(f.replies + used, text, length); f.replies[used + length] = 0; }
}
static void fake_client_close(void *context, int client) {
    (void)context; CHECK(client == 88 && f.client_open);
    f.client_open = 0; f.client_closes++; f.command_next++;
}
static void fake_control_close(void *context, int listener) { (void)context; CHECK(listener == 77); f.control_closed++; }
static const struct upd_backend fake = {NULL, fake_devices, fake_path_ok, fake_spawn, fake_reap, fake_read,
    fake_close, fake_signal, fake_now, fake_sleep, fake_now_wall, fake_write_status, fake_remove_status,
    fake_paused_read, fake_paused_write, fake_control_open, fake_control_accept, fake_client_read, fake_client_reply,
    fake_client_close, fake_control_close};

static char text[65536];
static void run(const struct script *scripts, unsigned count) {
    f.scripts = scripts; f.script_count = count;
    FILE *output = tmpfile();
    CHECK(upd_run(&fake, output, NULL, &f.stop) == 0);
    fseek(output, 0, SEEK_SET);
    memset(text, 0, sizeof(text));
    (void)fread(text, 1, sizeof(text) - 1, output); fclose(output);
    CHECK(f.spawned == count && !f.running && strstr(text, " upd=starting\n") && strstr(text, " upd=stopped\n"));
    CHECK(!strstr(text, "/Library") && !strstr(text, "4321"));
    /* Status and control end with the daemon: file removed, socket closed. */
    CHECK(!f.status_bad && !f.client_open && f.client_closes == f.command_next);
    CHECK(f.control_closed == (f.control_blocked ? 0 : 1));
    CHECK(f.status_removed == (f.status_result > 0 ? 0 : 1));
    CHECK(f.status_result > 0 || strstr(f.status_json, "{\"version\":1,\"state\":\"stopping\",") == f.status_json);
}
static unsigned occurrences(const char *needle) {
    unsigned n = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
static int starts(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }
static void reset(int number) {
    memset(&f, 0, sizeof(f)); scenario = number; f.now = 1000; f.devices_when_plugged = 1;
}
/* ---- Native backend on a real temporary tree (no root, no device) ---- */
static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (fd >= 0 && connect(fd, (const struct sockaddr *)&address, sizeof(address))) { close(fd); fd = -1; }
    return fd;
}
static int read_file(const char *path, char *buffer, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    size_t n = fread(buffer, 1, size - 1, file);
    buffer[n] = 0; fclose(file);
    return (int)n;
}
static void native_reset_run(const char *dir) {
    if (native_run_fd >= 0) close(native_run_fd);
    native_run_fd = -1; native_run_dir = dir;
}
/* Sends one request through the real socket and the daemon's control code. */
static void native_roundtrip(int listener, const char *path, const char *request, char *reply, size_t size) {
    static const struct upd_backend hybrid = {NULL, fake_devices, fake_path_ok, fake_spawn, fake_reap, fake_read,
        fake_close, fake_signal, native_now, native_sleep, fake_now_wall, fake_write_status, fake_remove_status,
        fake_paused_read, fake_paused_write, native_control_open, native_control_accept, native_read,
        native_client_reply, native_close, native_control_close};
    FILE *sink = tmpfile();
    struct upd u = {.b = &hybrid, .output = sink, .control = listener, .stop = &f.stop};
    u.s = (struct upd_status){.state = UPD_WAITING, .devices = 0, .session_started = -1, .connected_since = -1,
        .retry_at = -1, .updated = -1, .measured_at = -1, .end_at = -1};
    reply[0] = 0;
    int client = connect_to(path);
    CHECK(client >= 0);
    if (client < 0) { fclose(sink); return; }
    CHECK(write(client, request, strlen(request)) == (ssize_t)strlen(request));
    upd_control(&u);
    size_t used = 0;
    struct pollfd p = {.fd = client, .events = POLLIN};
    while (used + 1 < size && poll(&p, 1, 1000) > 0) {
        ssize_t n = read(client, reply + used, size - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    reply[used] = 0;
    close(client);
    fseek(sink, 0, SEEK_SET);
    memset(text, 0, sizeof(text));
    (void)fread(text, 1, sizeof(text) - 1, sink); fclose(sink);
}
static void native_tests(void) {
    char root[] = "build/upd-native-XXXXXX", path[256], other[256], buffer[1024];
    CHECK(mkdtemp(root) != NULL);
    native_owner = getuid(); native_group = getgid();
    struct group *own = getgrgid(getgid());
    static char group_name[64];
    CHECK(own != NULL);
    (void)snprintf(group_name, sizeof(group_name), "%s", own ? own->gr_name : "");
    native_admin_name = group_name;
    int root_only = -1;
    struct stat st;
    /* A symlinked or group-writable run directory is refused. */
    (void)snprintf(path, sizeof(path), "%s/real", root); CHECK(!mkdir(path, 0755));
    (void)snprintf(other, sizeof(other), "%s/link", root); CHECK(!symlink("real", other));
    native_reset_run(other);
    CHECK(native_write_status(NULL, "{}\n", 3) == 1 && native_control_open(NULL, &root_only) == -1);
    (void)snprintf(path, sizeof(path), "%s/group-writable", root);
    CHECK(!mkdir(path, 0755) && !chmod(path, 0775));
    native_reset_run(path);
    CHECK(native_write_status(NULL, "{}\n", 3) == 1);
    /* A missing one is made 0755; the file is written 0644 through tmp + rename. */
    char run[256];
    (void)snprintf(run, sizeof(run), "%s/run", root);
    native_reset_run(run);
    CHECK(native_write_status(NULL, "{\"a\":1}\n", 8) == 0);
    CHECK(!lstat(run, &st) && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0755 && st.st_uid == getuid());
    (void)snprintf(path, sizeof(path), "%s/status.json", run);
    CHECK(read_file(path, buffer, sizeof(buffer)) == 8 && !strcmp(buffer, "{\"a\":1}\n"));
    CHECK(!lstat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0644);
    /* A planted symlink at the tmp name is removed, never written through. */
    (void)snprintf(other, sizeof(other), "%s/target", root);
    FILE *target = fopen(other, "w"); CHECK(target != NULL);
    if (target) { fputs("keep", target); fclose(target); }
    (void)snprintf(path, sizeof(path), "%s/status.json.tmp", run); CHECK(!symlink("../target", path));
    CHECK(native_write_status(NULL, "{\"b\":2}\n", 8) == 0);
    CHECK(read_file(other, buffer, sizeof(buffer)) == 4 && !strcmp(buffer, "keep"));
    CHECK(lstat(path, &st) && errno == ENOENT);
    (void)snprintf(path, sizeof(path), "%s/status.json", run);
    CHECK(read_file(path, buffer, sizeof(buffer)) == 8 && !strcmp(buffer, "{\"b\":2}\n"));
    /* Anything but an owned socket at the socket name is left alone. */
    char sock[256];
    (void)snprintf(sock, sizeof(sock), "%s/control.sock", run);
    FILE *plain = fopen(sock, "w"); CHECK(plain != NULL);
    if (plain) fclose(plain);
    CHECK(native_control_open(NULL, &root_only) == -1 && !lstat(sock, &st) && S_ISREG(st.st_mode));
    CHECK(!unlink(sock));
    /* A stale socket is replaced; the new one is owner:admin 0660. */
    int stale = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", sock);
    CHECK(stale >= 0 && !bind(stale, (const struct sockaddr *)&address, sizeof(address)));
    close(stale);
    int listener = native_control_open(NULL, &root_only);
    CHECK(listener >= 0 && root_only == 0);
    CHECK(!lstat(sock, &st) && S_ISSOCK(st.st_mode) && (st.st_mode & 07777) == 0660 && st.st_gid == getgid());
    /* Same-uid peer: not root, so the admin-membership branch decides. */
    char reply[1024];
    if (listener >= 0) {
        native_roundtrip(listener, sock, "status\n", reply, sizeof(reply));
        CHECK(starts(reply, "{\"version\":1,\"state\":\"waiting\",") && strstr(text, " uid-class=admin\n"));
        native_roundtrip(listener, sock, "pause\n", reply, sizeof(reply));
        CHECK(!strcmp(reply, "ok\n") && f.paused_file == 1);
        native_control_close(NULL, listener);
    }
    CHECK(lstat(sock, &st) && errno == ENOENT);
    /* No admin group: root only, 0600, and this (non-root) peer is denied. */
    native_admin_name = "dji-dongle-no-such-group";
    listener = native_control_open(NULL, &root_only);
    CHECK(listener >= 0 && root_only == 1 && !lstat(sock, &st) && (st.st_mode & 07777) == 0600);
    if (listener >= 0) {
        native_roundtrip(listener, sock, "pause\n", reply, sizeof(reply));
        CHECK(!strcmp(reply, "error=denied\n") && strstr(text, " upd=control-denied\n"));
        native_control_close(NULL, listener);
    }
    native_remove_status(NULL);
    CHECK(lstat(path, &st) && errno == ENOENT);
    /* Saved pause: missing dir = not paused; made, read, removed. */
    char pause[256];
    (void)snprintf(pause, sizeof(pause), "%s/pause", root);
    native_pause_dir = pause;
    CHECK(native_paused_read(NULL) == 0 && native_paused_write(NULL, 0) == 0);
    CHECK(native_paused_write(NULL, 1) == 0 && native_paused_read(NULL) == 1);
    (void)snprintf(path, sizeof(path), "%s/paused", pause);
    CHECK(!lstat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0644);
    CHECK(native_paused_write(NULL, 0) == 0 && native_paused_read(NULL) == 0 && native_paused_write(NULL, 0) == 0);
    /* A symlink as the pause file or the pause directory cannot be verified. */
    CHECK(!symlink("../target", path) && native_paused_read(NULL) == -1);
    CHECK(!unlink(path) && !rmdir(pause) && !symlink("real", pause));
    CHECK(native_paused_read(NULL) == -1 && native_paused_write(NULL, 1) == -1 && native_paused_write(NULL, 0) == -1);
    /* Clean up and restore the production settings. */
    const char *const files[] = {"pause", "target", "link", "run", "real", "group-writable"};
    (void)snprintf(path, sizeof(path), "%s/run/status.json", root); (void)unlink(path);
    for (unsigned i = 0; i < 3; i++) { (void)snprintf(path, sizeof(path), "%s/%s", root, files[i]); CHECK(!unlink(path)); }
    for (unsigned i = 3; i < 6; i++) { (void)snprintf(path, sizeof(path), "%s/%s", root, files[i]); CHECK(!rmdir(path)); }
    CHECK(!rmdir(root));
    native_reset_run(UPD_RUN_DIR);
    native_pause_dir = UPD_PAUSE_DIR; native_admin_name = "admin"; native_owner = 0; native_group = 0;
}

int main(void) {
    /* 1: absent, then plugged. Quick failures double the backoff; a session
     * connected for 60 s or more resets it; the stop request ends a wait. */
    reset(1); f.plug_at = 4000; f.stop_at = 185000; /* inside the 20 s wait after the fifth session */
    static const struct script s1[] = {
        START_CLEAN,
        {"--up", "up=starting sc-pre-removed=0\nup-result=ended reason=primary-not-taken\n", 0, 2000, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 0, 2000, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up=starting sc-pre-removed=0\n", 6000, 130000, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 0, 2000, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 0, 2000, 3, 0, 0, 0, 0, 0, 0, 0},
    };
    run(s1, 6);
    CHECK(strstr(text, " upd=waiting devices=0\n") && occurrences("upd=waiting devices=0") == 1);
    CHECK(strstr(text, " child cleanup=clean\n") && strstr(text, "upd=session-ended mode=cleanup exit=0\n"));
    CHECK(strstr(text, " child up=connected elapsed-ms=6000 primary-ipv4=this-service\n") != NULL);
    {
        const unsigned want[] = {5000, 10000, 5000, 10000, 20000};
        const char *q = text;
        for (unsigned i = 0; i < 5; i++) {
            q = strstr(q, "upd=retry-in-ms=");
            CHECK(q && strtoul(q + 16, NULL, 10) == want[i]);
            if (!q) break;
            q += 16;
        }
        CHECK(occurrences("upd=retry-in-ms=") == 5);
    }
    CHECK(occurrences("upd=session-ended mode=up exit=3") == 5 && !f.terms && !f.kills);
    /* Fast ends that are not session errors never ask for a replug. */
    CHECK(!strstr(text, "upd=replug-required"));

    /* 2: leftover keys (exit 4) trigger one cleanup run, then the dongle is
     * unplugged: no retry, back to waiting. */
    reset(2); f.plug_at = 0; f.unplug_at = 4050; f.stop_at = 20000;
    static const struct script s2[] = {
        START_CLEAN,
        {"--up", "up-result=cleanup-required\n", 0, 3000, 4, 0, 0, 0, 0, 0, 0, 0},
        {"--cleanup", "cleanup=clean sc=done\n", 0, 100, 0, 0, 0, 0, 0, 0, 0, 0},
    };
    run(s2, 3);
    CHECK(strstr(text, "upd=session-ended mode=up exit=4\n") && strstr(text, " child cleanup=clean sc=done\n") &&
          !strstr(text, "upd=retry-in-ms") && strstr(text, " upd=waiting devices=0\n"));
    /* 3: unplug during the session: the child ends (usb-error), no backoff. */
    reset(3); f.plug_at = 0; f.unplug_at = 5000; f.stop_at = 15000;
    static const struct script s3[] = {START_CLEAN, {"--up", "up-result=ended reason=usb-error\n", 0, 4500, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s3, 2);
    CHECK(!strstr(text, "upd=retry-in-ms") && strstr(text, " upd=waiting devices=0\n"));
    /* 4: launchd stop while connected: SIGHUP once, the child stops cleanly. */
    reset(4); f.plug_at = 0; f.stop_at = 20000;
    static const struct script s4[] = {START_CLEAN, {"--up", NULL, 6000, 1000000, 3, 0, 0, 3000, 0, 0, 0, 0}};
    run(s4, 2);
    CHECK(f.terms == 1 && !f.kills && strstr(text, " upd=stopping-session\n") &&
          strstr(text, "upd=session-ended mode=up exit=0\n") && occurrences("mode=cleanup") == 1);
    /* 5: a child that ignores the stop is killed after 60 s; while stopping,
     * the cleanup that follows runs once (launchd's exit budget), never signalled. */
    reset(5); f.plug_at = 0; f.stop_at = 20000;
    static const struct script s5[] = {
        START_CLEAN,
        {"--up", NULL, 6000, 1000000, 3, 1, 0, 0, 0, 0, 0, 0},
        {"--cleanup", "cleanup=unproven\n", 0, 100, 4, 0, 0, 0, 0, 0, 0, 0},
    };
    run(s5, 3);
    CHECK(f.terms == 1 && f.kills == 1 && strstr(text, " upd=child-killed mode=up\n") &&
          strstr(text, "upd=session-ended mode=up signal=9\n") && occurrences("upd=session-ended mode=cleanup exit=4") == 1);
    CHECK(f.now >= 20000 + UPD_STOP_WAIT_MS);
    /* 6: unsafe child path: never spawned, not even the start cleanup. */
    reset(6); f.plug_at = 0; f.path_bad = 1; f.stop_at = 130000;
    run(NULL, 0);
    CHECK(occurrences("upd=blocked gate=child-path") >= 2);
    /* 7: two dongles, then an unreadable count: wait, never spawn a session. */
    reset(7); f.plug_at = 0; f.devices_when_plugged = 2; f.stop_at = 10000;
    static const struct script s7[] = {START_CLEAN};
    run(s7, 1);
    CHECK(occurrences("upd=waiting devices=2") == 1);
    reset(7); f.plug_at = 0; f.devices_when_plugged = -1; f.stop_at = 10000;
    run(s7, 1);
    CHECK(occurrences("upd=waiting devices=unknown") == 1);
    /* 8: spawn failure: logged, then the normal backoff. */
    reset(8); f.plug_at = 0; f.stop_at = 9000;
    static const struct script s8[] = {START_CLEAN, {"--up", NULL, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0},
        {"--up", NULL, 0, 1000, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s8, 3);
    CHECK(strstr(text, " upd=spawn-error mode=up\n") && strstr(text, "upd=retry-in-ms=5000\n"));
    /* 9: a line longer than the buffer is split; a last line without newline is kept. */
    reset(9); f.plug_at = 0; f.stop_at = 3000;
    static char long_output[700];
    memset(long_output, 'x', 600); memcpy(long_output + 600, "\ntail-without-newline", 21);
    struct script s9[] = {START_CLEAN, {"--up", long_output, 0, 500, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s9, 2);
    CHECK(strstr(text, " child tail-without-newline\n") && occurrences(" child x") == 2);
    /* 10: the stop lands right as the child exits, before its output EOF:
     * the reaped pid is never signalled. */
    reset(10); f.plug_at = 0;
    static const struct script s10[] = {START_CLEAN,
        {"--up", "up-result=ended reason=pppd-exit\n", 0, 3000, 3, 0, 0, 0, 1, 0, 1, 0}};
    run(s10, 2);
    CHECK(!f.terms && !f.kills && strstr(text, " child up-result=ended reason=pppd-exit\n"));
    /* 11: output still open after the exit: abandoned after 5 s. */
    reset(11); f.plug_at = 0; f.stop_at = 10000;
    static const struct script s11[] = {START_CLEAN, {"--up", NULL, 0, 1000, 3, 0, 0, 0, 0, 1, 0, 0}};
    run(s11, 2);
    CHECK(strstr(text, " upd=output-abandoned mode=up\n") && strstr(text, "upd=session-ended mode=up exit=3\n"));
    /* 12: a cleanup that never ends is killed after 30 s and not retried
     * beyond the bound. */
    reset(12); f.plug_at = 0; f.devices_when_plugged = 0;
    static const struct script s12[] = {{"--cleanup", NULL, 0, 10000000, 0, 1, 0, 0, 0, 0, 0, 0}};
    f.stop_at = 5000;
    run(s12, 1);
    CHECK(f.kills == 1 && !f.terms && strstr(text, " upd=child-killed mode=cleanup\n") &&
          strstr(text, "upd=session-ended mode=cleanup signal=9\n") && f.now >= 1000 + UPD_CLEANUP_LIMIT_MS);
    /* 13: a reap error is treated as a lost child: cleanup runs. */
    reset(13); f.plug_at = 0; f.stop_at = 4000;
    static const struct script s13[] = {START_CLEAN, {"--up", NULL, 0, 1000, 3, 0, 0, 0, 0, 0, 0, 1},
        {"--cleanup", "cleanup=clean\n", 0, 100, 0, 0, 0, 0, 0, 0, 0, 0}};
    run(s13, 3);
    CHECK(strstr(text, " upd=reap-error\n") && occurrences("upd=session-ended mode=cleanup exit=0") == 2);
    /* 14: not stopping, leftovers reported three times: three tries, then backoff. */
    reset(14); f.plug_at = 0; f.stop_at = 10000; /* inside the 5 s backoff after the third cleanup */
    static const struct script s14[] = {START_CLEAN, {"--up", NULL, 0, 1000, 4, 0, 0, 0, 0, 0, 0, 0},
        {"--cleanup", NULL, 0, 100, 4, 0, 0, 0, 0, 0, 0, 0}, {"--cleanup", NULL, 0, 100, 4, 0, 0, 0, 0, 0, 0, 0},
        {"--cleanup", NULL, 0, 100, 4, 0, 0, 0, 0, 0, 0, 0}};
    run(s14, 5);
    CHECK(occurrences("upd=session-ended mode=cleanup exit=4") == 3 && strstr(text, "upd=retry-in-ms=5000\n"));
    /* 15: three fast failures without a connection: stop retrying until the
     * dongle is unplugged and plugged again, then start normally. */
    reset(15); f.plug_at = 0; f.unplug_at = 40000; f.replug_at = 45000; f.stop_at = 55000;
    static const struct script s15[] = {START_CLEAN,
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 2000, 100000, 3, 0, 0, 3000, 0, 0, 0, 0}};
    run(s15, 5);
    CHECK(occurrences("upd=replug-required") == 1 && occurrences("upd=retry-in-ms=") == 2 &&
          strstr(text, " upd=waiting devices=0\n") && strstr(text, " child up=connected"));
    /* 16: fast ends that were blocked (another run holds the lock) never count. */
    reset(16); f.plug_at = 0; f.stop_at = 20000;
    static const struct script s16[] = {START_CLEAN,
        {"--up", "up=blocked gate=lock-held sc-pre=not-run\n", 0, 100, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up=blocked gate=lock-held sc-pre=not-run\n", 0, 100, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up=blocked gate=lock-held sc-pre=not-run\n", 0, 100, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s16, 4);
    CHECK(!strstr(text, "upd=replug-required") && occurrences("upd=retry-in-ms=") == 3);
    /* 17: two session errors, a connection, two more: the count restarts, no replug. */
    reset(17); f.plug_at = 0; f.stop_at = 90000; /* inside the wait after the fifth session */
    static const struct script s17[] = {START_CLEAN,
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 1000, 2000, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s17, 6);
    CHECK(!strstr(text, "upd=replug-required"));
    /* 18: while waiting for a replug, an unreadable count keeps waiting and a
     * stop ends the daemon. */
    reset(18); f.plug_at = 0; f.stop_at = 40000;
    static const struct script s18[] = {START_CLEAN,
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s18, 4);
    CHECK(occurrences("upd=replug-required") == 1);

    /* 19: the status follows the session: signal numbers, connection time,
     * then stopping on a launchd stop and removal at exit. */
    reset(19); f.plug_at = 0; f.stop_at = 20000;
    static const struct command c19[] = {{15000, "status\n", UPD_PEER_ROOT, 0}};
    f.commands = c19; f.command_count = 1;
    static const struct script s19[] = {START_CLEAN, {"--up", "up=starting sc-pre-removed=0\nup-signal=ok csq-rssi=20 "
        "rssi-dbm=-73 cesq-rsrp=42 rsrp-dbm=-99 cesq-rsrq=20 rsrq-db=-10.0 probe-status=0 stop=none\n",
        6000, 1000000, 3, 0, 0, 3000, 0, 0, 0, 0}};
    run(s19, 2);
    CHECK(!strcmp(f.replies, "{\"version\":1,\"state\":\"connected\",\"devices\":1,\"paused\":false,"
        "\"session_started\":1700000001,\"connected_since\":1700000007,\"retry_at\":null,\"signal\":{\"quality\":\"ok\","
        "\"rssi_dbm\":-73,\"rsrp_dbm\":-99,\"rsrq_db\":-10.0,\"measured_at\":1700000001},\"last_end\":{\"result\":\"none\","
        "\"reason\":null,\"at\":null},\"updated\":1700000007}\n"));
    CHECK(strstr(text, " upd=control command=status uid-class=root\n") && f.terms == 1);
    /* Signal stays after the session; the end without a result line is named by the daemon. */
    CHECK(strstr(f.status_json, "\"session_started\":null,\"connected_since\":null,") &&
          strstr(f.status_json, "\"rssi_dbm\":-73,") &&
          strstr(f.status_json, "\"last_end\":{\"result\":\"ended\",\"reason\":\"no-result\","));
    /* Written on changes only, never once per slice. */
    CHECK(f.status_writes >= 4 && f.status_writes <= 12);

    /* 20: pause ends the session like a stop (SIGHUP) and holds new sessions
     * across slices; resume starts at once. */
    reset(20); f.plug_at = 0; f.stop_at = 45000;
    static const struct command c20[] = {{10000, "pause\n", UPD_PEER_ADMIN, 0}, {20000, "pause\n", UPD_PEER_ADMIN, 0},
        {25000, "status\n", UPD_PEER_ADMIN, 0}, {30000, "resume\n", UPD_PEER_ROOT, 0}, {31000, "resume\n", UPD_PEER_ROOT, 0}};
    f.commands = c20; f.command_count = 5;
    static const struct script s20[] = {START_CLEAN, {"--up", NULL, 2000, 1000000, 3, 0, 0, 1000, 0, 0, 0, 0},
        {"--up", NULL, 2000, 1000000, 3, 0, 0, 1000, 0, 0, 0, 0}};
    run(s20, 3);
    CHECK(f.terms == 2 && !f.kills && strstr(text, " upd=ending-session for=pause\n") &&
          strstr(text, " upd=control command=pause uid-class=admin\n") && occurrences(" upd=stopping-session\n") == 1);
    CHECK(starts(f.replies, "ok ending-session\nok already-paused\n{\"version\":1,\"state\":\"paused\",\"devices\":1,\"paused\":true,"));
    CHECK(strstr(f.replies, "}\nok\nok already-running\n") != NULL);
    CHECK(f.spawn_at[2] >= 30000 && f.spawn_at[2] < 30500 && !strstr(text, "upd=retry-in-ms"));
    CHECK(f.pause_writes == 2 && f.paused_file == 0 && !strstr(text, "upd=reconnecting"));

    /* 21: reconnect during a session: ended with SIGHUP, the next one starts
     * right away without a backoff. */
    reset(21); f.plug_at = 0; f.stop_at = 20000;
    static const struct command c21[] = {{10000, "reconnect\n", UPD_PEER_ROOT, 0}};
    f.commands = c21; f.command_count = 1;
    static const struct script s21[] = {START_CLEAN, {"--up", NULL, 2000, 1000000, 3, 0, 0, 1000, 0, 0, 0, 0},
        {"--up", NULL, 2000, 1000000, 3, 0, 0, 1000, 0, 0, 0, 0}};
    run(s21, 3);
    CHECK(f.terms == 2 && strstr(text, " upd=ending-session for=reconnect\n") && strstr(text, " upd=reconnecting\n"));
    CHECK(!strcmp(f.replies, "ok\n") && f.spawn_at[2] < 11500 && !strstr(text, "upd=retry-in-ms"));

    /* 22: reconnect during the retry wait cuts it and keeps the backoff. */
    reset(22); f.plug_at = 0; f.stop_at = 8000;
    static const struct command c22[] = {{4000, "reconnect\n", UPD_PEER_ADMIN, 0}};
    f.commands = c22; f.command_count = 1;
    static const struct script s22[] = {START_CLEAN, {"--up", NULL, 0, 1000, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 0, 1000, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s22, 3);
    CHECK(!strcmp(f.replies, "ok\n") && f.spawn_at[2] >= 4000 && f.spawn_at[2] < 4500);
    CHECK(occurrences("upd=retry-in-ms=5000") == 2 && !strstr(text, "upd=retry-in-ms=10000"));

    /* 23: refused requests change nothing: a peer that is neither root nor
     * admin, unknown or partial lines, two commands at once, an overlong line,
     * reconnect without a dongle. */
    reset(23); f.plug_at = 0; f.devices_when_plugged = 0; f.stop_at = 7000;
    static char overlong[80];
    memset(overlong, 'p', 70); overlong[70] = '\n';
    const struct command c23[] = {{2000, "pause\n", UPD_PEER_DENIED, 0}, {2500, "bogus\n", UPD_PEER_ADMIN, 0},
        {3000, "pause\nresume\n", UPD_PEER_ADMIN, 0}, {3500, "paus", UPD_PEER_ADMIN, 0}, {4000, overlong, UPD_PEER_ADMIN, 0},
        {4500, "reconnect\n", UPD_PEER_ADMIN, 0}, {5000, "PAUSE\n", UPD_PEER_ROOT, 0}, {5500, "", UPD_PEER_ROOT, 0},
        {6000, "resume\n", UPD_PEER_ADMIN, 0}};
    f.commands = c23; f.command_count = 9;
    static const struct script s23[] = {START_CLEAN};
    run(s23, 1);
    CHECK(!strcmp(f.replies, "error=denied\nerror=unknown-command\nerror=bad-request\nerror=bad-request\n"
        "error=bad-request\nerror=no-device\nerror=unknown-command\nerror=bad-request\nok already-running\n"));
    CHECK(occurrences(" upd=control-denied\n") == 1 && occurrences("upd=control command=invalid uid-class=") == 6);
    CHECK(!f.pause_writes && !f.paused_file && f.command_next == 9);

    /* 24: a pause saved by an earlier run holds sessions from the start; the
     * start cleanup still runs. */
    reset(24); f.plug_at = 0; f.paused_file = 1; f.stop_at = 5000;
    static const struct command c24[] = {{3000, "status\n", UPD_PEER_ROOT, 0}};
    f.commands = c24; f.command_count = 1;
    static const struct script s24[] = {START_CLEAN};
    run(s24, 1);
    CHECK(strstr(text, " upd=paused source=file\n") && !strstr(text, "upd=session-start") && !strstr(text, "upd=waiting"));
    CHECK(starts(f.replies, "{\"version\":1,\"state\":\"paused\",\"devices\":1,\"paused\":true,"));

    /* 25: an unsafe status directory and control path: logged once, the
     * daemon runs as before without them. */
    reset(25); f.plug_at = 0; f.status_result = 1; f.control_blocked = 1; f.stop_at = 20000;
    static const struct script s25[] = {START_CLEAN, {"--up", NULL, 6000, 1000000, 3, 0, 0, 3000, 0, 0, 0, 0}};
    run(s25, 2);
    CHECK(occurrences("upd=status-blocked") == 1 && occurrences("upd=control-blocked") == 1 && f.status_writes == 1);
    CHECK(f.terms == 1 && strstr(text, " upd=stopping-session\n"));

    /* 26: a failed status write is logged once and retried; a pause that cannot
     * be saved still pauses. */
    reset(26); f.plug_at = 0; f.devices_when_plugged = 0; f.status_result = -1; f.pause_write_fails = 1; f.stop_at = 4000;
    static const struct command c26[] = {{2000, "pause\n", UPD_PEER_ROOT, 0}};
    f.commands = c26; f.command_count = 1;
    static const struct script s26[] = {START_CLEAN};
    run(s26, 1);
    CHECK(occurrences("upd=status-error") == 1 && f.status_writes >= 3 && f.status_writes <= 7 && !strcmp(f.replies, "ok not-saved\n"));
    CHECK(strstr(text, " upd=pause-save-error\n") && strstr(f.status_json, "\"paused\":true,"));

    /* 27: pause and resume while a replug is required: the replug stays. */
    reset(27); f.plug_at = 0; f.stop_at = 30000;
    static const struct command c27[] = {{20000, "pause\n", UPD_PEER_ADMIN, 0}, {22000, "resume\n", UPD_PEER_ADMIN, 0},
        {23000, "reconnect\n", UPD_PEER_ADMIN, 0}, {25000, "status\n", UPD_PEER_ADMIN, 0}};
    f.commands = c27; f.command_count = 4;
    static const struct script s27[] = {START_CLEAN,
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 0, 0, 0}};
    run(s27, 4);
    CHECK(occurrences("upd=replug-required") == 1 &&
          starts(f.replies, "ok\nok\nerror=replug-required\n{\"version\":1,\"state\":\"replug-required\",\"devices\":1,"
                 "\"paused\":false,"));
    CHECK(strstr(f.replies, "\"last_end\":{\"result\":\"ended\",\"reason\":\"session-error\",") != NULL);

    /* 28: stop wins: a session ending for a pause is killed on the usual bound,
     * the stop is not signalled twice, and commands after the stop are refused. */
    reset(28); f.plug_at = 0; f.stop_at = 12000;
    static const struct command c28[] = {{10000, "pause\n", UPD_PEER_ADMIN, 0}, {13000, "resume\n", UPD_PEER_ADMIN, 0},
        {14000, "status\n", UPD_PEER_ADMIN, 0}};
    f.commands = c28; f.command_count = 3;
    static const struct script s28[] = {START_CLEAN, {"--up", NULL, 2000, 1000000, 3, 1, 0, 0, 0, 0, 0, 0},
        {"--cleanup", "cleanup=clean\n", 0, 100, 0, 0, 0, 0, 0, 0, 0, 0}};
    run(s28, 3);
    CHECK(f.terms == 1 && f.kills == 1 && strstr(text, " upd=ending-session for=pause\n") &&
          !strstr(text, "upd=stopping-session") && f.now >= 10000 + UPD_STOP_WAIT_MS);
    CHECK(starts(f.replies, "ok ending-session\nerror=stopping\n{\"version\":1,\"state\":\"stopping\",") && f.paused_file == 1);

    /* 29: strict child-line parsing straight into the status. */
    reset(29); f.now = 0;
    {
        FILE *sink = tmpfile();
        struct upd u = {.b = &fake, .output = sink, .control = -1};
        u.s = (struct upd_status){.state = UPD_CONNECTING, .devices = 1, .session_started = -1, .connected_since = -1,
            .retry_at = -1, .updated = -1, .measured_at = -1, .end_at = -1};
        struct upd_child k = {.session = 1};
        upd_line(&u, &k, "up-signal=partial csq-rssi=20 rssi-dbm=-73 cesq-rsrp=unproven rsrp-dbm=unproven "
                 "cesq-rsrq=unproven rsrq-db=unproven probe-status=0 stop=none");
        CHECK(u.s.quality == UPD_QUALITY_PARTIAL && u.s.has_rssi && u.s.rssi == -73 && !u.s.has_rsrp && !u.s.has_rsrq);
        upd_line(&u, &k, "up-signal=ok rssi-dbm=-1234 rsrp-dbm=-99x rsrq-db=-10.55");
        CHECK(u.s.quality == UPD_QUALITY_OK && !u.s.has_rssi && !u.s.has_rsrp && !u.s.has_rsrq);
        upd_line(&u, &k, "up-signal=ok rssi-dbm=-113 rsrp-dbm=-140 rsrq-db=-19.5 xrsrp-dbm=1");
        CHECK(u.s.rssi == -113 && u.s.rsrp == -140 && u.s.has_rsrq && u.s.rsrq_tenths == -195);
        upd_line(&u, &k, "up-signal=great rssi-dbm=-50");
        upd_line(&u, &k, "up-signal= rssi-dbm=-50");
        upd_line(&u, &k, "xup-signal=ok rssi-dbm=-50");
        upd_line(&u, &k, "up-signal=ok rssi-dbm=- rsrp-dbm=.5 rsrq-db=-.5");
        CHECK(u.s.quality == UPD_QUALITY_OK && !u.s.has_rssi && !u.s.has_rsrp && !u.s.has_rsrq);
        upd_line(&u, &k, "up-signal=unproven rssi-dbm=unknown rsrp-dbm=-0 rsrq-db=0.0");
        CHECK(u.s.quality == UPD_QUALITY_UNPROVEN && !u.s.has_rssi && u.s.rsrp == 0 && u.s.rsrq_tenths == 0);
        CHECK(u.s.end_result == UPD_END_NONE && !k.result_seen);
        upd_line(&u, &k, "up-result=ended reason=Usb\"x connected=no");
        CHECK(u.s.end_result == UPD_END_ENDED && !u.s.end_reason[0] && k.result_seen);
        upd_line(&u, &k, "up-result=stopped reason=user-stop connected=yes");
        CHECK(u.s.end_result == UPD_END_STOPPED && !strcmp(u.s.end_reason, "user-stop"));
        upd_line(&u, &k, "up-result=bogus reason=usb-error");
        CHECK(u.s.end_result == UPD_END_STOPPED && !strcmp(u.s.end_reason, "user-stop"));
        upd_line(&u, &k, "up-result=ended reason=aaaaaaaaaabbbbbbbbbbccccccccccdddddddddde");
        CHECK(u.s.end_result == UPD_END_ENDED && !u.s.end_reason[0]);
        upd_line(&u, &k, "up-result=ended reason=aaaaaaaaaabbbbbbbbbbccccccccccdddddddddd");
        CHECK(!strcmp(u.s.end_reason, "aaaaaaaaaabbbbbbbbbbccccccccccdddddddddd"));
        upd_line(&u, &k, "up=blocked gate=lock-held sc-pre=not-run next-run-cleanup=not-required");
        CHECK(u.s.end_result == UPD_END_BLOCKED && !strcmp(u.s.end_reason, "lock-held") && k.blocked);
        upd_line(&u, &k, "up-result=cleanup-required");
        CHECK(u.s.end_result == UPD_END_CLEANUP && !u.s.end_reason[0]);
        upd_line(&u, &k, "up=connected elapsed-ms=6000 primary-ipv4=this-service");
        CHECK(u.s.state == UPD_CONNECTED && u.s.connected_since == WALL_BASE && k.connected);
        /* A cleanup child's lines never reach the status. */
        struct upd_child cleanup = {.session = 0};
        upd_line(&u, &cleanup, "up-result=stopped reason=user-stop");
        CHECK(u.s.end_result == UPD_END_CLEANUP);
        char json[UPD_STATUS_JSON];
        u.s.rsrq_tenths = -5; u.s.has_rssi = 0; u.s.devices = 150; u.paused = 1; u.s.retry_at = WALL_BASE + 9;
        CHECK(upd_render(&u.s, u.paused, WALL_BASE + 1, json, sizeof(json)) && !strcmp(json,
            "{\"version\":1,\"state\":\"connected\",\"devices\":99,\"paused\":true,\"session_started\":null,"
            "\"connected_since\":1700000000,\"retry_at\":1700000009,\"signal\":{\"quality\":\"unproven\",\"rssi_dbm\":null,"
            "\"rsrp_dbm\":0,\"rsrq_db\":-0.5,\"measured_at\":1700000000},\"last_end\":{\"result\":\"cleanup-required\","
            "\"reason\":null,\"at\":1700000000},\"updated\":1700000001}\n"));
        CHECK(!upd_render(&u.s, u.paused, 1, json, 40));
        fclose(sink);
    }

    /* 30: a pause or reconnect that arrives after the child already ended on
     * its own (output still open) is no SIGHUP and no excuse: fast failures
     * still count, the backoff still grows, only the wait is cut. */
    reset(30); f.plug_at = 0; f.stop_at = 60000;
    static const struct command c30[] = {{3000, "reconnect\n", UPD_PEER_ROOT, 0}};
    f.commands = c30; f.command_count = 1;
    static const struct script s30[] = {START_CLEAN,
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 1, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 1, 0, 0},
        {"--up", "up-result=ended reason=session-error connected=no\n", 0, 500, 3, 0, 0, 0, 0, 1, 0, 0}};
    run(s30, 4);
    CHECK(!strcmp(f.replies, "ok\n") && !f.terms && !strstr(text, "upd=reconnecting") &&
          !strstr(text, "upd=ending-session") && occurrences("upd=replug-required") == 1);
    CHECK(strstr(text, "upd=retry-in-ms=5000\n") && strstr(text, "upd=retry-in-ms=10000\n"));
    CHECK(f.spawn_at[2] < f.spawn_at[1] + UPD_EOF_WAIT_MS + 1000);

    /* 31: reconnect during the start cleanup with a dongle present (count not
     * read yet) is accepted, not "no device". */
    reset(31); f.plug_at = 0; f.stop_at = 4000;
    static const struct command c31[] = {{1400, "reconnect\n", UPD_PEER_ROOT, 0}};
    f.commands = c31; f.command_count = 1;
    static const struct script s31[] = {{"--cleanup", "cleanup=clean\n", 0, 1000, 0, 0, 0, 0, 0, 0, 0, 0},
        {"--up", NULL, 0, 100000, 3, 0, 0, 500, 0, 0, 0, 0}};
    run(s31, 2);
    CHECK(!strcmp(f.replies, "ok\n") && f.terms == 1);

    /* 32: a resume that cannot be saved still resumes; root-only control is logged. */
    reset(32); f.plug_at = 0; f.devices_when_plugged = 0; f.paused_file = 1; f.pause_write_fails = 1;
    f.root_only = 1; f.stop_at = 4000;
    static const struct command c32[] = {{2000, "resume\n", UPD_PEER_ROOT, 0}, {3000, "status\n", UPD_PEER_ROOT, 0}};
    f.commands = c32; f.command_count = 2;
    static const struct script s32[] = {START_CLEAN};
    run(s32, 1);
    CHECK(starts(f.replies, "ok not-saved\n{\"version\":1,\"state\":\"waiting\",\"devices\":0,\"paused\":false,"));
    CHECK(strstr(text, " upd=control-root-only\n") && strstr(text, " upd=pause-save-error\n") && f.paused_file == 1);

    /* 33: a pause file that cannot be verified fails safe: start paused. */
    reset(33); f.plug_at = 0; f.paused_file = -1; f.stop_at = 4000;
    static const struct script s33[] = {START_CLEAN};
    run(s33, 1);
    CHECK(strstr(text, " upd=pause-file-blocked paused=yes\n") && !strstr(text, "upd=session-start"));

    /* 34: NUL inside a line is a bad request. */
    reset(34); f.plug_at = 0; f.devices_when_plugged = 0; f.stop_at = 3000;
    static const struct command c34[] = {{2000, "pa\0use\n", UPD_PEER_ROOT, 7}};
    f.commands = c34; f.command_count = 1;
    static const struct script s34[] = {START_CLEAN};
    run(s34, 1);
    CHECK(!strcmp(f.replies, "error=bad-request\n") && !f.pause_writes);

    /* 35: one slice shares 200 ms among its clients: two silent clients and a
     * third that queues behind them is served in a later slice. */
    reset(35); f.plug_at = 0; f.devices_when_plugged = 0; f.stop_at = 4000;
    static const struct command c35[] = {{2000, "", UPD_PEER_ROOT, 0}, {2000, "", UPD_PEER_ROOT, 0},
        {2000, "status\n", UPD_PEER_ROOT, 0}};
    f.commands = c35; f.command_count = 3;
    static const struct script s35[] = {START_CLEAN};
    run(s35, 1);
    CHECK(starts(f.replies, "error=bad-request\nerror=bad-request\n{\"version\":1,") && occurrences("command=invalid") == 2);
    CHECK(f.reply_count == 3 && f.reply_at[1] > f.reply_at[0] && f.reply_at[2] > f.reply_at[1]);

    /* 36: a long unchanged session still rewrites the status once per
     * heartbeat interval (only "updated" moves), never once per slice. */
    reset(36); f.plug_at = 0; f.stop_at = 200000;
    static const struct script s36[] = {START_CLEAN, {"--up", NULL, 6000, 1000000, 3, 0, 0, 3000, 0, 0, 0, 0}};
    run(s36, 2);
    CHECK(f.max_write_gap >= UPD_STATUS_HEARTBEAT_MS && f.max_write_gap <= UPD_STATUS_HEARTBEAT_MS + 1000);
    CHECK(f.heartbeat_writes >= 3 && f.heartbeat_writes <= 4 && f.status_writes <= 14 && !f.status_bad);
    CHECK(f.terms == 1 && f.status_removed == 1);

    /* 37: the native backend on a real temporary tree under build/ (no root:
     * the expected owner is this user, the admin group is this user's group). */
    reset(37);
    native_tests();
    printf("lte-upd checks=%u failures=%u (virtual clock; no device, spawn or launchd)\n", checks, failures);
    return failures ? 1 : 0;
}
