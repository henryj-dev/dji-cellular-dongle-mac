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
    unsigned prepares, opens, writes, reads, closes, releases;
    int active, prepared;
    int open_status, close_status, read_status, write_status;
    int cancel_before_write, cancel_at_read;
    uint8_t written[16];
    uint32_t written_size;
    unsigned reads_before_write;
    int clock_bad_at, clock_back;
    unsigned now_calls;
    unsigned reply_count;
    const uint8_t *replies[MAX_REPLIES];
    uint32_t reply_sizes[MAX_REPLIES];
};

static uint64_t fake_now(void *p) {
    struct fake *f = p;
    f->now_calls++;
    if (f->clock_bad_at && f->now_calls == (unsigned)f->clock_bad_at) return AT_PROBE_CLOCK_ERROR;
    if (f->clock_back && f->now_calls > (unsigned)f->clock_back) return f->now - 1;
    return f->now;
}
static int fake_cancelled(void *p) {
    struct fake *f = p;
    if (f->cancel_before_write && f->opens > 0 && f->writes == 0) return 1;
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
    CHECK(at_probe_is_fixed_command(bytes, size));
    if (f->writes == 0 && size <= sizeof(f->written)) { memcpy(f->written, bytes, size); f->written_size = size; }
    f->writes++;
    return f->write_status;
}
static int fake_read(void *p, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active && pipe == 3);
    CHECK(*size == AT_PROBE_READ_SIZE && timeout > 0 && timeout <= AT_PROBE_READ_TOTAL_MS);
    if (f->writes == 0) f->reads_before_write++;
    unsigned index = f->reads++;
    memset(buffer, '8', *size); /* Error buffers hold digits that must never surface. */
    if (index >= f->reply_count) {
        f->now += timeout;
        *size = UINT32_MAX;
        return f->read_status ? f->read_status : AT_PROBE_TIMEOUT;
    }
    if (f->reply_sizes[index] == 0) { *size = 0; return DJI_OK; }
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
#define HAS(text) CHECK(strstr(output, (text)) != NULL)
#define LACKS(text) CHECK(strstr(output, (text)) == NULL)

