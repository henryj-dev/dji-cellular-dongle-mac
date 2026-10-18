#include "../src/status_probe.h"
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
    int open_status, close_status, read_status;
    int write_fail_at, write_status;   /* 1-based write index that fails */
    int cancel_after_writes;           /* cancel once this many writes are done */
    int cancel_at_read;
    int clock_back;                    /* now_ms goes backwards after this many calls */
    int clock_bad_at;                  /* 1-based now_ms call that returns the sentinel */
    int clock_overflow;                /* clock so close to UINT64_MAX that the deadline would wrap */
    unsigned now_calls;
    uint64_t read_delay;
    uint8_t written[4][16];
    uint32_t written_size[4];
    unsigned reads_per_command[4];
    unsigned reads_before_write;
    unsigned reply_count[4];
    const uint8_t *replies[4][MAX_REPLIES];
    uint32_t reply_sizes[4][MAX_REPLIES];
};

static uint64_t fake_now(void *p) {
    struct fake *f = p;
    f->now_calls++;
    if (f->clock_bad_at && f->now_calls == (unsigned)f->clock_bad_at) return AT_PROBE_CLOCK_ERROR;
    if (f->clock_back && f->now_calls > (unsigned)f->clock_back) return f->now - 1;
    if (f->clock_overflow) return UINT64_MAX - 1000;
    return f->now;
}
static int fake_cancelled(void *p) {
    struct fake *f = p;
    if (f->cancel_after_writes && f->writes >= (unsigned)f->cancel_after_writes) return 1;
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
    CHECK(f->active && pipe == 2 && timeout == AT_PROBE_WRITE_MS);
    CHECK(f->writes < 4);
    CHECK(at_probe_is_fixed_command(bytes, size));
    if (f->writes < 4 && size <= 16) { memcpy(f->written[f->writes], bytes, size); f->written_size[f->writes] = size; }
    f->writes++;
    if (f->write_fail_at && f->writes == (unsigned)f->write_fail_at) return f->write_status;
    return DJI_OK;
}
static int fake_read(void *p, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active && pipe == 3);
    CHECK(*size == AT_PROBE_READ_SIZE && timeout > 0 && timeout <= AT_PROBE_READ_TOTAL_MS);
    if (f->writes == 0) { f->reads_before_write++; return AT_PROBE_TIMEOUT; }
    unsigned command = f->writes - 1;
    unsigned index = f->reads_per_command[command]++;
    f->reads++;
    memset(buffer, '9', *size); /* Error buffers hold digits that must never surface. */
    if (index >= f->reply_count[command]) {
        f->now += timeout;
        *size = UINT32_MAX;
        return f->read_status ? f->read_status : AT_PROBE_TIMEOUT;
    }
    if (f->reply_sizes[command][index] == 0) { *size = 0; return DJI_OK; }
    f->now += f->read_delay;
    memcpy(buffer, f->replies[command][index], f->reply_sizes[command][index]);
    *size = f->reply_sizes[command][index];
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

static void add_reply(struct fake *f, unsigned command, const char *bytes, size_t size) {
    f->replies[command][f->reply_count[command]] = (const uint8_t *)bytes;
    f->reply_sizes[command][f->reply_count[command]] = (uint32_t)size;
    f->reply_count[command]++;
}
#define ADD(f, c, literal) add_reply((f), (c), (literal), sizeof(literal) - 1)

static char output[32768];
static void capture_begin(FILE **stream) { *stream = tmpfile(); CHECK(*stream != NULL); }
static void capture_end(FILE *stream) {
    output[0] = '\0';
    if (!stream) return;
    rewind(stream);
    size_t length = fread(output, 1, sizeof(output) - 1, stream);
    output[length] = '\0';
    fclose(stream);
}

static const char *const EXPECTED[4] = {"AT+CPIN?\r", "AT+CSQ\r", "AT+COPS?\r", "AT+CEREG?\r"};

