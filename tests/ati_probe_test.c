#include "../src/at_probe.h"
#include "../src/discovery.h"
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
    int open_status, write_status, close_status, read_status;
    int cancel_before_write, cancel_at_read;
    uint64_t read_delay;
    uint8_t written[16];
    uint32_t written_size, write_pipe, write_timeout;
    unsigned reply_count;
    const uint8_t *replies[MAX_REPLIES];
    uint32_t reply_sizes[MAX_REPLIES];
};

static uint64_t fake_now(void *p) { return ((struct fake *)p)->now; }
static int fake_cancelled(void *p) {
    struct fake *f = p;
    if (f->cancel_before_write && f->opens > 0 && f->writes == 0 && f->validates >= 2) return 1;
    return f->cancel_at_read && f->reads >= (unsigned)f->cancel_at_read;
}
static int fake_prepare(void *p, unsigned interface_id) {
    struct fake *f = p;
    CHECK(interface_id == AT_PROBE_INTERFACE && !f->prepared);
    f->prepares++; f->prepared = 1;
    return DJI_OK;
}
static int fake_validate(void *p, unsigned interface_id, int opened, uint8_t *out, uint8_t *in) {
    struct fake *f = p;
    CHECK(interface_id == AT_PROBE_INTERFACE && opened == f->active);
    f->validates++;
    if (opened) { *out = 2; *in = 3; }
    return DJI_OK;
}
static int fake_open(void *p) {
    struct fake *f = p;
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
    return f->write_status;
}
static int fake_read(void *p, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active && pipe == 3 && f->writes == 1);
    CHECK(*size == AT_PROBE_READ_SIZE && timeout > 0 && timeout <= AT_PROBE_READ_TOTAL_MS);
    unsigned index = f->reads++;
    memset(buffer, '7', *size); /* Error buffers hold digits that must never surface. */
    if (index >= f->reply_count) {
        f->now += timeout;
        *size = UINT32_MAX;
        return f->read_status ? f->read_status : AT_PROBE_TIMEOUT;
    }
    f->now += f->read_delay;
    CHECK(f->reply_sizes[index] <= AT_PROBE_READ_SIZE);
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

static char output[16384];
static void capture_begin(FILE **stream) { *stream = tmpfile(); CHECK(*stream != NULL); }
static void capture_end(FILE *stream) {
    output[0] = '\0';
    if (!stream) return;
    rewind(stream);
    size_t length = fread(output, 1, sizeof(output) - 1, stream);
    output[length] = '\0';
    fclose(stream);
}

