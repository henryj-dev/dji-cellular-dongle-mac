#define _DARWIN_C_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define main dd_main
#include "../src/dji_dongle.c"
#undef main

static unsigned checks, failures;
static int scenario;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL dji-dongle scenario=%d line=%d\n", scenario, __LINE__); } } while (0)

/* Fake file, socket and interface counters. */
static struct {
    const char *status;          /* NULL: read fails with status_error */
    int status_error;
    const char *reply;           /* NULL: request fails with request_error */
    int request_error;
    char sent[32];
    const struct dd_counter *samples[4];
    int sample_counts[4];
    unsigned sample_next, sleeps, stop_after;
    int64_t now;
    unsigned fail_reads; /* the first reads fail with ENOENT */
} f;
static long fake_read_status(void *context, char *buffer, size_t size, int *error) {
    (void)context;
    if (f.fail_reads) { f.fail_reads--; *error = ENOENT; return -1; }
    if (!f.status) { *error = f.status_error; return -1; }
    size_t length = strlen(f.status);
    if (length >= size) { *error = EINVAL; return -1; }
    memcpy(buffer, f.status, length);
    return (long)length;
}
static int fake_request(void *context, const char *command, char *reply, size_t size, int *error) {
    (void)context;
    (void)snprintf(f.sent, sizeof(f.sent), "%s", command);
    if (!f.reply) { *error = f.request_error; return -1; }
    (void)snprintf(reply, size, "%s", f.reply);
    return 0;
}
static int fake_counters(void *context, struct dd_counter *list, unsigned capacity) {
    (void)context;
    unsigned i = f.sample_next++ % 4;
    int count = f.sample_counts[i];
    for (int j = 0; j < count && (unsigned)j < capacity; j++) list[j] = f.samples[i][j];
    return count;
}
static int64_t fake_now_wall(void *context) { (void)context; return f.now; }
static void fake_sleep(void *context, unsigned ms) { (void)context; CHECK(ms == 1000); f.sleeps++; f.now++; }
static int fake_interrupted(void *context) { (void)context; return f.sleeps >= f.stop_after; }
static const struct dd_backend fake = {NULL, fake_read_status, fake_request, fake_counters, fake_now_wall, fake_sleep,
    fake_interrupted};

static char out_text[4096], err_text[4096];
static int run(int argc, const char *const *argv) {
    FILE *out = tmpfile(), *err = tmpfile();
    int code = dd_run(&fake, argc, (char **)argv, out, err);
    memset(out_text, 0, sizeof(out_text)); memset(err_text, 0, sizeof(err_text));
    fseek(out, 0, SEEK_SET); fseek(err, 0, SEEK_SET);
    (void)fread(out_text, 1, sizeof(out_text) - 1, out); (void)fread(err_text, 1, sizeof(err_text) - 1, err);
    fclose(out); fclose(err);
    return code;
}
static void reset(int number) { memset(&f, 0, sizeof(f)); scenario = number; f.now = 1700000800; }

#define CONNECTED "{\"version\":1,\"state\":\"connected\",\"devices\":1,\"paused\":false,\"session_started\":1700000001," \
    "\"connected_since\":1700000077,\"retry_at\":null,\"signal\":{\"quality\":\"ok\",\"rssi_dbm\":-73,\"rsrp_dbm\":-99," \
    "\"rsrq_db\":-10.0,\"measured_at\":1700000001},\"last_end\":{\"result\":\"ended\",\"reason\":\"usb-error\"," \
    "\"at\":1700000000},\"updated\":1700000077}\n"
static int parses(const char *text) { struct dd_status s; return !dd_parse(text, strlen(text), &s); }
/* CONNECTED with one fragment replaced. */
static int parses_with(const char *from, const char *to) {
    char text[DD_STATUS_MAX * 2];
    const char *p = strstr(CONNECTED, from);
    CHECK(p != NULL);
    if (!p) return 1;
    (void)snprintf(text, sizeof(text), "%.*s%s%s", (int)(p - CONNECTED), CONNECTED, to, p + strlen(from));
    return parses(text);
}