static int run(struct fake *f, struct status_probe_result *r) {
    const struct at_probe_backend backend = {f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    FILE *stream;
    capture_begin(&stream);
    int status = status_probe_run(&backend, r, stream);
    capture_end(stream);
    CHECK(!f->active && f->writes <= 4 && f->reads_before_write == 0);
    CHECK(f->releases == f->prepares);
    for (unsigned i = 0; i < 4; i++) CHECK(f->reads_per_command[i] <= AT_PROBE_READ_LIMIT);
    for (unsigned i = 0; i < f->writes && i < 4; i++)
        CHECK(f->written_size[i] == strlen(EXPECTED[i]) && memcmp(f->written[i], EXPECTED[i], f->written_size[i]) == 0);
    CHECK(strstr(output, "evidence=format-only protocol=unconfirmed") != NULL);
    CHECK(strstr(output, "stop=") != NULL);
    CHECK(strstr(output, "999") == NULL);
    return status;
}

static struct status_command_result parse(unsigned index, const char *bytes, size_t size) {
    struct status_command_result command;
    memset(&command, 0, sizeof(command));
    FILE *stream;
    capture_begin(&stream);
    status_print_lines(index, (const uint8_t *)bytes, size, stream, &command);
    capture_end(stream);
    return command;
}
#define PARSE(i, literal) parse((i), (literal), sizeof(literal) - 1)
#define HAS(text) CHECK(strstr(output, (text)) != NULL)
#define LACKS(text) CHECK(strstr(output, (text)) == NULL)

#define VERDICT(c, a, r) CHECK((c).attribution == (a) && (c).attribution_reason == (r))

static void attribution_checks(void) {
    struct status_command_result c;
    /* Own echo first, own prefix, one final last: format match only. */
    c = PARSE(0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY);
    HAS("cpin=\"READY\""); HAS("status cmd=cpin attribution=matched reason=format-only\n");
    c = PARSE(0, "AT+CPIN?\r\r\n+CME ERROR: 10\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY); HAS("cme=10\n");
    /* No echo: values still filtered and shown, but not attributed. */
    c = PARSE(0, "\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_ECHO);
    HAS("cpin=\"READY\""); HAS("status cmd=cpin attribution=unproven reason=no-echo\n");

    /* Synthetic foreign lines of length 17 / 17 / 10. Own values are withheld, length only. */
    c = PARSE(0, "AT+CGDATA=\"PPP\",1\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FOREIGN_ECHO);
    HAS("line=1 other=1 length=17\n"); HAS("line=3 withheld=1 length=12\n");
    HAS("status cmd=cpin attribution=mismatch reason=foreign-echo\n"); LACKS("READY"); LACKS("PPP");
    c = PARSE(0, "AT+CPIN?\r\r\n+CEREG: 0,1,\"ABC\"\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FOREIGN_PREFIX);
    HAS("line=2 unsolicited=1 length=17\n"); HAS("line=3 withheld=1 length=12\n"); LACKS("READY"); LACKS("ABC");
    /* Unknown + URC before own echo is no foreign evidence on its own: unproven, never mismatch. */
    c = PARSE(0, "\r\n+QIND: RDY\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_UNLISTED_PREFIX);
    HAS("line=1 unsolicited=1 length=10\n"); HAS("cpin=\"READY\""); LACKS("withheld="); LACKS("QIND");
    HAS("status cmd=cpin attribution=unproven reason=unlisted-prefix\n");
    /* With known foreign evidence beside it, mismatch still wins. */
    c = PARSE(0, "\r\n+QIND: RDY\r\nOK\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FINAL_BEFORE_ECHO);
    HAS("line=4 withheld=1 length=12\n"); LACKS("READY"); LACKS("QIND");
    c = PARSE(0, "\r\n+QIND: RDY\r\n+CSQ: 20,99\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_STALE_BEFORE_ECHO); LACKS("READY"); LACKS("QIND");
    /* A plain non-+ line before own echo keeps the stale verdict. */
    c = PARSE(0, "\r\nRDY\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_STALE_BEFORE_ECHO);
    HAS("line=1 other=1 length=3\n"); HAS("line=3 withheld=1 length=12\n"); LACKS("READY");
    c = PARSE(0, "\r\n+CPIN: SIM PIN\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_STALE_BEFORE_ECHO);
    HAS("line=1 withheld=1 length=14\n"); LACKS("SIM PIN"); LACKS("READY");
    c = PARSE(0, "AT+CPIN?\r\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_DUP_ECHO); LACKS("READY");
    c = PARSE(0, "\r\nOK\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FINAL_BEFORE_ECHO); LACKS("READY");
    c = PARSE(0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n\r\nERROR\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_MULTI_FINAL); LACKS("READY");
    /* Mismatch wins over every unproven reason. */
    c = PARSE(0, "\r\n+CSQ: 20,99\r\n+CPIN: READY\r\n+CPIN: REA");
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FOREIGN_PREFIX); LACKS("READY"); LACKS("rssi=");

    /* Unlisted + line between own echo and own prefix: not asserted foreign, only unproven. */
    c = PARSE(0, "AT+CPIN?\r\r\n+QIND: SMS DONE 12345678\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_UNLISTED_PREFIX);
    HAS("line=2 unsolicited=1 length=24\n"); HAS("cpin=\"READY\""); LACKS("QIND"); LACKS("12345678"); LACKS("SMS");
    HAS("status cmd=cpin attribution=unproven reason=unlisted-prefix\n");
    /* +CMS ERROR is not a final line for this reader (status_has_final_line unchanged). */
    c = PARSE(0, "AT+CPIN?\r\r\n+CMS ERROR: 500\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_FINAL); CHECK(c.result == STATUS_RESULT_NONE);
    HAS("line=2 unsolicited=1 length=15\n"); LACKS("500");
    c = PARSE(0, "AT+CPIN?\r\r\n+CMS ERROR: 500\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_CMS_ERROR); LACKS("500");
    c = PARSE(0, "AT+CPIN?\r\r\nRDY\r\n+CPIN: READY\r\n\r\nOK\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_OTHER_LINE); LACKS("RDY");
    c = PARSE(0, "AT+CPIN?\r\r\nOK\r\n+CPIN: READY\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_TRAILING);
    c = PARSE(0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\nO");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_PARTIAL); HAS("partial=1 length=1\n");
    c = PARSE(0, "AT+CPIN?\r\r\n+CPIN: READY\r\n");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_FINAL);
    c = PARSE(0, "AT+CPIN?");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_ECHO); HAS("partial=1 length=8\n");
    c = PARSE(0, "");
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_ECHO);
    {
        char many[512];
        size_t used = (size_t)snprintf(many, sizeof(many), "AT+CSQ\r\r\n+CSQ: 20,99\r\n");
        for (unsigned i = 0; i < 20; i++) used += (size_t)snprintf(many + used, sizeof(many) - used, "\r\n");
        for (unsigned i = 0; i < 15; i++) used += (size_t)snprintf(many + used, sizeof(many) - used, "+CSQ: 20,99\r\n");
        used += (size_t)snprintf(many + used, sizeof(many) - used, "OK\r\n");
        c = parse(1, many, used); /* 18 lines: echo, 16 values, OK */
        VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_OMITTED);
        CHECK(c.lines == STATUS_PROBE_MAX_LINES && c.omitted == 2);
        HAS("status cmd=csq attribution=unproven reason=omitted\n");
    }
    /* Exactly 16 lines still matches. */
    {
        char many[512];
        size_t used = (size_t)snprintf(many, sizeof(many), "AT+CSQ\r\r\n");
        for (unsigned i = 0; i < 14; i++) used += (size_t)snprintf(many + used, sizeof(many) - used, "+CSQ: 20,99\r\n");
        used += (size_t)snprintf(many + used, sizeof(many) - used, "OK\r\n");
        c = parse(1, many, used);
        VERDICT(c, STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY); CHECK(c.lines == 16 && c.omitted == 0);
    }

    /* Verdict is computed without a stream; nothing else changes. */
    static const char good[] = "AT+CEREG?\r\r\n+CEREG: 0,1\r\n\r\nOK\r\n";
    static const char shifted[] = "\r\n+COPS: 0\r\n\r\nOK\r\nAT+CEREG?\r";
    memset(&c, 0, sizeof(c));
    status_print_lines(3, (const uint8_t *)good, sizeof(good) - 1, NULL, &c);
    VERDICT(c, STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY); CHECK(c.lines == 3 && c.result == STATUS_RESULT_OK);
    memset(&c, 0, sizeof(c));
    status_print_lines(3, (const uint8_t *)shifted, sizeof(shifted) - 1, NULL, &c);
    VERDICT(c, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FINAL_BEFORE_ECHO);
    /* Bounds: unknown slot or NULL leaves the result untouched; preset counters stay safe. */
    FILE *stream;
    memset(&c, 0, sizeof(c));
    c.attribution_reason = STATUS_REASON_NOT_ANALYZED;
    capture_begin(&stream);
    status_print_lines(STATUS_PROBE_SLOTS, (const uint8_t *)good, sizeof(good) - 1, stream, &c);
    status_print_lines(0, NULL, 4, stream, &c);
    status_print_lines(0, (const uint8_t *)good, sizeof(good) - 1, stream, NULL);
    capture_end(stream);
    CHECK(output[0] == '\0' && c.lines == 0);
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NOT_ANALYZED);
    memset(&c, 0, sizeof(c));
    c.lines = 0xFFFFFFFFu;
    status_print_lines(3, (const uint8_t *)good, sizeof(good) - 1, NULL, &c);
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_OMITTED);
    memset(&c, 0, sizeof(c));
    c.omitted = 1;
    status_print_lines(3, (const uint8_t *)good, sizeof(good) - 1, NULL, &c);
    VERDICT(c, STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_OMITTED);
}

static void parser_checks(void) {
    struct status_command_result c;
    /* Echo, values and results. */
    c = PARSE(0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    HAS("status cmd=cpin line=1 echo=1\n"); HAS("status cmd=cpin line=2 cpin=\"READY\"\n");
    HAS("status cmd=cpin line=3 result=ok\n"); CHECK(c.result == STATUS_RESULT_OK && c.lines == 3);
    c = PARSE(0, "\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n");
    HAS("cpin=\"SIM PIN\"\n"); LACKS("echo=1");
    c = PARSE(0, "\r\n+CPIN: ready\r\n");
    HAS("line=1 redacted=1 length=12\n"); LACKS("ready");
    c = PARSE(0, "\r\n+CPIN: ABCDEFGHIJKLMNOPQRSTU\r\n"); /* 21 chars */
    HAS("redacted=1"); LACKS("ABCDEFGHIJ");
    c = PARSE(0, "\r\n+CPIN: PH-SIM PIN\r\n");
    HAS("redacted=1");
    c = PARSE(0, "\r\n+CME ERROR: SIM not inserted\r\n");
    HAS("line=1 result=cme-error cme=\"SIM not inserted\"\n"); CHECK(c.result == STATUS_RESULT_CME_ERROR);
    c = PARSE(0, "\r\n+CME ERROR: this message is much longer than forty characters total\r\n");
    HAS("cme=redacted\n"); LACKS("longer");
    c = PARSE(0, "\r\n+CME ERROR: bad:value\r\n");
    HAS("cme=redacted\n"); LACKS("bad");
    c = PARSE(0, "\r\n+CME ERROR: 1234567890\r\n");
    HAS("cme=redacted\n"); LACKS("1234567");
    /* Changed: numeric CME codes now print unquoted (cme=10), per the tightened rule. */
    c = PARSE(0, "\r\n+CME ERROR: 10\r\n");
    HAS("cme=10\n"); LACKS("cme=\"");
    c = PARSE(0, "\r\n+CME ERROR: 100\r\n");
    HAS("cme=100\n");
    c = PARSE(0, "\r\n+CME ERROR: 1000\r\n");
    HAS("cme=redacted\n"); LACKS("1000");
    c = PARSE(0, "\r\n+CME ERROR: 1A2B 01A2B3C4\r\n");
    HAS("cme=redacted\n"); LACKS("1A2B"); LACKS("01A2B3C4");
    c = PARSE(0, "\r\n+CME ERROR: SIM 1 failure\r\n");
    HAS("cme=redacted\n"); LACKS("failure");
    c = PARSE(0, "\r\n+CME ERROR: SIM2 failure\r\n");
    HAS("cme=redacted\n"); LACKS("SIM2");
    c = PARSE(0, "\r\n+CME ERROR: SIM not inserted\r\n");
    HAS("cme=\"SIM not inserted\"\n");
    c = PARSE(0, "\r\n+CME ERROR: \r\n");
    HAS("cme=redacted\n");
    c = PARSE(0, "\r\n+CME ERROR:    \r\n");
    HAS("cme=redacted\n");
    c = PARSE(0, "\r\n+CME ERROR: -10\r\n");
    HAS("cme=redacted\n");
    /* Total digit count: scattered digits in CPIN/oper are redacted at 7+. */
    c = PARSE(0, "\r\n+CPIN: 123456\r\n");
    HAS("redacted=1"); LACKS("123456");
    c = PARSE(0, "\r\n+CPIN: SIM PUK2\r\n");
    HAS("cpin=\"SIM PUK2\"\n");
    c = PARSE(0, "\r\n+CPIN: A123456\r\n");
    HAS("cpin=\"A123456\"\n"); /* 6 digits total stays under the 7-digit limit. */
    c = PARSE(0, "\r\n+CPIN: A1234567\r\n");
    HAS("redacted=1"); LACKS("1234567");
    c = PARSE(0, "\r\n+CPIN: 1 2 3 4 5 6 7\r\n");
    HAS("redacted=1"); LACKS("cpin=");
    c = PARSE(0, "\r\n+CPIN: READY\r\n");
    HAS("cpin=\"READY\"\n");
    c = PARSE(0, "\r\n+CPIN: SIM PIN\r\n");
    HAS("cpin=\"SIM PIN\"\n");
    c = PARSE(2, "\r\n+COPS: 0,0,\"123456-654321\",7\r\n");
    HAS("redacted=1"); LACKS("123456"); LACKS("654321");
    c = PARSE(2, "\r\n+COPS: 0,0,\"A1 B2 C3 D4 E5 F6 G7\",7\r\n");
    HAS("redacted=1"); LACKS("oper=");
    c = PARSE(2, "\r\n+COPS: 0,2,\"45005\",7\r\n");
    HAS("oper=\"45005\"");
    c = PARSE(2, "\r\n+COPS: 0,2,\"310260\",7\r\n");
    HAS("mode=0 format=2 oper=\"310260\" act=7\n");
    /* Widened operator filter: + / ' _ ( ) are now allowed. SK_Telecom moved here from redacted. */
    const char *opers[] = {"LG U+", "T-Mobile", "O2 - DE", "AT&T", "Vodafone UK", "SK Telecom", "KT",
                           "Movistar (ES)", "SK_Telecom", "Op/Net", "O'Net"};
    for (size_t i = 0; i < sizeof(opers) / sizeof(opers[0]); i++) {
        char in[96], expect[96];
        int len = snprintf(in, sizeof(in), "\r\n+COPS: 0,0,\"%s\",7\r\n", opers[i]);
        c = parse(2, in, (size_t)len);
        snprintf(expect, sizeof(expect), "mode=0 format=0 oper=\"%s\" act=7\n", opers[i]);
        HAS(expect); LACKS("redacted");
    }
    const char *bad_opers[] = {"\r\n+COPS: 0,0,\"a,b\",7\r\n", "\r\n+COPS: 0,0,\"a\"b\",7\r\n",
                               "\r\n+COPS: 0,0,\"ABCDEFGHIJKLMNOPQRSTUVWXYZ+/()_ab\",7\r\n",
                               "\r\n+COPS: 0,0,\"Net (1234567)\",7\r\n", "\r\n+COPS: 0,0,\"12/34/56/7\",7\r\n",
                               "\r\n+COPS: 0,0,\"LG\tU+\",7\r\n", "\r\n+COPS: 0,0,\"LG@U+\",7\r\n"};
    for (size_t i = 0; i < sizeof(bad_opers) / sizeof(bad_opers[0]); i++) {
        c = parse(2, bad_opers[i], strlen(bad_opers[i]));
        HAS("redacted=1"); LACKS("oper="); LACKS("1234567"); LACKS("34/56");
    }
    c = PARSE(0, "\r\nERROR\r\n");
    HAS("line=1 result=error\n"); CHECK(c.result == STATUS_RESULT_ERROR);

    c = PARSE(1, "AT+CSQ\r\r\n+CSQ: 21,99\r\n\r\nOK\r\n");
    HAS("status cmd=csq line=1 echo=1\n"); HAS("status cmd=csq line=2 rssi=21 ber=99\n");
    c = PARSE(1, "\r\n+CSQ: 99,99\r\n");
    HAS("rssi=99 ber=99\n");
    c = PARSE(1, "\r\n+CSQ:0,0\r\n");
    HAS("rssi=0 ber=0\n");
    const char *bad_csq[] = {"\r\n+CSQ: -1,99\r\n", "\r\n+CSQ: 1234,99\r\n", "\r\n+CSQ: 21,\r\n",
                             "\r\n+CSQ: ,99\r\n", "\r\n+CSQ: 21,99,5\r\n", "\r\n+CSQ: \"21\",99\r\n",
                             "\r\n+CSQ: 21 ,99\r\n", "\r\n+CSQ: 21\r\n"};
    for (size_t i = 0; i < sizeof(bad_csq) / sizeof(bad_csq[0]); i++) {
        c = parse(1, bad_csq[i], strlen(bad_csq[i]));
        HAS("redacted=1"); LACKS("rssi=");
    }

    c = PARSE(2, "AT+COPS?\r\r\n+COPS: 0\r\n\r\nOK\r\n");
    HAS("status cmd=cops line=2 mode=0\n"); LACKS("format=");
    c = PARSE(2, "\r\n+COPS: 0,0,\"SK Telecom\",7\r\n");
    HAS("mode=0 format=0 oper=\"SK Telecom\" act=7\n");
    c = PARSE(2, "\r\n+COPS: 0,2,\"45005\",7\r\n");
    HAS("mode=0 format=2 oper=\"45005\" act=7\n");
    c = PARSE(2, "\r\n+COPS: 1,0,\"AT&T\"\r\n");
    HAS("mode=1 format=0 oper=\"AT&T\"\n"); LACKS("act=");
    c = PARSE(2, "\r\n+COPS: 0,1,\"KT-LTE.x\",7\r\n");
    HAS("oper=\"KT-LTE.x\"");
    const char *bad_cops[] = {"\r\n+COPS: 0,0,\"SK\"Telecom\",7\r\n", "\r\n+COPS: 0,0,\"SK\r\n",
                              "\r\n+COPS: -1\r\n", "\r\n+COPS: 100\r\n", "\r\n+COPS: 0,0,\"x\",123\r\n",
                              "\r\n+COPS: 0,0,\"\",7\r\n", "\r\n+COPS: 0,,\"KT\",7\r\n", "\r\n+COPS: 0,0\r\n",
                              "\r\n+COPS: 0,2,\"4500512\",7\r\n", "\r\n+COPS: 0,0,\"SK,Telecom\",7\r\n",
                              "\r\n+COPS: 0,0,\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefg\",7\r\n", "\r\n+COPS: 0,0,\"KT\",\r\n",
                              "\r\n+COPS: 0,0,\"KT\",7,1\r\n"};
    for (size_t i = 0; i < sizeof(bad_cops) / sizeof(bad_cops[0]); i++) {
        c = parse(2, bad_cops[i], strlen(bad_cops[i]));
        HAS("redacted=1"); LACKS("mode="); LACKS("4500512"); LACKS("Telecom");
    }

    c = PARSE(3, "AT+CEREG?\r\r\n+CEREG: 0,1\r\n\r\nOK\r\n");
    HAS("status cmd=cereg line=2 n=0 stat=1 extra_fields=0\n");
    c = PARSE(3, "\r\n+CEREG: 2,1,\"1A2B\",\"01C3D4E5\",7\r\n\r\nOK\r\n");
    HAS("n=2 stat=1 extra_fields=3\n"); LACKS("1A2B"); LACKS("01C3D4E5"); LACKS("C3D4"); LACKS(",7");
    c = PARSE(3, "\r\n+CEREG: 4,5,\"FFEE\",\"0ABCDEF1\",7,,,\"00100100\",\"01011111\"\r\n");
    HAS("n=4 stat=5 extra_fields=7\n"); LACKS("FFEE"); LACKS("0ABCDEF1"); LACKS("00100100");
    const char *bad_cereg[] = {"\r\n+CEREG: 0\r\n", "\r\n+CEREG: -1,1\r\n", "\r\n+CEREG: 0,123\r\n",
                               "\r\n+CEREG: ,1\r\n", "\r\n+CEREG: 0,1x,\"1A2B\"\r\n", "\r\n+CEREG: \"0\",1\r\n"};
    for (size_t i = 0; i < sizeof(bad_cereg) / sizeof(bad_cereg[0]); i++) {
        c = parse(3, bad_cereg[i], strlen(bad_cereg[i]));
        HAS("redacted=1"); LACKS("stat="); LACKS("1A2B");
    }

    /* Unsolicited, other, cross-command prefix and partial lines. */
    c = PARSE(1, "\r\n+QIND: SMS DONE 12345678\r\n+CSQ: 20,99\r\n+CPIN: READY\r\nRDY\r\n\r\nOK\r\n");
    HAS("line=1 unsolicited=1 length=24\n"); HAS("line=2 withheld=1 length=11\n"); LACKS("rssi=");
    HAS("line=3 unsolicited=1 length=12\n"); HAS("line=4 other=1 length=3\n");
    LACKS("QIND"); LACKS("12345678"); LACKS("READY"); LACKS("RDY");
    c = PARSE(3, "\r\n+CEREG: 2,1,\"1A2B\"");
    HAS("line=1 partial=1 length=18\n"); LACKS("stat="); LACKS("1A2B");
    c = PARSE(0, "\r\nOK");
    HAS("partial=1 length=2\n"); CHECK(c.result == STATUS_RESULT_NONE);
    c = PARSE(0, "\r\n+CPIN: READY\r\nO");
    HAS("line=2 partial=1 length=1\n");

    /* Final line detection needs a terminator. */
    CHECK(status_has_final_line((const uint8_t *)"\r\nOK\r\n", 6));
    CHECK(!status_has_final_line((const uint8_t *)"\r\nOK", 4));
    CHECK(status_has_final_line((const uint8_t *)"x\r\nERROR\n", 9));
    CHECK(status_has_final_line((const uint8_t *)"\r\n+CME ERROR: 10\r\n", 18));
    CHECK(!status_has_final_line((const uint8_t *)"\r\n+CSQ: 1,2\r\nOKAY\r\n", 19));
    CHECK(!status_has_final_line(NULL, 4));

    /* 16-line cap. */
    char many[512];
    size_t used = 0;
    for (unsigned i = 0; i < 20; i++) used += (size_t)snprintf(many + used, sizeof(many) - used, "L%u\r\n", i);
    used += (size_t)snprintf(many + used, sizeof(many) - used, "OK\r\n");
    c = parse(1, many, used);
    CHECK(c.lines == STATUS_PROBE_MAX_LINES && c.omitted == 5 && c.result == STATUS_RESULT_OK);
    LACKS("line=17");

    attribution_checks();

    CHECK(strcmp(status_probe_name(0), "cpin") == 0 && strcmp(status_probe_name(3), "cereg") == 0);
    CHECK(strcmp(status_probe_name(4), "unknown") == 0);
    CHECK(status_probe_command(0) == AT_PROBE_COMMAND_CPIN && status_probe_command(3) == AT_PROBE_COMMAND_CEREG);
}

static void table_checks(void) {
    static const uint8_t lookalike[9] = {'A', 'T', '+', 'C', 'P', 'I', 'N', '?', '\r'};
    uint32_t size = 0;
    const uint8_t *cpin = at_probe_command_bytes(AT_PROBE_COMMAND_CPIN, &size);
    CHECK(cpin && size == 9 && at_probe_is_fixed_command(cpin, 9));
    CHECK(!at_probe_is_fixed_command(cpin, 8));
    CHECK(!at_probe_is_fixed_command(lookalike, 9)); /* Same bytes, not from the table. */
    CHECK(!at_probe_is_fixed_command(NULL, 0));
    const unsigned sizes[] = {3, 4, 9, 7, 9, 10};
    for (int command = AT_PROBE_COMMAND_AT; command <= AT_PROBE_COMMAND_CEREG; command++) {
        const uint8_t *bytes = at_probe_command_bytes((enum at_probe_command)command, &size);
        CHECK(bytes && size == sizes[command] && at_probe_is_fixed_command(bytes, size));
    }
    /* Status commands cannot be driven through the single-probe path. */
    struct at_probe_result r;
    struct fake f = {0};
    const struct at_probe_backend backend = {&f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    CHECK(at_probe_run_command(&backend, AT_PROBE_COMMAND_CSQ, &r, NULL) == DJI_SYSTEM_ERROR);
    CHECK(f.prepares == 0 && f.writes == 0);
}

/* Signal query: AT+CSQ then AT+CESQ, numbers kept even with no stream. */
static void signal_checks(void) {
    const struct at_probe_backend backend = {NULL, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    struct at_probe_backend b = backend;
    struct status_probe_result r;
    struct fake f = {0};
    b.context = &f;
    ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CESQ\r\r\n+CESQ: 99,99,255,255,19,46\r\n\r\nOK\r\n");
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(f.writes == 2 && r.commands_sent == 2 && r.stop == STATUS_STOP_NONE && r.stop_index == STATUS_PROBE_SLOTS);
    CHECK(f.written_size[0] == 7 && memcmp(f.written[0], "AT+CSQ\r", 7) == 0);
    CHECK(f.written_size[1] == 8 && memcmp(f.written[1], "AT+CESQ\r", 8) == 0);
    const struct status_command_result *csq = &r.commands[1], *cesq = &r.commands[STATUS_PROBE_CESQ_INDEX];
    CHECK(csq->complete && csq->result == STATUS_RESULT_OK && csq->attribution == STATUS_ATTRIBUTION_MATCHED);
    CHECK(csq->values == 2 && csq->value[0] == 20 && csq->value[1] == 99);
    CHECK(cesq->complete && cesq->result == STATUS_RESULT_OK && cesq->attribution == STATUS_ATTRIBUTION_MATCHED);
    CHECK(cesq->values == 6 && cesq->value[4] == 19 && cesq->value[5] == 46 && cesq->value[2] == 255);
    CHECK(csq->value_lines == 1 && cesq->value_lines == 1);
    CHECK(f.closes == 1 && f.releases == 1 && !f.active);

    /* With a stream: fixed fields only. */
    f = (struct fake){0}; b.context = &f;
    ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CESQ\r\r\n+CESQ: 99,99,255,255,19,46\r\n\r\nOK\r\n");
    FILE *stream;
    capture_begin(&stream);
    CHECK(signal_probe_run(&b, &r, stream) == DJI_OK);
    capture_end(stream);
    HAS("status cmd=cesq line=2 rxlev=99 ber=99 rscp=255 ecno=255 rsrq=19 rsrp=46\n");
    HAS("status cmd=cesq attribution=matched reason=format-only\n");
    HAS("probe status=0 commands_sent=2 stop=none stop_index=7 ");

    /* CESQ unsupported: CSQ numbers stay, CESQ has none, run stops on the final error. */
    f = (struct fake){0}; b.context = &f;
    ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CESQ\r\r\nERROR\r\n");
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_FINAL_ERROR && r.stop_index == STATUS_PROBE_CESQ_INDEX);
    CHECK(r.commands[1].values == 2 && r.commands[STATUS_PROBE_CESQ_INDEX].values == 0 &&
          r.commands[STATUS_PROBE_CESQ_INDEX].result == STATUS_RESULT_ERROR);

    /* Malformed CESQ (five fields, seven fields, 4 digits): never kept, never printed. */
    static const char *const bad[] = {"AT+CESQ\r\r\n+CESQ: 99,99,255,255,19\r\n\r\nOK\r\n",
        "AT+CESQ\r\r\n+CESQ: 99,99,255,255,19,46,1\r\n\r\nOK\r\n",
        "AT+CESQ\r\r\n+CESQ: 99,99,255,255,19,4600\r\n\r\nOK\r\n"};
    for (unsigned k = 0; k < 3; k++) {
        f = (struct fake){0}; b.context = &f;
        ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
        add_reply(&f, 1, bad[k], strlen(bad[k]));
        capture_begin(&stream);
        CHECK(signal_probe_run(&b, &r, stream) == DJI_OK);
        capture_end(stream);
        CHECK(r.commands[STATUS_PROBE_CESQ_INDEX].values == 0);
        HAS("status cmd=cesq line=2 redacted=1 length=");
        LACKS("4600");
    }

    /* Two value lines in one confirmed reply: counted, so the caller can refuse the numbers. */
    f = (struct fake){0}; b.context = &f;
    ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n+CSQ: garbage\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CESQ\r\r\n+CESQ: 99,99,255,255,19,46\r\n\r\nOK\r\n");
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(r.commands[1].value_lines == 2 && r.commands[STATUS_PROBE_CESQ_INDEX].value_lines == 1);

    /* CSQ timeout: CESQ never sent, nothing kept. */
    f = (struct fake){0}; b.context = &f;
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_TIMEOUT && r.commands[1].values == 0 &&
          r.commands[STATUS_PROBE_CESQ_INDEX].sent == 0);
    CHECK(signal_probe_run(NULL, &r, NULL) == DJI_SYSTEM_ERROR);

    /* Modem still in PPP data mode: four reads of HDLC frames, never our echo.
     * The facts the pre-dial data-mode check uses: read-cap on CSQ, own echo 0,
     * non-text bytes counted over every read. */
    static const char frame[] = "\x7e\xff\x7d\x23\xc0\x21\x7d\x29\x7d\x21\x7d\x20\x7d\x28\x12\x34\x7e\r\n";
    f = (struct fake){0}; b.context = &f;
    for (unsigned k = 0; k < AT_PROBE_READ_LIMIT; k++) add_reply(&f, 0, frame, sizeof(frame) - 1);
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(f.writes == 1 && r.commands_sent == 1 && r.stop == STATUS_STOP_READ_CAP && r.stop_index == 1);
    CHECK(r.commands[1].sent == 1 && r.commands[1].reads == AT_PROBE_READ_LIMIT && r.commands[1].own_echoes == 0);
    CHECK(r.commands[1].binary == 3 * AT_PROBE_READ_LIMIT && r.commands[STATUS_PROBE_CESQ_INDEX].sent == 0);
    /* Leftover AT text without our echo is not binary: the check stays off. */
    f = (struct fake){0}; b.context = &f;
    for (unsigned k = 0; k < AT_PROBE_READ_LIMIT; k++) ADD(&f, 0, "+CSQ: 20,99\r\n");
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_READ_CAP && r.commands[1].binary == 0 && r.commands[1].own_echoes == 0);
    /* A late own echo after data bytes is counted, so it is never taken for data mode. */
    f = (struct fake){0}; b.context = &f;
    add_reply(&f, 0, frame, sizeof(frame) - 1);
    ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n");
    ADD(&f, 0, "\r\n");
    ADD(&f, 0, "\r\n");
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_READ_CAP && r.commands[1].own_echoes == 1 && r.commands[1].binary == 3);
    /* A zero-length read after data bytes stops the run as zero-count; the counts stay. */
    f = (struct fake){0}; b.context = &f;
    add_reply(&f, 0, frame, sizeof(frame) - 1);
    add_reply(&f, 0, "", 0);
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_ZERO_COUNT && r.stop_index == 1 && r.commands[1].reads == 1 &&
          r.commands[1].binary == 3 && r.commands[1].own_echoes == 0);
    /* A confirmed reply has one echo and no binary bytes. */
    f = (struct fake){0}; b.context = &f;
    ADD(&f, 0, "AT+CSQ\r\r\n+CSQ: 20,99\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CESQ\r\r\n+CESQ: 99,99,255,255,19,46\r\n\r\nOK\r\n");
    CHECK(signal_probe_run(&b, &r, NULL) == DJI_OK);
    CHECK(r.commands[1].own_echoes == 1 && r.commands[1].binary == 0 &&
          r.commands[STATUS_PROBE_CESQ_INDEX].own_echoes == 1);
}

int main(void) {
    struct fake f;
    struct status_probe_result r;
    parser_checks();
    table_checks();
    signal_checks();

    /* Normal: all four with echo, each ends early on its final line (no extra reads). */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CSQ: 18,99\r\n\r\nOK\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\n+COPS: 0,0,\"SKTelecom\",7\r\n\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\n+CEREG: 2,1,\"1A2B\",\"01C3D4E5\",7\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 4 && r.commands_sent == 4 && f.closes == 1);
    for (unsigned i = 0; i < 4; i++) CHECK(f.reads_per_command[i] == 1 && r.commands[i].result == STATUS_RESULT_OK);
    HAS("cpin=\"READY\""); HAS("rssi=18 ber=99"); HAS("oper=\"SKTelecom\" act=7"); HAS("n=2 stat=1 extra_fields=3");
    HAS("status cmd=cpin sent=1 reads=1 result=ok\n"); HAS("status cmd=cereg sent=1 reads=1 result=ok\n");
    HAS("probe status=0 commands_sent=4 stop=none stop_index=7 evidence=format-only protocol=unconfirmed\n");
    LACKS("1A2B"); LACKS("01C3D4E5");
    for (unsigned i = 0; i < 4; i++) VERDICT(r.commands[i], STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY);
    HAS("status cmd=cpin attribution=matched reason=format-only\n");
    HAS("status cmd=cereg attribution=matched reason=format-only\n");
    /* Order in output follows the fixed send order. */
    {
        const char *w0 = strstr(output, "status cmd=cpin out=0x03 in=0x84 write=ok bytes=9\n");
        const char *w1 = strstr(output, "status cmd=csq out=0x03 in=0x84 write=ok bytes=7\n");
        const char *w2 = strstr(output, "status cmd=cops out=0x03 in=0x84 write=ok bytes=9\n");
        const char *w3 = strstr(output, "status cmd=cereg out=0x03 in=0x84 write=ok bytes=10\n");
        CHECK(w0 && w1 && w2 && w3 && w0 < w1 && w1 < w2 && w2 < w3);
    }

    /* Without echo, split across reads. Lines before an own echo are skipped unread, so the
     * slot waits for the echo, times out and the run ends: no later write, no later read. */
    f = (struct fake){0};
    ADD(&f, 0, "\r\n+CME ERROR: SIM not ");
    ADD(&f, 0, "inserted\r\n");
    ADD(&f, 1, "\r\n+CSQ: 99,99\r\n");
    ADD(&f, 1, "\r\n+QIND: \"csq\",5\r\n\r\nOK\r\n");
    ADD(&f, 2, "\r\n+COPS: 0\r\n\r\nOK\r\n");
    ADD(&f, 3, "\r\n+CEREG: 0,0\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads_per_command[0] == 3 && f.reads_per_command[1] == 0 && f.closes == 1);
    LACKS("SIM not inserted"); LACKS("cme="); HAS("status cmd=cpin sent=1 reads=2 result=none\n");
    HAS("status cmd=cpin read=3 status=timeout bytes=0\n"); HAS("status cmd=cpin pre_echo_skipped=1\n");
    LACKS("rssi="); LACKS("QIND"); LACKS("echo=1");
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_ECHO);
    CHECK(r.commands[1].sent == 0 && r.commands[2].sent == 0 && r.commands[3].sent == 0);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_TIMEOUT && r.stop_index == 0);
    HAS("stop=timeout stop_index=0"); HAS("status cmd=csq sent=0 reads=0 result=none\n");
    LACKS("attribution=matched"); LACKS("mode=0"); LACKS("n=0 stat=0");

    /* Echo split across reads is joined before analysis. */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CP"); ADD(&f, 0, "IN?\r\r\n+CPIN: RE"); ADD(&f, 0, "ADY\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\nOK\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads_per_command[0] == 3 && f.writes == 4 && r.stop == STATUS_STOP_NONE);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY);
    HAS("status cmd=cpin line=1 echo=1\n"); HAS("cpin=\"READY\"");

    /* Every reply shifted one window late: the first slot never sees a final after its echo,
     * times out, and the run ends there. */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n");
    ADD(&f, 1, "\r\n+CPIN: READY\r\n\r\nOK\r\nAT+CSQ\r\r\n");
    ADD(&f, 2, "\r\n+CSQ: 20,99\r\n\r\nOK\r\nAT+COPS?\r\r\n");
    ADD(&f, 3, "\r\n+COPS: 0\r\n\r\nOK\r\nAT+CEREG?\r\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 2);
    CHECK(f.reads_per_command[1] == 0 && f.reads_per_command[2] == 0 && f.reads_per_command[3] == 0);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_NO_FINAL);
    CHECK(r.commands[1].sent == 0 && r.commands[2].sent == 0 && r.commands[3].sent == 0);
    CHECK(r.stop == STATUS_STOP_TIMEOUT && r.stop_index == 0 && f.closes == 1);
    LACKS("attribution=matched"); LACKS("cpin=\"READY\""); LACKS("rssi="); LACKS("mode=");
    HAS("status cmd=csq sent=0 reads=0 result=none\n");
    HAS("stop=timeout stop_index=0");

    /* A full 2048-byte buffer (4 x 512) without a final line: partial, never truncated. */
    {
        static char block[512], first[512];
        memset(block, 'x', sizeof(block));
        memset(first, 'x', sizeof(first));
        memcpy(first, "AT+CPIN?\r\r\n", 11);
        f = (struct fake){0};
        add_reply(&f, 0, first, sizeof(first));
        for (unsigned i = 0; i < 3; i++) add_reply(&f, 0, block, sizeof(block));
        CHECK(run(&f, &r) == DJI_OK);
        CHECK(f.reads_per_command[0] == AT_PROBE_READ_LIMIT && f.writes == 1);
        CHECK(f.reads_per_command[1] == 0 && r.stop == STATUS_STOP_READ_CAP && r.stop_index == 0);
        VERDICT(r.commands[0], STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_PARTIAL);
        HAS("status cmd=cpin line=2 partial=1 length=2037\n"); LACKS("truncated=1"); LACKS("xxxx");
        HAS("stop=read-cap stop_index=0");
    }
    /* Disconnect after a reply without a final line: downgraded to io-error, no later command. */
    f = (struct fake){.read_status = DJI_GONE};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n");
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 2 && f.closes == 1);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_IO_ERROR);
    HAS("status cmd=cpin attribution=unproven reason=io-error\n");
    /* Cancelled after a read: that read is never joined; io-error. */
    f = (struct fake){.cancel_at_read = 2};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n");
    ADD(&f, 0, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 2 && r.commands[0].reads == 1);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_IO_ERROR);
    CHECK(r.commands[0].result == STATUS_RESULT_NONE); LACKS("result=ok");
    HAS("status cmd=cpin attribution=unproven reason=io-error\n");

    /* Timeout without final line: next command still sent, result none. */
    f = (struct fake){0};
    ADD(&f, 0, "\r\n+CPIN: READY\r\n");
    ADD(&f, 2, "\r\n+COPS: 0,2,\"45005\",7\r\n\r\nOK\r\n");
    ADD(&f, 3, "\r\nERROR\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 2 && f.reads_per_command[1] == 0);
    CHECK(r.stop == STATUS_STOP_TIMEOUT && r.stop_index == 0);
    HAS("status cmd=cpin sent=1 reads=1 result=none\n"); HAS("status cmd=csq sent=0 reads=0 result=none\n");
    LACKS("45005"); HAS("stop=timeout stop_index=0");

    /* Four-read cap and deadline per command. */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n");
    for (unsigned i = 0; i < 5; i++) ADD(&f, 0, "\r\n+X: 1\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads_per_command[0] == AT_PROBE_READ_LIMIT && f.writes == 1);
    CHECK(r.stop == STATUS_STOP_READ_CAP && r.commands[1].sent == 0);
    f = (struct fake){.read_delay = 1000};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    for (unsigned i = 0; i < 4; i++) ADD(&f, 1, "\r\n+X: 1\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads_per_command[0] == 1 && f.reads_per_command[1] == 2 && f.writes == 2);
    CHECK(r.stop == STATUS_STOP_TIME_BUDGET && r.stop_index == 1);
    HAS("stop=time-budget stop_index=1"); HAS("status cmd=cops sent=0 reads=0 result=none\n");

    /* Second write fails: 3rd/4th never sent, no reads after, closed. */
    int write_errors[] = {AT_PROBE_TIMEOUT, DJI_GONE};
    for (size_t i = 0; i < 2; i++) {
        f = (struct fake){.write_fail_at = 2, .write_status = write_errors[i]};
        ADD(&f, 0, "AT+CPIN?\r\r\nOK\r\n");
        ADD(&f, 1, "AT+CSQ\r\r\nOK\r\n");
        int status = run(&f, &r);
        CHECK(status == (i == 0 ? DJI_SYSTEM_ERROR : DJI_GONE));
        CHECK(f.writes == 2 && f.reads_per_command[1] == 0 && f.reads_per_command[2] == 0 && f.closes == 1);
        CHECK(r.commands_sent == 1 && r.commands[1].sent == 0 && r.commands[2].sent == 0);
        HAS("cmd=csq write="); HAS("bytes=0 delivery=unknown");
        HAS("status cmd=cops sent=0 reads=0 result=none\n"); HAS("commands_sent=1 stop=write-error stop_index=1");
    }
    f = (struct fake){.write_fail_at = 1, .write_status = AT_PROBE_TIMEOUT, .close_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    CHECK(f.writes == 1 && f.reads == 0);

    /* Cancellation between commands: later sends 0. */
    f = (struct fake){.cancel_after_writes = 2};
    ADD(&f, 0, "AT+CPIN?\r\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 2 && f.closes == 1 && r.commands_sent == 2);
    f = (struct fake){.cancel_at_read = 1};
    ADD(&f, 0, "\r\n+CPIN: READY\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 1 && f.closes == 1);
    f = (struct fake){.cancel_after_writes = 1, .close_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    f = (struct fake){.close_status = DJI_SYSTEM_ERROR};
    ADD(&f, 0, "AT+CPIN?\r\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\nOK\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    CHECK(f.writes == 4);
    HAS("probe status=12 commands_sent=4 stop=none");
    f = (struct fake){.open_status = DJI_BUSY};
    CHECK(run(&f, &r) == DJI_BUSY);
    CHECK(f.writes == 0 && f.closes == 0 && f.releases == 1);
    /* A failure before the first slot is never reported as "all slots done". */
    CHECK(r.stop == STATUS_STOP_IO_ERROR && r.stop_index == 0);
    HAS("commands_sent=0 stop=io-error stop_index=0"); LACKS("stop=none");
    f = (struct fake){.read_status = DJI_GONE};
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 1 && f.closes == 1);

    /* Stale OK before the own echo does not end the slot; the own reply still matches. */
    f = (struct fake){0};
    ADD(&f, 0, "\r\nOK\r\n");
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CSQ: 18,99\r\n\r\nOK\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\n+COPS: 0\r\n\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\n+CEREG: 0,1\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 4 && f.reads_per_command[0] == 2 && r.stop == STATUS_STOP_NONE);
    CHECK(r.commands[0].pre_echo_skipped == 1 && r.commands[0].complete == 1);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY);
    HAS("cpin=\"READY\""); HAS("status cmd=cpin pre_echo_skipped=1\n"); LACKS("line=1 result=ok");
    /* The direct parser keeps the strict meaning for the same bytes. */
    {
        struct status_command_result direct = PARSE(0, "\r\nOK\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
        VERDICT(direct, STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FINAL_BEFORE_ECHO);
        CHECK(direct.pre_echo_skipped == 0); LACKS("cpin=\"READY\"");
    }

    /* Own prefix before the own echo is not promoted by skipping: mismatch, later slots blocked. */
    f = (struct fake){0};
    ADD(&f, 0, "\r\n+CPIN: SIM PIN\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CSQ: 18,99\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads_per_command[1] == 0 && r.stop == STATUS_STOP_UNCONFIRMED);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_STALE_BEFORE_ECHO);
    CHECK(r.commands[0].pre_echo_skipped == 1 && r.commands[0].complete == 1);
    LACKS("SIM PIN"); LACKS("READY"); LACKS("rssi="); HAS("status cmd=csq sent=0 reads=0 result=none\n");

    /* Duplicate own echo and a foreign line after the own echo stay conservative. */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_UNCONFIRMED);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_DUP_ECHO); LACKS("READY");
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CEREG: 0,1\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_UNCONFIRMED);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_MISMATCH, STATUS_REASON_FOREIGN_PREFIX); LACKS("READY");

    /* Foreign line before the echo is discarded unread; a foreign line after it is not. */
    f = (struct fake){0};
    ADD(&f, 0, "\r\n+QIND: RDY\r\nAT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CSQ: 18,99\r\n\r\nOK\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\n+COPS: 0\r\n\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\n+CEREG: 0,1\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 4 && r.commands[0].pre_echo_skipped == 1 && r.stop == STATUS_STOP_NONE);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_MATCHED, STATUS_REASON_FORMAT_ONLY);
    HAS("cpin=\"READY\""); LACKS("QIND"); LACKS("unsolicited=1");
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n+QIND: RDY\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_UNCONFIRMED);
    VERDICT(r.commands[0], STATUS_ATTRIBUTION_UNPROVEN, STATUS_REASON_UNLISTED_PREFIX);
    HAS("unsolicited=1 length=10"); LACKS("QIND");

    /* Matched ERROR and CME stop the run; values already filtered. */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\nERROR\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CSQ: 18,99\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_FINAL_ERROR && r.stop_index == 0);
    HAS("stop=final-error stop_index=0"); HAS("status cmd=csq sent=0 reads=0 result=none\n");
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CME ERROR: 10\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_FINAL_ERROR); HAS("cme=10\n");

    /* Zero count, oversize, read error and cancellation: no later slot, closed once. */
    f = (struct fake){0};
    add_reply(&f, 0, "", 0);
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 1 && r.commands[1].sent == 0);
    CHECK(r.stop == STATUS_STOP_ZERO_COUNT && f.closes == 1); HAS("stop=zero-count stop_index=0");
    f = (struct fake){.read_status = DJI_GONE};
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 1 && f.reads == 1 && r.commands[1].sent == 0 && f.closes == 1);
    CHECK(r.stop == STATUS_STOP_IO_ERROR); HAS("stop=io-error stop_index=0");
    f = (struct fake){.cancel_at_read = 1};
    ADD(&f, 0, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 1 && r.commands[1].sent == 0 && f.closes == 1);
    CHECK(r.stop == STATUS_STOP_CANCELLED); HAS("stop=cancelled stop_index=0");

    /* Clock failure before the first send, between slots, and mid-read: nothing further sent. */
    f = (struct fake){.clock_bad_at = 1};
    ADD(&f, 0, "AT+CPIN?\r\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    HAS("stop=clock-error"); HAS("status cmd=cpin sent=0 reads=0 result=none\n");
    /* A real backward step (not the UINT64_MAX sentinel) at each check point.
     * now_ms calls: 1 start, 2 before send, 3 after send, 4 before read, 5 before the next send. */
    f = (struct fake){.now = 1000, .clock_back = 1};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR && r.stop_index == 0);
    f = (struct fake){.now = 1000, .clock_back = 2};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR && r.stop_index == 0);
    f = (struct fake){.now = 1000, .clock_back = 4};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 1 && f.reads_per_command[1] == 0 && f.closes == 1);
    CHECK(r.stop == STATUS_STOP_CLOCK_ERROR && r.stop_index == 1 && r.commands[0].attribution == STATUS_ATTRIBUTION_MATCHED);
    HAS("stop=clock-error stop_index=1"); HAS("status cmd=csq sent=0 reads=0 result=none\n");
    f = (struct fake){.clock_bad_at = 3};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads_per_command[0] == 0 && r.stop == STATUS_STOP_CLOCK_ERROR && f.closes == 1);
    f = (struct fake){.clock_overflow = 1};
    ADD(&f, 0, "AT+CPIN?\r\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads == 0 && r.stop == STATUS_STOP_CLOCK_ERROR && f.closes == 1);

    /* Privacy across a full run. Each slot must be confirmed or the run stops, so later
     * slots' private text is never read; the first slot's text is still redacted. */
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CME ERROR: operation not allowed because of reason 490154203237518\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CSQ: 12345678,99\r\n\r\nOK\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\n+COPS: 0,0,\"Net 89860012345678901234\",7\r\n\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\n+CEREG: 2,5,\"BEEF\",\"0DEADC0D\",7\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_FINAL_ERROR);
    LACKS("490154"); LACKS("12345678"); LACKS("8986"); LACKS("BEEF"); LACKS("DEADC0D"); LACKS("operation");
    HAS("cme=redacted"); LACKS("stat=5");
    f = (struct fake){0};
    ADD(&f, 0, "AT+CPIN?\r\r\n+CPIN: 123456\r\n\r\n+CME ERROR: 1A2B 01A2B3C4\r\n");
    ADD(&f, 1, "AT+CSQ\r\r\n+CME ERROR: SIM 1 failure\r\n");
    ADD(&f, 2, "AT+COPS?\r\r\n+COPS: 0,0,\"123456-654321\",7\r\n\r\nOK\r\n");
    ADD(&f, 3, "AT+CEREG?\r\r\n+CME ERROR: 1000\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && r.stop == STATUS_STOP_FINAL_ERROR);
    LACKS("123456"); LACKS("654321"); LACKS("1A2B"); LACKS("01A2B3C4"); LACKS("SIM 1"); LACKS("1000");
    CHECK(r.commands[0].result == STATUS_RESULT_CME_ERROR && r.commands[3].sent == 0);

    CHECK(status_probe_run(NULL, &r, NULL) == DJI_SYSTEM_ERROR);

    printf("status probe checks=%u failures=%u (fake transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