static int run(struct fake *f, enum at_probe_command command, struct at_probe_result *r) {
    const struct at_probe_backend backend = {f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    FILE *stream;
    capture_begin(&stream);
    int status = at_probe_run_command(&backend, command, r, stream);
    capture_end(stream);
    CHECK(!f->active && f->writes <= 1 && f->reads <= AT_PROBE_READ_LIMIT);
    CHECK(f->releases == f->prepares);
    CHECK(strstr(output, "evidence=format-only protocol=unconfirmed") != NULL);
    CHECK(strstr(output, "7777") == NULL);
    if (f->writes == 1) {
        CHECK(f->write_pipe == 2 && f->write_timeout == AT_PROBE_WRITE_MS);
        if (command == AT_PROBE_COMMAND_ATI) CHECK(f->written_size == 4 && memcmp(f->written, "ATI\r", 4) == 0);
        else CHECK(f->written_size == 3 && memcmp(f->written, "AT\r", 3) == 0);
    }
    return status;
}

/* Prints lines from a synthetic buffer into output. */
static struct ati_summary lines(const char *bytes, size_t size) {
    struct ati_summary summary;
    FILE *stream;
    capture_begin(&stream);
    ati_print_lines((const uint8_t *)bytes, size, stream, &summary);
    capture_end(stream);
    return summary;
}
#define LINES(literal) lines((literal), sizeof(literal) - 1)

static void line_checks(void) {
    struct ati_summary s;
    s = LINES("ATI\r\r\nQuectel\r\nEG25\r\nRevision: EG25GGBR07A08M2G\r\n\r\nOK\r\n");
    CHECK(s.lines == 5 && s.result == ATI_RESULT_OK && s.omitted == 0);
    CHECK(strstr(output, "ati line=1 echo=1\n") != NULL);
    CHECK(strstr(output, "ati line=2 text=\"Quectel\"\n") != NULL);
    CHECK(strstr(output, "ati line=3 text=\"EG25\"\n") != NULL);
    CHECK(strstr(output, "ati line=4 text=\"Revision: EG25GGBR07A08M2G\"\n") != NULL);
    CHECK(strstr(output, "ati line=5 result=ok\n") != NULL);

    /* Digit masking: 8, 15, 20 digits masked; 7 kept; several runs in one line. */
    s = LINES("\r\nA 12345678 B\r\n");
    CHECK(strstr(output, "text=\"A <digits:8> B\"") != NULL && strstr(output, "12345678") == NULL);
    s = LINES("\r\nIMEI: 490154203237518\r\n");
    CHECK(strstr(output, "text=\"IMEI: <digits:15>\"") != NULL && strstr(output, "49015420") == NULL);
    s = LINES("\r\n89860012345678901234\r\n");
    CHECK(strstr(output, "text=\"<digits:20>\"") != NULL && strstr(output, "8986") == NULL);
    s = LINES("\r\nv1234567\r\n");
    CHECK(strstr(output, "text=\"v1234567\"") != NULL);
    s = LINES("\r\nx 98765432 y 11112222333 z 42\r\n");
    CHECK(strstr(output, "text=\"x <digits:8> y <digits:11> z 42\"") != NULL);
    CHECK(strstr(output, "98765432") == NULL && strstr(output, "11112222") == NULL);
    /* Changed by boundary rule: both runs touch the buffer's start/end boundary. */
    s = LINES("1234567\n12345678");
    CHECK(strstr(output, "line=1 text=\"<digits:7+>\"\n") != NULL);
    CHECK(strstr(output, "line=2 text=\"<digits:8+>\" partial=1\n") != NULL);
    CHECK(strstr(output, "1234567") == NULL);
    s = LINES("\r\nab 1234567 cd\r\n");
    CHECK(strstr(output, "text=\"ab 1234567 cd\"\n") != NULL);

    /* Allowed symbols only; anything else redacts the whole line. */
    s = LINES("\r\nA-Z_a/z+(1)=2#3.4,5:6;7\r\n");
    CHECK(strstr(output, "text=\"A-Z_a/z+(1)=2#3.4,5:6;7\"") != NULL);
    const char *bad[] = {"ok\tx", "ok\x7fx", "ok\xc3\xa9", "ok<x", "ok@x", "ok\"x", "ok\\x", "ok%x", "ok!x", "ok*x"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        s = lines(bad[i], strlen(bad[i]));
        char expect[64];
        /* Changed: no trailing CR/LF, so the line is now flagged partial=1. */
        snprintf(expect, sizeof(expect), "ati line=1 redacted=disallowed length=%zu partial=1\n", strlen(bad[i]));
        CHECK(strstr(output, expect) != NULL && strstr(output, "text=") == NULL);
    }
    s = lines("ab\0cd", 5);
    CHECK(strstr(output, "redacted=disallowed length=5") != NULL);
    s = LINES("\r\nsecret@12345678901\r\n");
    CHECK(strstr(output, "secret") == NULL && strstr(output, "12345678") == NULL);

    /* Result lines: CME value never printed. */
    s = LINES("\r\nERROR\r\n");
    CHECK(s.result == ATI_RESULT_ERROR && strstr(output, "ati line=1 result=error\n") != NULL);
    s = LINES("\r\n+CME ERROR: private-value 10\r\n");
    CHECK(s.result == ATI_RESULT_ERROR && strstr(output, "private") == NULL && strstr(output, "CME") == NULL);
    s = LINES("\r\nOKAY\r\n");
    CHECK(s.result == ATI_RESULT_NONE && strstr(output, "text=\"OKAY\"") != NULL);
    s = LINES("\r\nATI \r\n");
    CHECK(strstr(output, "echo=1") == NULL && strstr(output, "text=\"ATI \"") != NULL);

    /* 16-line cap; later result lines are still counted. */
    char many[512];
    size_t used = 0;
    for (unsigned i = 0; i < 20; i++) used += (size_t)snprintf(many + used, sizeof(many) - used, "L%u\r\n", i);
    used += (size_t)snprintf(many + used, sizeof(many) - used, "OK\r\n");
    s = lines(many, used);
    CHECK(s.lines == AT_PROBE_ATI_MAX_LINES && s.omitted == 5 && s.result == ATI_RESULT_OK);
    CHECK(strstr(output, "line=16 text=\"L15\"") != NULL && strstr(output, "line=17") == NULL);
    CHECK(strstr(output, "L16") == NULL);

    /* 80-char cut, including a mask marker that would cross the limit. */
    char long_line[200];
    memset(long_line, 'a', 100);
    s = lines(long_line, 100);
    CHECK(strstr(output, " cut=1 partial=1\n") != NULL); /* Changed: unterminated input. */
    {
        const char *text = strstr(output, "text=\"");
        CHECK(text && strchr(text + 6, '"') - (text + 6) == 80);
    }
    memset(long_line, 'b', 80);
    s = lines(long_line, 80);
    CHECK(strstr(output, "cut=1") == NULL);
    memset(long_line, 'c', 75);
    memcpy(long_line + 75, "123456789012", 12);
    s = lines(long_line, 87);
    CHECK(strstr(output, "cut=1") != NULL && strstr(output, "<digits") == NULL && strstr(output, "1234") == NULL);

    /* Boundary rule: 1..7 visible digits at the end or start are always masked. */
    for (unsigned n = 1; n < AT_PROBE_ATI_DIGIT_RUN; n++) {
        char in[64], expect[64];
        int len = snprintf(in, sizeof(in), "\r\nIMEI %.*s", (int)n, "9876543");
        s = lines(in, (size_t)len);
        snprintf(expect, sizeof(expect), "text=\"IMEI <digits:%u+>\" partial=1\n", n);
        CHECK(strstr(output, expect) != NULL && strstr(output, "98") == NULL);
        CHECK(n == 1 || strstr(output, "9") == NULL);
        len = snprintf(in, sizeof(in), "%.*s\r\nOK\r\n", (int)n, "9876543");
        s = lines(in, (size_t)len);
        snprintf(expect, sizeof(expect), "ati line=1 text=\"<digits:%u+>\"\n", n);
        CHECK(strstr(output, expect) != NULL && strstr(output, "98") == NULL && s.result == ATI_RESULT_OK);
        /* Boundary run preceded/followed by a non-digit: only the touching run is masked. */
        len = snprintf(in, sizeof(in), "%.*s-x 42 y-%.*s", (int)n, "9876543", (int)n, "1357913");
        s = lines(in, (size_t)len);
        snprintf(expect, sizeof(expect), "text=\"<digits:%u+>-x 42 y-<digits:%u+>\" partial=1\n", n, n);
        CHECK(strstr(output, expect) != NULL);
    }
    s = LINES("5643809\r\nOK\r\n");
    CHECK(strstr(output, "ati line=1 text=\"<digits:7+>\"\n") != NULL && strstr(output, "5643809") == NULL);
    s = LINES("\r\nIMEI 4901542");
    CHECK(strstr(output, "text=\"IMEI <digits:7+>\" partial=1") != NULL && strstr(output, "4901542") == NULL);
    s = LINES("490154203237518");
    CHECK(strstr(output, "text=\"<digits:15+>\" partial=1\n") != NULL && strstr(output, "3569") == NULL);
    s = LINES("7");
    CHECK(strstr(output, "text=\"<digits:1+>\" partial=1\n") != NULL);
    s = LINES("abc\r\n");
    CHECK(strstr(output, "text=\"abc\"\n") != NULL && strstr(output, "partial") == NULL);
    s = LINES("\r\nabc");
    CHECK(strstr(output, "text=\"abc\" partial=1\n") != NULL);
    s = LINES("\r\nOK");
    CHECK(strstr(output, "result=ok partial=1\n") != NULL);
    s = LINES("ATI");
    CHECK(strstr(output, "echo=1 partial=1\n") != NULL);
    /* 80-char cut: a boundary run crossing the cut is never shown in part. */
    {
        char cut_line[100];
        memset(cut_line, 'k', 76);
        memcpy(cut_line + 76, "1234567", 7);
        s = lines(cut_line, 83);
        CHECK(strstr(output, "cut=1 partial=1") != NULL && strstr(output, "123") == NULL);
        memset(cut_line, 'k', 70);
        memcpy(cut_line + 70, "123\r\n", 5);
        s = lines(cut_line, 75);
        CHECK(strstr(output, "<digits:3+>\"") == NULL && strstr(output, "123\"\n") != NULL);
    }

    /* Empty and separator-only inputs. */
    s = LINES("\r\n\r\n\n\r");
    CHECK(s.lines == 0 && output[0] == '\0');
    s = lines(NULL, 10);
    CHECK(s.lines == 0);
}

static void append_checks(void) {
    uint8_t dest[AT_PROBE_ATI_BUFFER + 8];
    uint8_t chunk[AT_PROBE_READ_SIZE];
    memset(dest, 0xee, sizeof(dest));
    memset(chunk, 'x', sizeof(chunk));
    int truncated = 0;
    size_t used = 0;
    for (unsigned i = 0; i < 4; i++) used = at_probe_append(dest, AT_PROBE_ATI_BUFFER, used, chunk, sizeof(chunk), &truncated);
    CHECK(used == AT_PROBE_ATI_BUFFER && truncated == 0);
    used = at_probe_append(dest, AT_PROBE_ATI_BUFFER, used, chunk, 1, &truncated);
    CHECK(used == AT_PROBE_ATI_BUFFER && truncated == 1);
    CHECK(dest[AT_PROBE_ATI_BUFFER] == 0xee && dest[AT_PROBE_ATI_BUFFER + 7] == 0xee);
    truncated = 0;
    used = at_probe_append(dest, AT_PROBE_ATI_BUFFER, 2000, chunk, 100, &truncated);
    CHECK(used == AT_PROBE_ATI_BUFFER && truncated == 1 && dest[AT_PROBE_ATI_BUFFER] == 0xee);
    truncated = 0;
    used = at_probe_append(dest, AT_PROBE_ATI_BUFFER, 5000, chunk, 1, &truncated);
    CHECK(used == AT_PROBE_ATI_BUFFER && truncated == 1);
    /* Exact-size heap-like buffer: ASan catches any overrun. */
    uint8_t exact[16];
    truncated = 0;
    used = at_probe_append(exact, sizeof(exact), 10, chunk, 512, &truncated);
    CHECK(used == 16 && truncated == 1);
    uint32_t size = 9;
    CHECK(at_probe_command_bytes(AT_PROBE_COMMAND_AT, &size) && size == 3);
    CHECK(at_probe_command_bytes(AT_PROBE_COMMAND_ATI, &size) && size == 4);
    CHECK(at_probe_command_bytes((enum at_probe_command)(AT_PROBE_COMMAND_CGDATA_TEST + 1), &size) == NULL && size == 0);
}

int main(void) {
    struct fake f;
    struct at_probe_result r;
    line_checks();
    append_checks();

    /* Normal multi-line ATI reply in one read, with echo. */
    f = (struct fake){0};
    ADD(&f, "ATI\r\r\nQuectel\r\nEG25\r\nRevision: EG25GGBR07A08M2G\r\n\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.sent == 1 && r.reads == 1 && f.reads == 2);
    CHECK(strstr(output, "probe interface=2 out=0x03 in=0x84 write=ok bytes=4\n") != NULL);
    CHECK(strstr(output, "probe read=1 bytes=") != NULL && strstr(output, "probe_reply=") == NULL);
    CHECK(strstr(output, "analysis bytes=") == NULL);
    CHECK(strstr(output, "probe status=0 sent=1 reads=1 lines=5 result=ok truncated=0 evidence=") != NULL);

    /* Split across reads, without echo; a line broken mid-read is joined. */
    f = (struct fake){0};
    ADD(&f, "\r\nQuec");
    ADD(&f, "tel\r\nEG2");
    ADD(&f, "5\r\n\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(r.reads == 3 && f.reads == 4);
    CHECK(strstr(output, "ati line=1 text=\"Quectel\"") != NULL && strstr(output, "ati line=2 text=\"EG25\"") != NULL);
    CHECK(strstr(output, "echo=1") == NULL);
    CHECK(strstr(output, "lines=3 result=ok truncated=0") != NULL);

    /* Read cap 4 x 512 fills the 2048 buffer exactly: no overflow, truncated=0. */
    static char big[4][AT_PROBE_READ_SIZE];
    f = (struct fake){0};
    for (unsigned i = 0; i < 4; i++) { memset(big[i], 'q', sizeof(big[i])); big[i][511] = '\n'; add_reply(&f, big[i], sizeof(big[i])); }
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(f.reads == 4 && r.reads == 4 && r.ati_bytes == AT_PROBE_ATI_BUFFER && r.truncated == 0);
    CHECK(strstr(output, "lines=4 result=none truncated=0") != NULL);
    CHECK(strstr(output, "cut=1") != NULL);

    /* No reply: first read timeout. */
    f = (struct fake){0};
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(strstr(output, "probe status=0 sent=1 reads=0 lines=0 result=none truncated=0") != NULL);

    /* Error replies never show CME content. */
    f = (struct fake){0};
    ADD(&f, "ATI\r\r\n+CME ERROR: synthetic-private 3\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(strstr(output, "result=error truncated=0") != NULL && strstr(output, "synthetic") == NULL);

    /* Read error after data: error buffer ignored, collected lines still printed. */
    f = (struct fake){.read_status = DJI_GONE};
    ADD(&f, "\r\nQuectel\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_GONE);
    CHECK(f.closes == 1 && strstr(output, "text=\"Quectel\"") != NULL && strstr(output, "lines=1") != NULL);

    /* Write failure: no read, no retry, closed, sent=0. */
    f = (struct fake){.write_status = AT_PROBE_TIMEOUT};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1);
    CHECK(strstr(output, "write=timeout bytes=0 delivery=unknown") != NULL);
    CHECK(strstr(output, "sent=0 reads=0 lines=0 result=none truncated=0") != NULL);

    /* Cancellation and close-failure priority unchanged. */
    f = (struct fake){.cancel_before_write = 1};
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_CANCELLED);
    CHECK(f.writes == 0 && f.closes == 1);
    f = (struct fake){.cancel_at_read = 1, .close_status = DJI_SYSTEM_ERROR};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_CLOSE_ERROR);
    f = (struct fake){.open_status = DJI_BUSY};
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_BUSY);
    CHECK(f.writes == 0 && f.closes == 0 && f.releases == 1);

    /* --probe-at through the generalized entry keeps its exact bytes and output form. */
    f = (struct fake){0};
    ADD(&f, "AT\r\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_AT, &r) == DJI_OK);
    CHECK(strstr(output, "write=ok bytes=3\n") != NULL && strstr(output, "probe_reply=echo-ok") != NULL);
    CHECK(strstr(output, "reply=echo-ok evidence=") != NULL && strstr(output, "ati ") == NULL);
    CHECK(strstr(output, "lines=") == NULL);

    /* Privacy across a full run: masked digits and redacted lines never appear. */
    f = (struct fake){0};
    ADD(&f, "\r\nIMEI 490154203237518\r\nSN<ABCDEF>\r\n");
    ADD(&f, "\r\nICCID 89860012345678901234\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(strstr(output, "49015420") == NULL && strstr(output, "89860012") == NULL);
    CHECK(strstr(output, "ABCDEF") == NULL && strstr(output, "redacted=disallowed length=10") != NULL);
    CHECK(strstr(output, "text=\"IMEI <digits:15>\"") != NULL && strstr(output, "text=\"ICCID <digits:20>\"") != NULL);

    /* Verifier repro 1: one read ending mid-number, then timeout. */
    f = (struct fake){0};
    ADD(&f, "\r\nIMEI 4901542");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(strstr(output, "ati line=1 text=\"IMEI <digits:7+>\" partial=1\n") != NULL);
    CHECK(strstr(output, "4901542") == NULL && strstr(output, "490") == NULL);
    /* Repro 2: 4-read cap ends mid-number. */
    f = (struct fake){0};
    ADD(&f, "A\r\n"); ADD(&f, "B\r\n"); ADD(&f, "C\r\n"); ADD(&f, "IMEI 4901542"); ADD(&f, "03237518\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(f.reads == 4 && strstr(output, "ati line=4 text=\"IMEI <digits:7+>\" partial=1\n") != NULL);
    CHECK(strstr(output, "4901542") == NULL && strstr(output, "truncated=0") != NULL);
    /* Repro 3: buffer starts mid-number (queued data before the write). */
    f = (struct fake){0};
    ADD(&f, "5643809\r\nOK\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(strstr(output, "ati line=1 text=\"<digits:7+>\"\n") != NULL && strstr(output, "5643809") == NULL);
    /* Deadline end mid-number. */
    f = (struct fake){.read_delay = 1000};
    ADD(&f, "\r\nSN 12"); ADD(&f, "3456"); ADD(&f, "789012\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(f.reads == 2 && strstr(output, "text=\"SN <digits:6+>\" partial=1") != NULL && strstr(output, "123") == NULL);
    /* Read error mid-number. */
    f = (struct fake){.read_status = DJI_GONE};
    ADD(&f, "\r\nSN 4455667");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_GONE);
    CHECK(strstr(output, "text=\"SN <digits:7+>\" partial=1") != NULL && strstr(output, "445") == NULL);
    /* Cancel during reads, after a read ending mid-number. */
    f = (struct fake){.cancel_at_read = 3};
    ADD(&f, "\r\nX"); ADD(&f, " 24681"); ADD(&f, "357\r\n");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_CANCELLED);
    CHECK(f.reads == 3 && r.reads == 2 && strstr(output, "text=\"X <digits:5+>\" partial=1") != NULL);
    CHECK(strstr(output, "2468") == NULL);
    /* 2048 truncation ending mid-number: dropped side is the end boundary. */
    {
        static char fill[3][AT_PROBE_READ_SIZE], tail[AT_PROBE_READ_SIZE];
        f = (struct fake){0};
        for (unsigned i = 0; i < 3; i++) { memset(fill[i], 'w', sizeof(fill[i])); fill[i][0] = '\n'; add_reply(&f, fill[i], sizeof(fill[i])); }
        memset(tail, 'w', sizeof(tail));
        tail[0] = '\n';
        tail[499] = '\n';
        memcpy(tail + 500, "ID 12345", 8);
        add_reply(&f, tail, 508); /* Exactly 2044 bytes so far. */
        CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
        CHECK(r.ati_bytes == 2044 && r.truncated == 0);
        CHECK(strstr(output, "text=\"ID <digits:5+>\" partial=1\n") != NULL && strstr(output, "12345") == NULL);
        f = (struct fake){0};
        for (unsigned i = 0; i < 3; i++) add_reply(&f, fill[i], sizeof(fill[i]));
        tail[504] = '\n';
        memcpy(tail + 505, "4321098", 7);
        add_reply(&f, tail, sizeof(tail)); /* Exactly fills 2048; the run touches the last byte. */
        CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
        CHECK(r.ati_bytes == AT_PROBE_ATI_BUFFER && r.truncated == 0);
        CHECK(strstr(output, "text=\"<digits:7+>\" partial=1\n") != NULL && strstr(output, "4321098") == NULL);
    }
    /* Real truncation: the 4 x 512 cap cannot exceed 2048, so drive append + print directly. */
    {
        static uint8_t joined[AT_PROBE_ATI_BUFFER];
        static uint8_t chunk[AT_PROBE_ATI_BUFFER];
        memset(chunk, 'v', sizeof(chunk));
        chunk[0] = '\n';
        chunk[2042] = '\n';
        memcpy(chunk + 2043, "AB 12", 5); /* Ends exactly at byte 2047. */
        int truncated = 0;
        size_t used = at_probe_append(joined, sizeof(joined), 0, chunk, sizeof(chunk), &truncated);
        used = at_probe_append(joined, sizeof(joined), used, (const uint8_t *)"345\r\n", 5, &truncated);
        CHECK(used == AT_PROBE_ATI_BUFFER && truncated == 1);
        struct ati_summary t = lines((const char *)joined, used);
        CHECK(t.lines == 2 && strstr(output, "ati line=2 text=\"AB <digits:2+>\" partial=1\n") != NULL);
        CHECK(strstr(output, "AB 12") == NULL && strstr(output, "345") == NULL);
    }

    /* Whole-buffer digits through a full run. */
    f = (struct fake){0};
    ADD(&f, "0123");
    CHECK(run(&f, AT_PROBE_COMMAND_ATI, &r) == DJI_OK);
    CHECK(strstr(output, "text=\"<digits:4+>\" partial=1") != NULL && strstr(output, "0123") == NULL);

    CHECK(at_probe_run_command(NULL, AT_PROBE_COMMAND_ATI, &r, NULL) == DJI_SYSTEM_ERROR);
    f = (struct fake){0};
    {
        const struct at_probe_backend backend = {&f, fake_now, fake_cancelled, fake_prepare, fake_validate,
            fake_open, fake_write, fake_read, fake_close, fake_release};
        CHECK(at_probe_run_command(&backend, (enum at_probe_command)9, &r, NULL) == DJI_SYSTEM_ERROR);
        CHECK(f.prepares == 0 && f.writes == 0);
    }

    printf("ati probe checks=%u failures=%u (fake transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
