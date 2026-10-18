#include "../src/at_probe.h"
#include "../src/discovery.h"
#include "../src/payload_analysis.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

#define MAX_REPLIES 6

struct fake {
    uint64_t now;
    unsigned prepares, validates, opens, writes, reads, closes, releases;
    int active, prepared;
    int prepare_status, open_status, write_status, close_status, read_status;
    int validate_fail_at, validate_status;
    int mismatch; /* 1: wrong OUT, 2: missing IN, 3: same pipe */
    int cancel_before_write, cancel_at_read;
    uint64_t write_delay, read_delay;
    uint8_t written[16];
    uint32_t written_size, write_timeout, write_pipe;
    uint32_t timeouts[8];
    unsigned reply_count;
    const uint8_t *replies[MAX_REPLIES];
    uint32_t reply_sizes[MAX_REPLIES];
    int reads_before_write;
};

static uint64_t fake_now(void *p) { return ((struct fake *)p)->now; }
static int fake_cancelled(void *p) {
    struct fake *f = p;
    if (f->cancel_before_write && f->opens > 0 && f->writes == 0 && f->validates >= 2) return 1;
    return f->cancel_at_read && f->reads >= (unsigned)f->cancel_at_read;
}
static int fake_prepare(void *p, unsigned interface_id) {
    struct fake *f = p;
    CHECK(interface_id == AT_PROBE_INTERFACE);
    CHECK(!f->prepared);
    f->prepares++; f->prepared = 1;
    return f->prepare_status;
}
static int fake_validate(void *p, unsigned interface_id, int opened, uint8_t *out, uint8_t *in) {
    struct fake *f = p;
    CHECK(interface_id == AT_PROBE_INTERFACE);
    CHECK(opened == f->active);
    f->validates++;
    if (f->validate_fail_at && f->validates >= (unsigned)f->validate_fail_at) return f->validate_status;
    if (!opened) return DJI_OK;
    *out = 2; *in = 3;
    if (f->mismatch == 1) *out = 0;
    if (f->mismatch == 2) *in = 0;
    if (f->mismatch == 3) *out = *in = 3;
    return DJI_OK;
}
static int fake_open(void *p) {
    struct fake *f = p;
    CHECK(!f->active);
    f->opens++;
    if (f->open_status == DJI_OK) f->active = 1;
    return f->open_status;
}
static int fake_write(void *p, uint8_t pipe, const uint8_t *bytes, uint32_t size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active);
    f->writes++;
    f->write_pipe = pipe; f->write_timeout = timeout; f->written_size = size;
    if (size <= sizeof(f->written)) memcpy(f->written, bytes, size);
    f->now += f->write_delay;
    return f->write_status;
}
static int fake_read(void *p, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active);
    CHECK(pipe == 3);
    CHECK(*size == AT_PROBE_READ_SIZE);
    CHECK(timeout > 0 && timeout <= AT_PROBE_READ_TOTAL_MS);
    if (f->writes == 0) f->reads_before_write++;
    if (f->reads < 8) f->timeouts[f->reads] = timeout;
    unsigned index = f->reads++;
    memset(buffer, 0xa5, *size);
    if (index >= f->reply_count) {
        f->now += timeout;
        *size = UINT32_MAX; /* Error size lies; production must ignore it. */
        return f->read_status ? f->read_status : AT_PROBE_TIMEOUT;
    }
    f->now += f->read_delay;
    memcpy(buffer, f->replies[index], f->reply_sizes[index]);
    *size = f->reply_sizes[index];
    return DJI_OK;
}
static int fake_close(void *p) {
    struct fake *f = p;
    CHECK(f->active);
    f->closes++; f->active = 0;
    return f->close_status;
}
static void fake_release(void *p) {
    struct fake *f = p;
    CHECK(!f->active);
    f->releases++; f->prepared = 0;
}

static void add_reply(struct fake *f, const char *bytes, size_t size) {
    f->replies[f->reply_count] = (const uint8_t *)bytes;
    f->reply_sizes[f->reply_count] = (uint32_t)size;
    f->reply_count++;
}
#define ADD(f, literal) add_reply((f), (literal), sizeof(literal) - 1)