static int run(struct fake *f, struct status_probe_result *r) {
    const struct at_probe_backend backend = {f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    FILE *stream;
    capture_begin(&stream);
    int status = pdp_probe_run(&backend, r, stream);
    capture_end(stream);
    CHECK(!f->active && f->writes <= 1 && f->reads <= AT_PROBE_READ_LIMIT && f->reads_before_write == 0);
    CHECK(f->releases == f->prepares);
    if (f->writes == 1) CHECK(f->written_size == 12 && memcmp(f->written, "AT+CGDCONT?\r", 12) == 0);
    CHECK(strstr(output, "evidence=format-only protocol=unconfirmed") != NULL);
    /* Only the cgdcont slot is ever reported by this mode. */
    LACKS("cmd=cpin"); LACKS("cmd=csq"); LACKS("cmd=cops"); LACKS("cmd=cereg");
    LACKS("8888");
    return status;
}

static struct status_command_result parse(const char *bytes, size_t size) {
    struct status_command_result command;
    memset(&command, 0, sizeof(command));
    FILE *stream;
    capture_begin(&stream);
    status_print_lines(STATUS_PROBE_PDP_INDEX, (const uint8_t *)bytes, size, stream, &command);
    capture_end(stream);
    return command;
}
#define PARSE(literal) parse((literal), sizeof(literal) - 1)

static void parser_checks(void) {
    struct status_command_result c;
    c = PARSE("AT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\",\"0.0.0.0\",0,0\r\n\r\nOK\r\n");
    HAS("pdp line=1 echo=1\n");
    HAS("pdp line=2 cid=1 type=\"IP\" apn=\"internet\" addr_present=1 addr_length=7 extra_fields=2\n");
    HAS("pdp line=3 result=ok\n");
    CHECK(c.contexts == 1 && c.result == STATUS_RESULT_OK);
    LACKS("0.0.0.0");
    CHECK(c.attribution == STATUS_ATTRIBUTION_MATCHED && c.attribution_reason == STATUS_REASON_FORMAT_ONLY);
    HAS("pdp attribution=matched reason=format-only\n");

    /* Late CEREG reply and a stale CGDCONT before the own echo: mismatch, values withheld. */
    c = PARSE("\r\n+CEREG: 0,1\r\n\r\nOK\r\nAT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\",\"10.1.2.3\"\r\n\r\nOK\r\n");
    CHECK(c.contexts == 0 && c.attribution == STATUS_ATTRIBUTION_MISMATCH &&
          c.attribution_reason == STATUS_REASON_FINAL_BEFORE_ECHO);
    HAS("pdp line=1 unsolicited=1 length=11\n"); HAS("pdp line=4 withheld=1 length=38\n");
    HAS("pdp attribution=mismatch reason=final-before-echo\n");
    LACKS("cid="); LACKS("internet"); LACKS("10.1.2.3");
    c = PARSE("AT+CGDCONT?\r\r\n+CEREG: 0,1\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n\r\nOK\r\n");
    CHECK(c.contexts == 0 && c.attribution_reason == STATUS_REASON_FOREIGN_PREFIX);
    HAS("pdp line=3 withheld=1 length=27\n"); LACKS("internet");
    c = PARSE("\r\n+CGDCONT: 1,\"IP\",\"old\"\r\nAT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n\r\nOK\r\n");
    CHECK(c.contexts == 0 && c.attribution_reason == STATUS_REASON_STALE_BEFORE_ECHO);
    LACKS("old"); LACKS("internet");
    /* Without echo the filtered context still prints; it is only not attributed. */
    c = PARSE("\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n\r\nOK\r\n");
    CHECK(c.contexts == 1 && c.attribution == STATUS_ATTRIBUTION_UNPROVEN &&
          c.attribution_reason == STATUS_REASON_NO_ECHO);
    HAS("cid=1 type=\"IP\" apn=\"internet\""); HAS("pdp attribution=unproven reason=no-echo\n");

    c = PARSE("\r\n+CGDCONT: 1,\"IPV4V6\",\"lte.ktfwing.com\",\"10.123.45.67\",0,0,0,0\r\n"
              "+CGDCONT: 2,\"IPV6\",\"ims\",\"2001:db8::1\",0,0\r\n"
              "+CGDCONT: 3,\"IP\",\"\",\"\",0,0\r\n\r\nOK\r\n");
    CHECK(c.contexts == 3);
    HAS("pdp line=1 cid=1 type=\"IPV4V6\" apn=\"lte.ktfwing.com\" addr_present=1 addr_length=12 extra_fields=4\n");
    HAS("pdp line=2 cid=2 type=\"IPV6\" apn=\"ims\" addr_present=1 addr_length=11 extra_fields=2\n");
    HAS("pdp line=3 cid=3 type=\"IP\" apn=\"\" addr_present=0 addr_length=0 extra_fields=2\n");
    LACKS("10.123"); LACKS("45.67"); LACKS("2001"); LACKS("db8");

    /* Minimal three-field form, address absent. */
    c = PARSE("\r\n+CGDCONT: 1,\"PPP\",\"web.sktelecom.com\"\r\n");
    HAS("cid=1 type=\"PPP\" apn=\"web.sktelecom.com\" addr_present=0 addr_length=0 extra_fields=0\n");
    /* Many trailing fields, some quoted with commas inside. */
    c = PARSE("\r\n+CGDCONT: 11,\"IPV4V6\",\"apn_x-1\",\"\",0,0,0,0,0,0,\"a,b\",1\r\n");
    HAS("cid=11 type=\"IPV4V6\" apn=\"apn_x-1\" addr_present=0 addr_length=0 extra_fields=8\n");
    LACKS("a,b");
    /* Long IPv6 address: length only. */
    c = PARSE("\r\n+CGDCONT: 1,\"IPV6\",\"ims\",\"254.128.0.0.0.0.0.0.0.0.0.0.0.0.0.1.32.1.13.184.0.0.0.0.0.0.0.0.0.0.0.1\"\r\n");
    HAS("addr_present=1 addr_length=71 extra_fields=0\n"); LACKS("254.128"); LACKS("13.184");

    /* APN boundaries and redaction. */
    char apn[64], in[160], expect[200];
    memset(apn, 'a', 63);
    int len = snprintf(in, sizeof(in), "\r\n+CGDCONT: 1,\"IP\",\"%.*s\"\r\n", 63, apn);
    c = parse(in, (size_t)len);
    snprintf(expect, sizeof(expect), "apn=\"%.*s\" addr_present=0", 63, apn);
    HAS(expect);
    len = snprintf(in, sizeof(in), "\r\n+CGDCONT: 1,\"IP\",\"%.*sa\"\r\n", 63, apn);
    c = parse(in, (size_t)len);
    HAS("apn=redacted apn_length=64 addr_present=0"); LACKS("aaaaaaaa");
    const char *bad_apns[] = {"my apn", "user@net", "lg+u", "a1b2c3d4e5f6g7", "12345.67", "net:x", "a/b"};
    for (size_t i = 0; i < sizeof(bad_apns) / sizeof(bad_apns[0]); i++) {
        len = snprintf(in, sizeof(in), "\r\n+CGDCONT: 1,\"IP\",\"%s\",\"10.0.0.1\"\r\n", bad_apns[i]);
        c = parse(in, (size_t)len);
        snprintf(expect, sizeof(expect), "cid=1 type=\"IP\" apn=redacted apn_length=%zu addr_present=1 addr_length=8",
                 strlen(bad_apns[i]));
        HAS(expect); LACKS(bad_apns[i]); LACKS("10.0.0.1");
        CHECK(c.contexts == 1);
    }
    c = PARSE("\r\n+CGDCONT: 1,\"IP\",\"a1b2c3d4\"\r\n");
    HAS("apn=\"a1b2c3d4\""); /* four digits in total is allowed */
    c = PARSE("\r\n+CGDCONT: 1,\"IP\",\"a123456\"\r\n");
    HAS("apn=\"a123456\""); /* six digits stays below the limit */

    /* Broken structure: whole line redacted, never parsed further. */
    const char *bad_lines[] = {
        "\r\n+CGDCONT: 1,\"IP,\"internet\"\r\n",          /* quote pair mismatch */
        "\r\n+CGDCONT: 1,\"IP\",\"internet\r\n",          /* unterminated APN */
        "\r\n+CGDCONT: 1,\"IP\",\"internet\",\"10.1.2.3\r\n", /* unterminated address */
        "\r\n+CGDCONT: 123,\"IP\",\"internet\"\r\n",      /* cid 3 digits */
        "\r\n+CGDCONT: ,\"IP\",\"internet\"\r\n",          /* empty cid */
        "\r\n+CGDCONT: 1,IP,\"internet\"\r\n",             /* unquoted type */
        "\r\n+CGDCONT: 1,\"IP\",internet\r\n",             /* unquoted APN */
        "\r\n+CGDCONT: 1,\"IP\",\"internet\"x\r\n",        /* junk after APN */
        "\r\n+CGDCONT: 1,\"IP\",\"internet\",10.1.2.3\r\n", /* unquoted address */
        "\r\n+CGDCONT: 1,\"IP\",\"internet\",\"10.1.2.3\"x,0\r\n", /* junk after address */
        "\r\n+CGDCONT: 1,\"IP\",\"internet\",\"10.1.2.3\",\"0\r\n", /* unbalanced later quote */
        "\r\n+CGDCONT: 1\r\n"};
    for (size_t i = 0; i < sizeof(bad_lines) / sizeof(bad_lines[0]); i++) {
        c = parse(bad_lines[i], strlen(bad_lines[i]));
        HAS("pdp line=1 redacted=1 length="); LACKS("cid="); LACKS("internet"); LACKS("10.1.2.3");
        CHECK(c.contexts == 0);
    }

    /* Changed: lowercase, 11-char and empty types are no longer whole-line redactions;
     * an unlisted type prints as type=other type_length=L and the line keeps parsing. */
    struct { const char *line; const char *expect; } other_types[] = {
        {"\r\n+CGDCONT: 1,\"ip\",\"internet\"\r\n", "cid=1 type=other type_length=2 apn=\"internet\""},
        {"\r\n+CGDCONT: 1,\"IPV4V6XXXXX\",\"internet\"\r\n", "cid=1 type=other type_length=11 apn=\"internet\""},
        {"\r\n+CGDCONT: 1,\"\",\"internet\"\r\n", "cid=1 type=other type_length=0 apn=\"internet\""},
        {"\r\n+CGDCONT: 1,\"0A7B2D43\",\"ims\",\"10.1.2.3\"\r\n", "cid=1 type=other type_length=8 apn=\"ims\" addr_present=1 addr_length=8"},
        {"\r\n+CGDCONT: 2,\"FOO\",\"\"\r\n", "cid=2 type=other type_length=3 apn=\"\""},
        {"\r\n+CGDCONT: 2,\"ip v6\",\"x\"\r\n", "cid=2 type=other type_length=5 apn=\"x\""}};
    for (size_t i = 0; i < sizeof(other_types) / sizeof(other_types[0]); i++) {
        c = parse(other_types[i].line, strlen(other_types[i].line));
        HAS(other_types[i].expect); LACKS("0A7B2D43"); LACKS("FOO"); LACKS("XXXXX"); LACKS("\"ip"); LACKS("10.1.2.3");
        CHECK(c.contexts == 1);
    }
    const char *known_types[] = {"IP", "IPV6", "IPV4V6", "PPP", "X.25", "OSPIH", "NON-IP", "ETHERNET", "UNSTRUCTURED"};
    for (size_t i = 0; i < sizeof(known_types) / sizeof(known_types[0]); i++) {
        len = snprintf(in, sizeof(in), "\r\n+CGDCONT: 1,\"%s\",\"internet\"\r\n", known_types[i]);
        c = parse(in, (size_t)len);
        snprintf(expect, sizeof(expect), "cid=1 type=\"%s\" apn=\"internet\"", known_types[i]);
        HAS(expect); LACKS("type=other");
    }
    /* Address-shaped APNs are redacted; ordinary names (even with digits) still print. */
    const char *addr_apns[] = {"10.1.2.3", "1.2.3.4", "fe80::1", "2001:db8::1", "::1", "123.45", "0.0"};
    for (size_t i = 0; i < sizeof(addr_apns) / sizeof(addr_apns[0]); i++) {
        len = snprintf(in, sizeof(in), "\r\n+CGDCONT: 1,\"IP\",\"%s\"\r\n", addr_apns[i]);
        c = parse(in, (size_t)len);
        snprintf(expect, sizeof(expect), "apn=redacted apn_length=%zu addr_present=0", strlen(addr_apns[i]));
        HAS(expect); LACKS(addr_apns[i]);
    }
    const char *ok_apns[] = {"internet", "lte.ktfwing.com", "web.sktelecom.com", "ims", "", "apn1.example",
                             "abc.def", "cafe", "dead.beef"};
    for (size_t i = 0; i < sizeof(ok_apns) / sizeof(ok_apns[0]); i++) {
        len = snprintf(in, sizeof(in), "\r\n+CGDCONT: 1,\"IP\",\"%s\"\r\n", ok_apns[i]);
        c = parse(in, (size_t)len);
        snprintf(expect, sizeof(expect), "apn=\"%s\" addr_present=0", ok_apns[i]);
        HAS(expect);
    }

    /* Results, CME filter, unsolicited/other/partial lines. */
    c = PARSE("\r\nERROR\r\n");
    HAS("pdp line=1 result=error\n"); CHECK(c.result == STATUS_RESULT_ERROR);
    c = PARSE("\r\n+CME ERROR: SIM not inserted\r\n");
    HAS("result=cme-error cme=\"SIM not inserted\""); CHECK(c.result == STATUS_RESULT_CME_ERROR);
    c = PARSE("\r\n+CME ERROR: 10\r\n");
    HAS("cme=10\n");
    c = PARSE("\r\n+CME ERROR: apn 1A2B 0123\r\n");
    HAS("cme=redacted"); LACKS("1A2B");
    c = PARSE("\r\n+CEREG: 2,1,\"1A2B\",\"01C3D4E5\",7\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\nRDY\r\n\r\nOK\r\n");
    HAS("pdp line=1 unsolicited=1 length=31\n"); HAS("pdp line=2 withheld=1 length="); HAS("pdp line=3 other=1 length=3\n");
    CHECK(c.contexts == 0 && c.attribution == STATUS_ATTRIBUTION_MISMATCH);
    HAS("pdp attribution=mismatch reason=foreign-prefix\n"); LACKS("cid="); LACKS("internet");
    LACKS("1A2B"); LACKS("RDY");
    c = PARSE("\r\n+CGDCONT: 1,\"IP\",\"internet\",\"10.9");
    HAS("pdp line=1 partial=1 length="); LACKS("internet"); LACKS("10.9");
    CHECK(c.contexts == 0);

    /* 16-line cap. */
    char many[2048];
    size_t used = 0;
    for (unsigned i = 0; i < 20; i++)
        used += (size_t)snprintf(many + used, sizeof(many) - used, "+CGDCONT: %u,\"IP\",\"a\"\r\n", i + 1);
    used += (size_t)snprintf(many + used, sizeof(many) - used, "OK\r\n");
    c = parse(many, used);
    CHECK(c.lines == STATUS_PROBE_MAX_LINES && c.omitted == 5 && c.contexts == 16 && c.result == STATUS_RESULT_OK);
    LACKS("pdp line=17");
}

int main(void) {
    struct fake f;
    struct status_probe_result r;
    parser_checks();

    {
        uint32_t size = 0;
        const uint8_t *cgdcont = at_probe_command_bytes(AT_PROBE_COMMAND_CGDCONT, &size);
        static const uint8_t lookalike[12] = {'A', 'T', '+', 'C', 'G', 'D', 'C', 'O', 'N', 'T', '?', '\r'};
        CHECK(cgdcont && size == 12 && at_probe_is_fixed_command(cgdcont, 12));
        CHECK(!at_probe_is_fixed_command(lookalike, 12) && !at_probe_is_fixed_command(cgdcont, 11));
        struct at_probe_result single;
        f = (struct fake){0};
        const struct at_probe_backend backend = {&f, fake_now, fake_cancelled, fake_prepare, fake_validate,
            fake_open, fake_write, fake_read, fake_close, fake_release};
        CHECK(at_probe_run_command(&backend, AT_PROBE_COMMAND_CGDCONT, &single, NULL) == DJI_SYSTEM_ERROR);
        CHECK(f.prepares == 0 && f.writes == 0);
    }

    /* Normal run with echo, early stop on final line. */
    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\n+CGDCONT: 1,\"IPV4V6\",\"internet\",\"10.123.45.67\",0,0\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads == 1 && r.commands_sent == 1 && f.closes == 1);
    HAS("status cmd=cgdcont out=0x03 in=0x84 write=ok bytes=12\n");
    HAS("pdp line=2 cid=1 type=\"IPV4V6\" apn=\"internet\" addr_present=1 addr_length=12 extra_fields=2\n");
    HAS("status cmd=cgdcont sent=1 reads=1 result=ok contexts=1\n");
    HAS("probe status=0 commands_sent=1 stop=none stop_index=6 evidence=format-only protocol=unconfirmed\n");
    LACKS("10.123"); LACKS("45.67");
    HAS("pdp attribution=matched reason=format-only\n");

    /* Shifted CEREG reply in the PDP window: summary format kept, contexts not counted. */
    f = (struct fake){0};
    ADD(&f, "\r\n+CEREG: 0,1\r\n\r\nOK\r\nAT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads == 1);
    /* The run path skips the earlier reply unread; only the own context counts. */
    CHECK(r.commands[STATUS_PROBE_PDP_INDEX].attribution == STATUS_ATTRIBUTION_MATCHED);
    CHECK(r.commands[STATUS_PROBE_PDP_INDEX].contexts == 1);
    CHECK(r.commands[STATUS_PROBE_PDP_INDEX].pre_echo_skipped == 2 && r.stop == STATUS_STOP_NONE);
    HAS("status cmd=cgdcont sent=1 reads=1 result=ok contexts=1\n");
    HAS("pdp attribution=matched reason=format-only\n"); HAS("status cmd=cgdcont pre_echo_skipped=2\n");
    HAS("cid=1 type=\"IP\" apn=\"internet\""); LACKS("unsolicited=1"); LACKS("10.1.2.3");
    /* Same bytes through the direct parser stay strict and withhold the context. */
    {
        struct status_command_result direct =
            PARSE("\r\n+CEREG: 0,1\r\n\r\nOK\r\nAT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n\r\nOK\r\n");
        CHECK(direct.attribution == STATUS_ATTRIBUTION_MISMATCH && direct.contexts == 0);
        HAS("withheld=1 length=27\n"); LACKS("apn=\"internet\"");
    }
    /* Disconnect after a context line: io-error, context still filtered and counted. */
    f = (struct fake){.read_status = DJI_GONE};
    ADD(&f, "AT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n");
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 1 && f.reads == 2);
    CHECK(r.commands[STATUS_PROBE_PDP_INDEX].attribution_reason == STATUS_REASON_IO_ERROR);
    HAS("pdp attribution=unproven reason=io-error\n"); HAS("contexts=1\n");

    /* Split across reads, unsolicited line mixed in. The own echo is required or the run stops. */
    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"inter");
    ADD(&f, "net\",\"\",0,0\r\n+QIND: SMS DONE\r\n+CGDCONT: 2,\"IPV6\",\"ims\",\"2001:db8::1\"\r\n");
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 3 && r.stop == STATUS_STOP_UNCONFIRMED);
    HAS("cid=1 type=\"IP\" apn=\"internet\" addr_present=0"); HAS("unsolicited=1 length=15");
    HAS("cid=2 type=\"IPV6\" apn=\"ims\" addr_present=1 addr_length=11");
    HAS("status cmd=cgdcont sent=1 reads=3 result=ok contexts=2\n");
    HAS("echo=1"); LACKS("QIND"); LACKS("2001");

    /* Timeout without reply: result none, contexts 0, run stopped. */
    f = (struct fake){0};
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 1 && r.stop == STATUS_STOP_TIMEOUT);
    HAS("status cmd=cgdcont sent=1 reads=0 result=none contexts=0\n"); HAS("stop=timeout");
    /* Stale context before the own echo is not mixed into contexts. */
    f = (struct fake){0};
    ADD(&f, "\r\n+CGDCONT: 9,\"IP\",\"old.apn\"\r\nAT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"internet\"\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(r.commands[STATUS_PROBE_PDP_INDEX].contexts == 0 && r.stop == STATUS_STOP_UNCONFIRMED);
    LACKS("old.apn"); LACKS("internet"); HAS("status cmd=cgdcont sent=1 reads=1 result=ok contexts=0\n");
    /* Zero count and clock failure: no extra read, closed once. */
    f = (struct fake){0};
    add_reply(&f, "", 0);
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads == 1 && f.closes == 1 && r.stop == STATUS_STOP_ZERO_COUNT);
    f = (struct fake){.clock_bad_at = 1};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    /* Real backward steps, not the UINT64_MAX sentinel: before and after the send. */
    f = (struct fake){.now = 1000, .clock_back = 1};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    f = (struct fake){.now = 1000, .clock_back = 2};
    ADD(&f, "\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    /* ERROR and CME. */
    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\nERROR\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_FINAL_ERROR);
    HAS("result=error contexts=0\n");
    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\n+CME ERROR: operation not allowed 490154203237518\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_FINAL_ERROR);
    HAS("cme=redacted"); HAS("result=cme-error contexts=0\n"); LACKS("490154");

    /* Read cap 4: echoed contexts without a final line. */
    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"a\"\r\n");
    for (unsigned i = 0; i < 5; i++) ADD(&f, "+CGDCONT: 1,\"IP\",\"a\"\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == AT_PROBE_READ_LIMIT && r.stop == STATUS_STOP_READ_CAP);
    HAS("contexts=4\n");

    /* Write failure: no reads, closed. */
    int write_errors[] = {AT_PROBE_TIMEOUT, DJI_GONE};
    for (size_t i = 0; i < 2; i++) {
        f = (struct fake){.write_status = write_errors[i]};
        ADD(&f, "\r\nOK\r\n");
        CHECK(run(&f, &r) == (i == 0 ? DJI_SYSTEM_ERROR : DJI_GONE));
        CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1 && r.commands_sent == 0);
        HAS("bytes=0 delivery=unknown"); HAS("sent=0 reads=0 result=none contexts=0");
    }
    /* Cancellation and close priority. */
    f = (struct fake){.cancel_before_write = 1};
    CHECK(run(&f, &r) == DJI_CANCELLED);
    CHECK(f.writes == 0 && f.closes == 1);
    f = (struct fake){.cancel_at_read = 1};
    ADD(&f, "\r\n+CGDCONT: 1,\"IP\",\"a\"\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED);
    f = (struct fake){.cancel_at_read = 1, .close_status = DJI_SYSTEM_ERROR};
    ADD(&f, "\r\n+CGDCONT: 1,\"IP\",\"a\"\r\n");
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    f = (struct fake){.write_status = AT_PROBE_TIMEOUT, .close_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    f = (struct fake){.open_status = DJI_BUSY};
    CHECK(run(&f, &r) == DJI_BUSY);
    CHECK(f.writes == 0 && f.closes == 0 && f.releases == 1);
    f = (struct fake){.read_status = DJI_GONE};
    CHECK(run(&f, &r) == DJI_GONE);
    CHECK(f.writes == 1 && f.closes == 1);

    /* Privacy across a full run (own echo first, so the reply is parsed and redacted). */
    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\n+CGDCONT: 1,\"IP\",\"user@corp\",\"100.64.12.34\",0,0\r\n"
            "+CGDCONT: 2,\"IPV6\",\"apn12345678\",\"fe80:0000:0000:0000:1234:5678:9abc:def0\"\r\n"
            "+CGDCONT: 3,\"IP\",\"x\",\"10.0.0.9\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    LACKS("user@corp"); LACKS("100.64"); LACKS("12345678"); LACKS("fe80"); LACKS("9abc"); LACKS("10.0.0.9");
    HAS("cid=1 type=\"IP\" apn=redacted apn_length=9 addr_present=1 addr_length=12");
    HAS("cid=2 type=\"IPV6\" apn=redacted apn_length=11 addr_present=1 addr_length=39");
    HAS("pdp line=4 redacted=1"); HAS("contexts=2\n");

    f = (struct fake){0};
    ADD(&f, "AT+CGDCONT?\r\r\n+CGDCONT: 1,\"0A7B2D43\",\"10.1.2.3\",\"\"\r\n"
            "+CGDCONT: 2,\"FOO\",\"fe80::1\",\"1.2.3.4\"\r\n"
            "+CGDCONT: 3,\"IPV4V6\",\"1.2.3.4\"\r\n\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    LACKS("0A7B2D43"); LACKS("FOO"); LACKS("10.1.2.3"); LACKS("fe80"); LACKS("1.2.3.4");
    HAS("cid=1 type=other type_length=8 apn=redacted apn_length=8 addr_present=0");
    HAS("cid=2 type=other type_length=3 apn=redacted apn_length=7 addr_present=1 addr_length=7");
    HAS("cid=3 type=\"IPV4V6\" apn=redacted apn_length=7"); HAS("contexts=3\n");

    CHECK(pdp_probe_run(NULL, &r, NULL) == DJI_SYSTEM_ERROR);

    printf("pdp probe checks=%u failures=%u (fake transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
