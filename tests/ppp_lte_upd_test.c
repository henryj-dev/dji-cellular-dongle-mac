#define _DARWIN_C_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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
static struct {
    uint64_t now, plug_at, unplug_at, replug_at, stop_at;
    int devices_when_plugged, path_bad;
    const struct script *scripts;
    unsigned script_count, spawned, terms, kills, closes, reaps;
    /* Running child. */
    int running, ended, status, connected_printed, eof_given, reaped_now;
    size_t output_offset;
    uint64_t started, term_at;
    const struct script *current;
    volatile sig_atomic_t stop;
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
static const struct upd_backend fake = {NULL, fake_devices, fake_path_ok, fake_spawn, fake_reap, fake_read,
    fake_close, fake_signal, fake_now, fake_sleep};

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
}
static unsigned occurrences(const char *needle) {
    unsigned n = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
static void reset(int number) {
    memset(&f, 0, sizeof(f)); scenario = number; f.now = 1000; f.devices_when_plugged = 1;
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
    printf("lte-upd checks=%u failures=%u (virtual clock; no device, spawn or launchd)\n", checks, failures);
    return failures ? 1 : 0;
}