static char output[8192];
static int run(struct fake *f, struct at_probe_result *result) {
    const struct at_probe_backend backend = {f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    int status = at_probe_run(&backend, result, stream);
    output[0] = '\0';
    if (stream) {
        rewind(stream);
        size_t length = fread(output, 1, sizeof(output) - 1, stream);
        output[length] = '\0';
        fclose(stream);
    }
    CHECK(!f->active);
    CHECK(f->writes <= 1);
    CHECK(f->reads <= AT_PROBE_READ_LIMIT);
    CHECK(f->reads_before_write == 0);
    CHECK(f->closes == f->opens - (f->open_status != DJI_OK ? f->opens : 0));
    CHECK(f->releases == f->prepares);
    CHECK(strstr(output, "evidence=format-only protocol=unconfirmed") != NULL);
    if (f->writes == 1) {
        CHECK(f->written_size == 3 && memcmp(f->written, "AT\r", 3) == 0);
        CHECK(f->write_pipe == 2 && f->write_timeout == AT_PROBE_WRITE_MS);
    }
    return status;
}

static void classify_checks(void) {
    CHECK(at_probe_classify((const uint8_t *)"\r\nOK\r\n", 6) == AT_REPLY_OK);
    CHECK(at_probe_classify((const uint8_t *)"AT\r\r\nOK\r\n", 9) == AT_REPLY_ECHO_OK);
    CHECK(at_probe_classify((const uint8_t *)"\r\nERROR\r\n", 9) == AT_REPLY_ERROR);
    CHECK(at_probe_classify((const uint8_t *)"\r\n+CME ERROR: 10\r\n", 18) == AT_REPLY_ERROR);
    CHECK(at_probe_classify((const uint8_t *)"\r\n+CREG: 0\r\n", 12) == AT_REPLY_CRLF_LINE);
    /* Truncated prefixes and trailing bytes are never ok/echo-ok. */
    CHECK(at_probe_classify((const uint8_t *)"\r\nOK\r", 5) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"\r\nOK\r\nx", 7) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"\r\nOK\r\n\r\n", 8) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"AT\r\r\nOK\r", 8) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"AT\r\r\nOK\r\n\n", 10) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"AT\r", 3) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"OK", 2) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"\r\nok\r\n", 6) == AT_REPLY_CRLF_LINE);
    CHECK(at_probe_classify((const uint8_t *)"\r\nERRO\r\n", 8) == AT_REPLY_CRLF_LINE);
    CHECK(at_probe_classify((const uint8_t *)"\r\n\r\n", 4) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify((const uint8_t *)"\x01\x0c\x00", 3) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify(NULL, 0) == AT_REPLY_UNKNOWN);
    CHECK(at_probe_classify(NULL, 6) == AT_REPLY_UNKNOWN);
    CHECK(strcmp(at_probe_reply_label(AT_REPLY_NONE), "none") == 0);
    CHECK(strcmp(at_probe_reply_label(AT_REPLY_OK), "ok") == 0);
    CHECK(strcmp(at_probe_reply_label(AT_REPLY_ECHO_OK), "echo-ok") == 0);
    CHECK(strcmp(at_probe_reply_label(AT_REPLY_ERROR), "error") == 0);
    CHECK(strcmp(at_probe_reply_label(AT_REPLY_CRLF_LINE), "crlf-line") == 0);
    CHECK(strcmp(at_probe_reply_label(AT_REPLY_UNKNOWN), "unknown") == 0);
}