/* ---- Native file and socket calls on a temporary tree under build/ ---- */
static const struct dd_backend native_fake = {NULL, native_read_status, native_request, fake_counters, fake_now_wall,
    fake_sleep, fake_interrupted};
static int listen_at(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (fd < 0 || bind(fd, (const struct sockaddr *)&address, sizeof(address)) || listen(fd, 4)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}
static int write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    fputs(text, file);
    return fclose(file);
}
static void native_tests(void) {
    char root[] = "build/dd-native-XXXXXX", status[128], sock[128], link[128], buffer[DD_STATUS_MAX];
    CHECK(mkdtemp(root) != NULL);
    (void)snprintf(status, sizeof(status), "%s/status.json", root);
    (void)snprintf(sock, sizeof(sock), "%s/control.sock", root);
    (void)snprintf(link, sizeof(link), "%s/link.json", root);
    dd_status_path = status; dd_control_path = sock;
    int error = 0;
    /* Missing file; a file not owned by root (the default owner) is refused. */
    CHECK(native_read_status(NULL, buffer, sizeof(buffer), &error) == -1 && error == ENOENT);
    CHECK(!write_text(status, CONNECTED));
    CHECK(native_read_status(NULL, buffer, sizeof(buffer), &error) == -1 && error == EINVAL);
    dd_status_owner = getuid();
    CHECK(native_read_status(NULL, buffer, sizeof(buffer), &error) == (long)strlen(CONNECTED));
    {
        FILE *out = tmpfile(), *err = tmpfile();
        CHECK(dd_run(&native_fake, 3, (char *[]){"dji-dongle", "status", "--json", NULL}, out, err) == 0);
        fclose(out); fclose(err);
    }
    /* Size bounds: empty, or as large as the buffer. */
    CHECK(!write_text(status, ""));
    CHECK(native_read_status(NULL, buffer, sizeof(buffer), &error) == -1 && error == EINVAL);
    CHECK(!write_text(status, CONNECTED));
    CHECK(native_read_status(NULL, buffer, strlen(CONNECTED), &error) == -1 && error == EINVAL);
    /* The status path as a symlink is not followed. */
    CHECK(!symlink("status.json", link));
    dd_status_path = link;
    CHECK(native_read_status(NULL, buffer, sizeof(buffer), &error) == -1 && error == ELOOP);
    dd_status_path = status;
    /* No socket: not installed or not running. */
    FILE *out = tmpfile(), *err = tmpfile();
    CHECK(dd_control(&native_fake, "pause", out, err) == 1);
    memset(err_text, 0, sizeof(err_text)); fseek(err, 0, SEEK_SET); (void)fread(err_text, 1, sizeof(err_text) - 1, err);
    CHECK(strstr(err_text, "not installed or not running") != NULL);
    /* A socket this user may not write: the admin hint. */
    int listener = listen_at(sock);
    CHECK(listener >= 0 && !chmod(sock, 0));
    fseek(err, 0, SEEK_SET); ftruncate(fileno(err), 0);
    CHECK(dd_control(&native_fake, "resume", out, err) == 1);
    memset(err_text, 0, sizeof(err_text)); fseek(err, 0, SEEK_SET); (void)fread(err_text, 1, sizeof(err_text) - 1, err);
    CHECK(!strcmp(err_text, "need an admin account (try: sudo dji-dongle resume)\n"));
    /* A listener that never answers: the reply wait ends. */
    CHECK(!chmod(sock, 0600));
    dd_reply_wait_s = 1;
    fseek(err, 0, SEEK_SET); ftruncate(fileno(err), 0);
    CHECK(dd_control(&native_fake, "reconnect", out, err) == 1);
    memset(err_text, 0, sizeof(err_text)); fseek(err, 0, SEEK_SET); (void)fread(err_text, 1, sizeof(err_text) - 1, err);
    CHECK(!strcmp(err_text, "dji-dongle: no reply from the daemon\n"));
    fclose(out); fclose(err);
    if (listener >= 0) close(listener);
    CHECK(!unlink(sock) && !unlink(link) && !unlink(status) && !rmdir(root));
    dd_status_path = DD_STATUS_PATH; dd_control_path = DD_CONTROL_PATH; dd_status_owner = 0;
    dd_reply_wait_s = DD_REPLY_WAIT_S;
}