int main(void) {
    struct fake f;
    struct at_probe_result r;
    classify_checks();

    /* Each normal reply label, then timeout ends the reads. */
    struct { const char *bytes; size_t size; const char *label; } cases[] = {
        {"\r\nOK\r\n", 6, "ok"}, {"AT\r\r\nOK\r\n", 9, "echo-ok"}, {"\r\nERROR\r\n", 9, "error"},
        {"\r\n+QIND: 1\r\n", 12, "crlf-line"}, {"\x01\x02\x03\x04", 4, "unknown"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        f = (struct fake){0};
        add_reply(&f, cases[i].bytes, cases[i].size);
        CHECK(run(&f, &r) == DJI_OK);
        CHECK(f.writes == 1 && r.sent == 1 && r.reads == 1 && f.reads == 2 && r.timeouts == 1);
        char expect[96];
        snprintf(expect, sizeof(expect), "probe read=1 bytes=%zu probe_reply=%s\n", cases[i].size, cases[i].label);
        CHECK(strstr(output, expect) != NULL);
        snprintf(expect, sizeof(expect), "reads=1 reply=%s evidence", cases[i].label);
        CHECK(strstr(output, expect) != NULL);
        CHECK(strstr(output, "probe interface=2 out=0x03 in=0x84 write=ok bytes=3\n") != NULL);
        CHECK(strstr(output, "analysis bytes=") != NULL);
        CHECK(f.closes == 1 && f.releases == 1);
    }

    /* None: the first read times out. */
    f = (struct fake){0};
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads == 1 && r.reads == 0 && r.reply == AT_REPLY_NONE);
    CHECK(f.timeouts[0] == AT_PROBE_READ_TOTAL_MS);
    CHECK(strstr(output, "probe status=0 sent=1 reads=0 reply=none") != NULL);

    /* First non-unknown label wins over a leading unknown read. */
    f = (struct fake){0};
    ADD(&f, "\x7f");
    ADD(&f, "\r\nOK\r\n");
    ADD(&f, "\r\nERROR\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(r.reads == 3 && r.reply == AT_REPLY_OK);

    /* Read cap 4 and remaining-time timeouts from the write completion point. */
    f = (struct fake){.now = 1000, .write_delay = 700, .read_delay = 300};
    for (unsigned i = 0; i < 6; i++) ADD(&f, "\r\n+X\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == AT_PROBE_READ_LIMIT && r.reads == 4);
    CHECK(f.timeouts[0] == 2000 && f.timeouts[1] == 1700 && f.timeouts[2] == 1400 && f.timeouts[3] == 1100);

    /* Deadline: no read starts once 2000ms have passed, never a 0ms timeout. */
    f = (struct fake){.read_delay = 1000};
    for (unsigned i = 0; i < 6; i++) ADD(&f, "\r\n+X\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 2 && f.timeouts[1] == 1000);
    f = (struct fake){.read_delay = 1999};
    for (unsigned i = 0; i < 6; i++) ADD(&f, "\r\n+X\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 2 && f.timeouts[1] == 1);

    /* Zero-length success ends reads. */
    f = (struct fake){0};
    add_reply(&f, "", 0);
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 1 && r.reply == AT_REPLY_UNKNOWN);

    /* Write timeout / error: no read, no retry, no clear, still closed. */
    int write_errors[] = {AT_PROBE_TIMEOUT, DJI_SYSTEM_ERROR, DJI_GONE};
    for (size_t i = 0; i < 3; i++) {
        f = (struct fake){.write_status = write_errors[i]};
        ADD(&f, "\r\nOK\r\n");
        int status = run(&f, &r);
        CHECK(status != DJI_OK && status != AT_PROBE_TIMEOUT);
        CHECK(f.writes == 1 && f.reads == 0 && r.sent == 0 && f.closes == 1 && f.releases == 1);
        CHECK(strstr(output, "bytes=0 delivery=unknown") != NULL);
        CHECK(strstr(output, "sent=0 reads=0 reply=none") != NULL);
    }
    f = (struct fake){.write_status = AT_PROBE_TIMEOUT};
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(strstr(output, "write=timeout") != NULL);
    f = (struct fake){.write_status = AT_PROBE_TIMEOUT, .close_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);

    /* Pipe mismatch or absence: zero writes. */
    for (int m = 1; m <= 3; m++) {
        f = (struct fake){.mismatch = m};
        CHECK(run(&f, &r) == DJI_INVALID_DESCRIPTOR);
        CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1);
        CHECK(strstr(output, "write=skipped") != NULL);
    }
    f = (struct fake){.validate_fail_at = 2, .validate_status = DJI_GONE};
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 0 && f.closes == 1);
    /* Identity lost after write: stop reading, still close. */
    f = (struct fake){.validate_fail_at = 3, .validate_status = DJI_GONE};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1);

    /* Cancellation before write: zero writes. During reads: status 11, closed. */
    f = (struct fake){.cancel_before_write = 1};
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1);
    f = (struct fake){.cancel_at_read = 1};
    ADD(&f, "\r\nOK\r\n");
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 1 && f.reads == 1 && r.reads == 0 && f.closes == 1);
    f = (struct fake){.cancel_at_read = 1, .close_status = DJI_SYSTEM_ERROR};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);

    /* Open failure / busy prepare: no write, no close, release still runs. */
    f = (struct fake){.open_status = DJI_BUSY};
    CHECK(run(&f, &r) == DJI_BUSY);
    CHECK(f.writes == 0 && f.closes == 0 && f.releases == 1);
    f = (struct fake){.prepare_status = DJI_BUSY};
    CHECK(run(&f, &r) == DJI_BUSY);
    CHECK(f.opens == 0 && f.writes == 0 && f.releases == 1);
    /* Read error: stop, close failure has priority. */
    f = (struct fake){.read_status = DJI_GONE};
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.reads == 1 && f.closes == 1);
    f = (struct fake){.close_status = DJI_SYSTEM_ERROR};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    CHECK(strstr(output, "probe status=12 sent=1 reads=1 reply=ok") != NULL);

    /* Privacy: reply contents never reach output, only counts and labels. */
    f = (struct fake){0};
    ADD(&f, "\r\n+CME ERROR: synthetic-private-value\r\n");
    ADD(&f, "\r\n+CGSN: 490154203237518\r\n");
    ADD(&f, "secret-binary\xff");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(r.reads == 3 && r.reply == AT_REPLY_ERROR);
    CHECK(strstr(output, "synthetic") == NULL && strstr(output, "490154") == NULL);
    CHECK(strstr(output, "secret") == NULL && strstr(output, "CME") == NULL && strstr(output, "CGSN") == NULL);
    CHECK(strstr(output, "a5") == NULL && strstr(output, "AT\r") == NULL);

    /* Null backend is refused before any I/O. */
    CHECK(at_probe_run(NULL, &r, NULL) == DJI_SYSTEM_ERROR);

    printf("at probe checks=%u failures=%u (fake transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