int main(void) {
    /* 1: parse the daemon's line, field by field. */
    reset(1);
    {
        struct dd_status s;
        CHECK(!dd_parse(CONNECTED, strlen(CONNECTED), &s));
        CHECK(s.state == DD_CONNECTED && s.devices == 1 && !s.paused && s.session_started == 1700000001 &&
              s.connected_since == 1700000077 && s.retry_at == -1 && s.quality == 1 && s.has_rssi && s.rssi == -73 &&
              s.has_rsrp && s.rsrp == -99 && s.has_rsrq && s.rsrq_tenths == -100 && s.measured_at == 1700000001 &&
              s.end_result == 2 && !strcmp(s.end_reason, "usb-error") && s.end_at == 1700000000 && s.updated == 1700000077);
        const char *nulls = "{\"version\":1,\"state\":\"waiting\",\"devices\":-1,\"paused\":true,\"session_started\":null,"
            "\"connected_since\":null,\"retry_at\":null,\"signal\":{\"quality\":null,\"rssi_dbm\":null,\"rsrp_dbm\":null,"
            "\"rsrq_db\":null,\"measured_at\":null},\"last_end\":{\"result\":\"none\",\"reason\":null,\"at\":null},"
            "\"updated\":null}";
        CHECK(!dd_parse(nulls, strlen(nulls), &s) && s.state == DD_WAITING && s.devices == -1 && s.paused &&
              !s.quality && !s.has_rssi && !s.has_rsrp && !s.has_rsrq && !s.end_result && !s.end_reason[0] && s.updated == -1);
    }
    /* 2: anything off the fixed shape is refused as a whole. */
    reset(2);
    CHECK(parses_with("\"usb-error\"", "\"aaaaaaaaaabbbbbbbbbbccccccccccdddddddddd\""));
    CHECK(!parses_with("\"usb-error\"", "\"aaaaaaaaaabbbbbbbbbbccccccccccdddddddddde\""));
    CHECK(!parses_with("\"connected\"", "\"online\""));
    CHECK(!parses_with("\"version\":1", "\"version\":2"));
    CHECK(!parses_with("-10.0", "-10.05"));
    CHECK(!parses_with("-10.0", "-100.0"));
    CHECK(!parses_with("-10.0", "-10"));
    CHECK(!parses_with("-73", "-7300"));
    CHECK(!parses_with("\"usb-error\"", "\"usb error\""));
    CHECK(!parses_with("\"usb-error\"", "\"usb-error\\\"\""));
    CHECK(!parses_with("\"usb-error\"", "\"USB\""));
    CHECK(!parses_with("\"usb-error\"", "\"\""));
    CHECK(!parses_with("1700000077,\"retry_at\"", "17000000770000,\"retry_at\""));
    CHECK(!parses_with("\"session_started\":1700000001", "\"session_started\":-5"));
    CHECK(!parses_with("\"paused\":false", "\"paused\":0"));
    CHECK(!parses_with("\"quality\":\"ok\"", "\"quality\":\"great\""));
    CHECK(!parses_with(",\"updated\":1700000077", ""));
    CHECK(!parses_with("\"devices\":1", "\"devices\":100"));
    CHECK(!parses_with("}\n", "} "));
    CHECK(!parses(""));
    CHECK(!parses("{}"));
    {
        char with_nul[] = CONNECTED;
        struct dd_status s;
        with_nul[40] = 0;
        CHECK(dd_parse(with_nul, sizeof(with_nul) - 1, &s) == -1);
    }
    /* 3: human status output. */
    reset(3); f.status = CONNECTED; f.now = 1700000077 + 723;
    CHECK(run(2, (const char *[]){"dji-dongle", "status", NULL}) == 0);
    CHECK(!strcmp(out_text, "state: connected (12m 03s)\n"
                            "signal at connect: RSRP -99 dBm, RSRQ -10.0 dB, RSSI -73 dBm\n"
                            "paused: no\n"
                            "last end: ended (usb-error), 13m 20s ago\n") && !err_text[0]);
    /* --json prints the daemon's line as is. */
    CHECK(run(3, (const char *[]){"dji-dongle", "status", "--json", NULL}) == 0 && !strcmp(out_text, CONNECTED));
    /* Other states read as what they mean now. */
    reset(3); f.now = 1700000010;
    f.status = "{\"version\":1,\"state\":\"retry-wait\",\"devices\":1,\"paused\":false,\"session_started\":null,"
        "\"connected_since\":null,\"retry_at\":1700000018,\"signal\":{\"quality\":\"partial\",\"rssi_dbm\":-73,"
        "\"rsrp_dbm\":null,\"rsrq_db\":null,\"measured_at\":1700000001},\"last_end\":{\"result\":\"ended\",\"reason\":null,"
        "\"at\":null},\"updated\":1700000008}";
    CHECK(run(2, (const char *[]){"dji-dongle", "status", NULL}) == 0);
    CHECK(!strcmp(out_text, "state: retry-wait (next try in 8s)\n"
                            "signal at connect: RSRP unknown, RSRQ unknown, RSSI -73 dBm\n"
                            "paused: no\n"
                            "last end: ended\n"));
    f.status = "{\"version\":1,\"state\":\"waiting\",\"devices\":0,\"paused\":true,\"session_started\":null,"
        "\"connected_since\":null,\"retry_at\":null,\"signal\":{\"quality\":null,\"rssi_dbm\":null,\"rsrp_dbm\":null,"
        "\"rsrq_db\":null,\"measured_at\":null},\"last_end\":{\"result\":\"none\",\"reason\":null,\"at\":null},"
        "\"updated\":1700000008}";
    CHECK(run(2, (const char *[]){"dji-dongle", "status", NULL}) == 0);
    CHECK(!strcmp(out_text, "state: waiting (no dongle)\nsignal at connect: not measured yet\npaused: yes\n"));
    /* 4: no file, an unreadable or invalid one. */
    reset(4); f.status_error = ENOENT;
    CHECK(run(2, (const char *[]){"dji-dongle", "status", NULL}) == 1 && !out_text[0] &&
          !strcmp(err_text, "dji-dongle: no status (daemon not installed or not running)\n"));
    f.status_error = EINVAL;
    CHECK(run(2, (const char *[]){"dji-dongle", "status", NULL}) == 1 &&
          !strcmp(err_text, "dji-dongle: cannot read the status file\n"));
    f.status = "{\"version\":1,\"state\":\"connected\"}";
    CHECK(run(3, (const char *[]){"dji-dongle", "status", "--json", NULL}) == 1 && !out_text[0] &&
          !strcmp(err_text, "dji-dongle: the status file is not valid\n"));
    /* 5: control commands and the replies the daemon can give. */
    reset(5); f.reply = "ok";
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 0 && !strcmp(out_text, "paused\n") &&
          !strcmp(f.sent, "pause"));
    CHECK(run(2, (const char *[]){"dji-dongle", "resume", NULL}) == 0 && !strcmp(out_text, "resumed\n"));
    CHECK(run(2, (const char *[]){"dji-dongle", "reconnect", NULL}) == 0 && !strcmp(out_text, "reconnecting\n") &&
          !strcmp(f.sent, "reconnect"));
    f.reply = "ok already-paused";
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 0 && !strcmp(out_text, "already paused\n"));
    f.reply = "ok ending-session";
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 0 &&
          !strcmp(out_text, "pausing (current session is ending; up to 60 s)\n"));
    f.reply = "ok not-saved";
    CHECK(run(2, (const char *[]){"dji-dongle", "resume", NULL}) == 0 &&
          !strcmp(out_text, "resumed (not saved: a daemon restart pauses again)\n"));
    f.reply = "ok already-running";
    CHECK(run(2, (const char *[]){"dji-dongle", "resume", NULL}) == 0 && !strcmp(out_text, "already running\n"));
    f.reply = "ok not-saved";
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 0 &&
          !strcmp(out_text, "paused (not saved: a daemon restart resumes)\n"));
    f.reply = "error=denied";
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 1 &&
          !strcmp(err_text, "need an admin account (try: sudo dji-dongle pause)\n"));
    f.reply = "error=replug-required";
    CHECK(run(2, (const char *[]){"dji-dongle", "reconnect", NULL}) == 1 &&
          !strcmp(err_text, "unplug the dongle and plug it in again first\n"));
    f.reply = "error=paused";
    CHECK(run(2, (const char *[]){"dji-dongle", "reconnect", NULL}) == 1 &&
          !strcmp(err_text, "paused: run dji-dongle resume first\n"));
    f.reply = "error=no-device";
    CHECK(run(2, (const char *[]){"dji-dongle", "reconnect", NULL}) == 1 && !strcmp(err_text, "no dongle plugged in\n"));
    f.reply = "error=stopping";
    CHECK(run(2, (const char *[]){"dji-dongle", "resume", NULL}) == 1 && !strcmp(err_text, "the daemon is stopping\n"));
    /* A reply that does not belong to the command, or anything unknown, is never echoed. */
    f.reply = "ok already-paused";
    CHECK(run(2, (const char *[]){"dji-dongle", "resume", NULL}) == 1 &&
          !strcmp(err_text, "dji-dongle: unexpected reply from the daemon\n"));
    f.reply = "\033[2Jhello";
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 1 && !strstr(err_text, "hello") && !out_text[0]);
    /* Socket errors. */
    f.reply = NULL; f.request_error = EACCES;
    CHECK(run(2, (const char *[]){"dji-dongle", "resume", NULL}) == 1 &&
          !strcmp(err_text, "need an admin account (try: sudo dji-dongle resume)\n"));
    f.request_error = ENOENT;
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 1 &&
          !strcmp(err_text, "dji-dongle: daemon not installed or not running (no control socket)\n"));
    f.request_error = ECONNREFUSED;
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 1 && strstr(err_text, "not running"));
    f.request_error = ETIMEDOUT;
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 1 &&
          !strcmp(err_text, "dji-dongle: no reply from the daemon\n"));
    f.request_error = EIO;
    CHECK(run(2, (const char *[]){"dji-dongle", "pause", NULL}) == 1 && !strcmp(err_text, "dji-dongle: control socket error\n"));
    /* 6: usage. */
    reset(6); f.reply = "ok";
    CHECK(run(1, (const char *[]){"dji-dongle", NULL}) == 2 && strstr(err_text, "usage: dji-dongle") && !f.sent[0]);
    CHECK(run(3, (const char *[]){"dji-dongle", "pause", "now", NULL}) == 2 && !f.sent[0]);
    CHECK(run(3, (const char *[]){"dji-dongle", "status", "--xml", NULL}) == 2);
    CHECK(run(2, (const char *[]){"dji-dongle", "stop", NULL}) == 2 && !f.sent[0]);
    CHECK(run(2, (const char *[]){"dji-dongle", "--help", NULL}) == 0 && strstr(out_text, "reconnect") && !err_text[0]);
    /* 7: watch: one line per second until interrupted; 32-bit counters wrap. */
    reset(7); f.status = CONNECTED; f.now = 1700000077 + 59; f.stop_after = 3;
    static const struct dd_counter a[] = {{"ppp0", 0xFFFFFF00u, 1000}}, b[] = {{"ppp0", 0x100u, 1000 + 125000}};
    static const struct dd_counter c[] = {{"ppp0", 0x100u, 1000 + 125000}}, d[] = {{"ppp1", 5000, 5000}};
    f.samples[0] = a; f.sample_counts[0] = 1; f.samples[1] = b; f.sample_counts[1] = 1;
    f.samples[2] = c; f.sample_counts[2] = 1; f.samples[3] = d; f.sample_counts[3] = 1;
    CHECK(run(2, (const char *[]){"dji-dongle", "watch", NULL}) == 0 && f.sleeps == 3);
    CHECK(!strcmp(out_text,
        "connected (1m 00s) | RSRP -99 dBm, RSRQ -10.0 dB, RSSI -73 dBm | RX 4.10 kbit/s TX 1.00 Mbit/s\n"
        "connected (1m 01s) | RSRP -99 dBm, RSRQ -10.0 dB, RSSI -73 dBm | RX - TX -\n"));
    /* The status going away ends the watch with the same message as status. */
    reset(7); f.status_error = ENOENT; f.stop_after = 9;
    CHECK(run(2, (const char *[]){"dji-dongle", "watch", NULL}) == 1 && f.sleeps == 3 &&
          strstr(err_text, "daemon not installed or not running") &&
          !strcmp(out_text, "state: unknown (no status; retrying)\nstate: unknown (no status; retrying)\n"));
    /* Gone briefly (a restart): two misses, then the watch goes on. */
    reset(7); f.status = CONNECTED; f.now = 1700000077 + 59; f.fail_reads = 2; f.stop_after = 4;
    f.samples[0] = d; f.sample_counts[0] = 1; f.samples[1] = d; f.sample_counts[1] = 1;
    f.samples[2] = d; f.sample_counts[2] = 1; f.samples[3] = d; f.sample_counts[3] = 1;
    CHECK(run(2, (const char *[]){"dji-dongle", "watch", NULL}) == 0 && !err_text[0] &&
          !strcmp(out_text, "state: unknown (no status; retrying)\nstate: unknown (no status; retrying)\n"
                            "connected (1m 02s) | RSRP -99 dBm, RSRQ -10.0 dB, RSSI -73 dBm | RX 0 bit/s TX 0 bit/s\n"));
    /* A jump of 2^31 bytes or more (interface recreated) is no rate. */
    reset(7); f.status = CONNECTED; f.now = 1700000077 + 59; f.stop_after = 2;
    static const struct dd_counter jump_a[] = {{"ppp0", 100, 100}}, jump_b[] = {{"ppp0", 100 + 0x80000000u, 200}};
    f.samples[0] = jump_a; f.sample_counts[0] = 1; f.samples[1] = jump_b; f.sample_counts[1] = 1;
    CHECK(run(2, (const char *[]){"dji-dongle", "watch", NULL}) == 0 && strstr(out_text, "| RX - TX -\n"));
    /* 8: formatting edges. */
    reset(8);
    {
        char text[32];
        dd_duration(text, sizeof(text), -5); CHECK(!strcmp(text, "0s"));
        dd_duration(text, sizeof(text), 59); CHECK(!strcmp(text, "59s"));
        dd_duration(text, sizeof(text), 60); CHECK(!strcmp(text, "1m 00s"));
        dd_duration(text, sizeof(text), 3599); CHECK(!strcmp(text, "59m 59s"));
        dd_duration(text, sizeof(text), 3600 * 26 + 120); CHECK(!strcmp(text, "26h 02m"));
        dd_rate(text, sizeof(text), 0); CHECK(!strcmp(text, "0 bit/s"));
        dd_rate(text, sizeof(text), 124); CHECK(!strcmp(text, "992 bit/s"));
        dd_rate(text, sizeof(text), 125); CHECK(!strcmp(text, "1.00 kbit/s"));
        dd_rate(text, sizeof(text), 450000); CHECK(!strcmp(text, "3.60 Mbit/s"));
        uint64_t rx, tx;
        static const struct dd_counter two_a[] = {{"ppp0", 10, 10}, {"ppp1", 100, 100}};
        static const struct dd_counter two_b[] = {{"ppp1", 150, 300}, {"ppp0", 20, 15}};
        CHECK(dd_rates(two_a, 2, two_b, 2, &rx, &tx) && rx == 60 && tx == 205);
        CHECK(!dd_rates(two_a, 2, two_b, 0, &rx, &tx) && !rx && !tx);
        static const struct dd_counter big_a[] = {{"ppp0", 0, 0}}, big_b[] = {{"ppp0", 0x7FFFFFFFu, 0}};
        CHECK(dd_rates(big_a, 1, big_b, 1, &rx, &tx) == 1 && rx == 0x7FFFFFFFu);
        static const struct dd_counter over_b[] = {{"ppp0", 0x80000000u, 0}};
        CHECK(dd_rates(big_a, 1, over_b, 1, &rx, &tx) == -1);
    }
    /* 9: native file and socket calls. */
    reset(9);
    native_tests();
    printf("dji-dongle checks=%u failures=%u (fakes plus temp files and sockets under build/; no daemon)\n", checks, failures);
    return failures ? 1 : 0;
}
