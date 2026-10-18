#include "../src/status_probe.h"
#include "../src/discovery.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

struct fake {
    uint64_t now;
    unsigned prepares, opens, writes, reads, closes, releases, validates;
    int active, prepared;
    int prepare_status, open_status, close_status, read_status, write_status;
    unsigned fail_validation;
    int cancel_initial, cancel_before_write, cancel_at_read;
    int clock_bad_at, clock_back;
    unsigned now_calls;
    uint32_t read_advance, validate_advance;
    int oversize;
    uint32_t timeouts[4];
    unsigned reply_count;
    const uint8_t *replies[6];
    uint32_t reply_sizes[6];
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
    return f->cancel_initial || (f->cancel_before_write && f->opens && !f->writes) ||
           (f->cancel_at_read && f->reads >= (unsigned)f->cancel_at_read);
}
static int fake_prepare(void *p, unsigned interface_id) {
    struct fake *f = p;
    CHECK(interface_id == 2 && !f->prepared);
    f->prepares++; f->prepared = 1;
    return f->prepare_status;
}
static int fake_validate(void *p, unsigned interface_id, int opened, uint8_t *out, uint8_t *in) {
    struct fake *f = p;
    CHECK(interface_id == 2 && opened == f->active);
    f->validates++;
    if (opened) { *out = 2; *in = 3; }
    if (f->writes) f->now += f->validate_advance;
    return f->validates == f->fail_validation ? DJI_GONE : DJI_OK;
}
static int fake_open(void *p) {
    struct fake *f = p;
    f->opens++;
    if (f->open_status == DJI_OK) f->active = 1;
    return f->open_status;
}
static int fake_write(void *p, uint8_t pipe, const uint8_t *bytes, uint32_t size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active && pipe == 2 && timeout == 1000);
    CHECK(f->writes == 0 && f->reads == 0);
    CHECK(size == 12 && memcmp(bytes, "AT+CGDATA=?\r", 12) == 0);
    CHECK(at_probe_is_fixed_command(bytes, size));
    f->writes++;
    return f->write_status;
}
static int fake_read(void *p, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active && pipe == 3 && f->writes == 1);
    CHECK(*size == 512 && timeout > 0 && timeout <= 2000 && f->reads < 4);
    unsigned index = f->reads++;
    if (index < 4) f->timeouts[index] = timeout;
    f->now += f->read_advance;
    memset(buffer, '8', *size);
    if (index >= f->reply_count) {
        f->now += timeout;
        *size = UINT32_MAX;
        return f->read_status ? f->read_status : AT_PROBE_TIMEOUT;
    }
    if (f->reply_sizes[index] == 0) { *size = 0; return DJI_OK; }
    CHECK(f->reply_sizes[index] <= *size);
    memcpy(buffer, f->replies[index], f->reply_sizes[index]);
    *size = f->oversize ? 513 : f->reply_sizes[index];
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
    CHECK(f->reply_count < 6);
    f->replies[f->reply_count] = (const uint8_t *)bytes;
    f->reply_sizes[f->reply_count++] = (uint32_t)size;
}
#define ADD(f, literal) add_reply((f), (literal), sizeof(literal) - 1)
static char output[32768];
static FILE *capture_begin(void) { FILE *s = tmpfile(); CHECK(s != NULL); return s; }
static void capture_end(FILE *s) {
    output[0] = '\0';
    if (!s) return;
    rewind(s);
    size_t n = fread(output, 1, sizeof(output) - 1, s);
    output[n] = '\0'; fclose(s);
}
#define HAS(text) CHECK(strstr(output, (text)) != NULL)
#define LACKS(text) CHECK(strstr(output, (text)) == NULL)
static int run(struct fake *f, struct status_probe_result *r) {
    const struct at_probe_backend b = {f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    FILE *s = capture_begin();
    int rc = data_support_probe_run(&b, r, s);
    capture_end(s);
    CHECK(!f->active && f->writes <= 1 && f->reads <= 4 && f->releases == f->prepares);
    CHECK(r->commands_sent <= 1);
    for (unsigned k = 0; k < STATUS_PROBE_DATA_SUPPORT_INDEX; k++)
        CHECK(r->commands[k].sent == 0 && r->commands[k].reads == 0);
    HAS("evidence=format-only protocol=unconfirmed"); HAS("ppp_connection=unproven");
    LACKS("cmd=cpin"); LACKS("cmd=csq"); LACKS("cmd=cops"); LACKS("cmd=cereg"); LACKS("cmd=cgdcont");
    LACKS("8888");
    return rc;
}
static struct status_command_result parse(const char *bytes, size_t size) {
    struct status_command_result c = {0};
    FILE *s = capture_begin();
    status_print_lines(STATUS_PROBE_DATA_SUPPORT_INDEX, (const uint8_t *)bytes, size, s, &c);
    capture_end(s);
    return c;
}
#define PARSE(literal) parse((literal), sizeof(literal) - 1)

static void parser_checks(void) {
    struct status_command_result c;
    c = PARSE("AT+CGDATA=?\r\r\n+CGDATA: (\"PPP\",\"M-HEX\",\"M-RAW_IP\")\r\nOK\r\n");
    CHECK(c.support_lists == 1 && c.result == STATUS_RESULT_OK);
    HAS("data-support line=1 echo=1\n"); HAS("entries=3 l2p=\"PPP,M-HEX,M-RAW_IP\" ppp_connection=unproven");
    LACKS("AT+CGDATA");
    CHECK(c.attribution == STATUS_ATTRIBUTION_MATCHED && c.attribution_reason == STATUS_REASON_FORMAT_ONLY);
    HAS("data-support attribution=matched reason=format-only\n");
    /* Trailing line after the final: unproven, the list stays filtered, the tail redacted. */
    c = PARSE("AT+CGDATA=?\r\r\n+CGDATA: (\"PPP\")\r\nOK\r\nprivate 490154203237518\r\n");
    CHECK(c.attribution == STATUS_ATTRIBUTION_UNPROVEN && c.attribution_reason == STATUS_REASON_TRAILING);
    CHECK(c.support_lists == 1); HAS("l2p=\"PPP\""); HAS("data-support line=4 redacted=1 length=23\n");
    HAS("data-support attribution=unproven reason=trailing\n"); LACKS("private"); LACKS("490154");
    /* Another command's prefix in the window: list withheld, never counted. */
    c = PARSE("AT+CGDATA=?\r\r\n+CGDCONT: 1,\"IP\",\"private\"\r\n+CGDATA: (\"PPP\")\r\nOK\r\n");
    CHECK(c.attribution == STATUS_ATTRIBUTION_MISMATCH && c.attribution_reason == STATUS_REASON_FOREIGN_PREFIX);
    CHECK(c.support_lists == 0 && c.result == STATUS_RESULT_OK);
    HAS("data-support line=3 withheld=1 length=16\n"); LACKS("l2p="); LACKS("PPP"); LACKS("private");
    const char *valid[] = {"(\"PPP\")", "(\"M-HEX\")", "(\"M-RAW_IP\")",
        " ( \"M-RAW_IP\" , \"PPP\" ) ", "(\"PPP\",\"PPP\")"};
    char in[256];
    for (size_t k = 0; k < sizeof(valid) / sizeof(valid[0]); k++) {
        int n = snprintf(in, sizeof(in), "+CGDATA: %s\r\n", valid[k]);
        c = parse(in, (size_t)n);
        CHECK(c.support_lists == 1); HAS("capability=reported"); LACKS("redacted");
    }
    const char *bad[] = {"()", "(\"\")", "(PPP)", "\"PPP\"", "(\"PPP\"", "(\"PPP\",)",
        "(,\"PPP\")", "((\"PPP\"))", "(\"PPP\" \"M-HEX\")", "(\"PPP\")junk",
        "(\"PPP\",\"private-imei-490154203237518\")", "(\"10.1.2.3\")", "(\"fe80::1\")",
        "(\"0A7B2D43\")", "(\"ppp\")", "(\"PPP \" )", "(\"PPP\\\"\")",
        "(\"PPP\",\"M-RAW-IP\")", "(\"M-HEX\",\"private\")", "(\"PPP\")\t",
        "(\"PPP\"\t)", "(\"PPP\", \"M-HEX\"),1"};
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        int n = snprintf(in, sizeof(in), "+CGDATA: %s\r\n", bad[k]);
        c = parse(in, (size_t)n);
        CHECK(c.support_lists == 0); HAS("redacted=1 length=");
        LACKS("l2p="); LACKS("PPP"); LACKS("M-HEX"); LACKS("private"); LACKS("490154");
        LACKS("10.1.2.3"); LACKS("fe80"); LACKS("0A7B2D43");
    }
    /* Every control/high byte, including CR/LF, rejects the quoted list. */
    for (unsigned ch = 0; ch <= 255; ch++) {
        if (ch >= 32 && ch < 127) continue;
        char bytes[] = "+CGDATA: (\"PPP\",\"PPP\")\r\n";
        bytes[18] = (char)ch;
        c = parse(bytes, sizeof(bytes) - 1);
        CHECK(c.support_lists == 0); LACKS("l2p="); LACKS("PPP");
    }
    c = PARSE("+CGDATA: (\"PPP\")");
    CHECK(c.support_lists == 0); HAS("partial=1 length="); LACKS("PPP");
    c = PARSE("OK\r\n");
    CHECK(c.support_lists == 0 && c.result == STATUS_RESULT_OK); LACKS("capability=reported");
    c = PARSE("+CME ERROR: private address 10.1.2.3\r\nERROR\r\n");
    CHECK(c.result == STATUS_RESULT_CME_ERROR);
    HAS("result=cme-error\n"); LACKS("result=error\n"); HAS("redacted=1 length=5");
    LACKS("private"); LACKS("10.1.2.3"); LACKS("cme=");
    c = PARSE("+CGDCONT: 1,\"IP\",\"private\",\"10.1.2.3\"\r\nCONNECT\r\n");
    HAS("unsolicited=1 length="); HAS("other=1 length="); LACKS("private"); LACKS("CONNECT"); LACKS("10.1.2.3");
    char many[2048]; size_t used = 0;
    for (unsigned k = 0; k < 20; k++)
        used += (size_t)snprintf(many + used, sizeof(many) - used, "+CGDATA: (\"PPP\")\r\n");
    used += (size_t)snprintf(many + used, sizeof(many) - used, "OK\r\n");
    c = parse(many, used);
    CHECK(c.lines == 16 && c.omitted == 5 && c.support_lists == 16 && c.result == STATUS_RESULT_OK);
    LACKS("line=17");
    const char *terminals[] = {"OK", "ERROR", "+CME ERROR: private"};
    const enum status_result terminal_results[] = {
        STATUS_RESULT_OK, STATUS_RESULT_ERROR, STATUS_RESULT_CME_ERROR};
    for (unsigned k = 0; k < 3; k++) {
        int n = snprintf(in, sizeof(in), "%s\r\n+CGDATA: (\"PPP\")\r\nOK\r\nERROR\r\n", terminals[k]);
        c = parse(in, (size_t)n);
        CHECK(c.result == terminal_results[k] && c.support_lists == 0);
        LACKS("capability=reported"); LACKS("PPP"); LACKS("private");
    }
    const char *broken[] = {
        "+CGDATA: (\"private\r\n+CGDATA: (\"PPP\")\r\n\")\r\nOK\r\n",
        "+CGDATA: (\"private\r+CGDATA: (\"PPP\")\r\")\rOK\r",
        "+CGDATA: (\"private\n+CGDATA: (\"PPP\")\n\")\nOK\n",
        "+CGDATA: (\"PPP\",\r\n+CGDATA: (\"M-HEX\")\r\nOK\r\n",
        "+CGDATA: ()\r\n+CGDATA: (\"PPP\")\r\nOK\r\n",
        "+CGDATA: (\"unknown\")\r\n+CGDATA: (\"PPP\")\r\nOK\r\n"};
    for (size_t k = 0; k < sizeof(broken) / sizeof(broken[0]); k++) {
        c = parse(broken[k], strlen(broken[k]));
        CHECK(c.result == STATUS_RESULT_OK && c.support_lists == 0);
        HAS("redacted=1 length="); LACKS("capability=reported");
        LACKS("PPP"); LACKS("M-HEX"); LACKS("private"); LACKS("unknown");
    }
}

int main(void) {
    parser_checks();
    struct fake f;
    struct status_probe_result r;
    uint32_t size;
    const uint8_t *fixed = at_probe_command_bytes(AT_PROBE_COMMAND_CGDATA_TEST, &size);
    CHECK(fixed && size == 12 && memcmp(fixed, "AT+CGDATA=?\r", 12) == 0);
    CHECK(at_probe_is_fixed_command(fixed, 12));
    CHECK(!at_probe_is_fixed_command((const uint8_t *)"AT+CGDATA=?\r", 12));
    CHECK(!at_probe_is_fixed_command(fixed, 11) && !at_probe_is_fixed_command(fixed, 13));
    for (int k = AT_PROBE_COMMAND_AT; k <= AT_PROBE_COMMAND_CGDCONT; k++) {
        const uint8_t *prior = at_probe_command_bytes((enum at_probe_command)k, &size);
        CHECK(prior && at_probe_is_fixed_command(prior, size) && prior != fixed);
    }
    f = (struct fake){0};
    const struct at_probe_backend b = {&f, fake_now, fake_cancelled, fake_prepare, fake_validate,
        fake_open, fake_write, fake_read, fake_close, fake_release};
    struct at_probe_result single;
    CHECK(at_probe_run_command(&b, AT_PROBE_COMMAND_CGDATA_TEST, &single, NULL) == DJI_SYSTEM_ERROR);
    CHECK(f.prepares == 0 && f.writes == 0);
    CHECK(data_support_probe_run(NULL, &r, NULL) == DJI_SYSTEM_ERROR);

    f = (struct fake){0};
    ADD(&f, "AT+CGDATA=?\r\r\n+CGDATA: (\"PPP\",\"M-HEX\")\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 1 && f.writes == 1 && f.closes == 1);
    HAS("cmd=cgdata-test out=0x03 in=0x84 write=ok bytes=12");
    HAS("sent=1 reads=1 result=ok support_lists=1 ppp_connection=unproven");
    f = (struct fake){0};
    ADD(&f, "AT+CG"); ADD(&f, "DATA=?\r\r\n+CGDA"); ADD(&f, "TA: (\"M-RAW_"); ADD(&f, "IP\",\"PPP\")\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.reads == 4 && r.stop == STATUS_STOP_NONE);
    HAS("l2p=\"PPP,M-RAW_IP\""); HAS("result=ok support_lists=1");
    const char *boundary[] = {
        "AT+CGDATA=?\r\r\n+CGDATA: (\"private\r\n+CGDATA: (\"PPP\")\r\n\")\r\nOK\r\n",
        "AT+CGDATA=?\r\r\nERROR\r\n+CGDATA: (\"PPP\")\r\nOK\r\n",
        "AT+CGDATA=?\r\r\n+CME ERROR: private\r\nOK\r\n",
        "AT+CGDATA=?\r\r\nOK\r\n+CGDATA: (\"PPP\")\r\nERROR\r\n"};
    const enum status_result boundary_results[] = {
        STATUS_RESULT_OK, STATUS_RESULT_ERROR, STATUS_RESULT_CME_ERROR, STATUS_RESULT_OK};
    for (unsigned k = 0; k < 4; k++) {
        f = (struct fake){0};
        add_reply(&f, boundary[k], strlen(boundary[k]));
        CHECK(run(&f, &r) == DJI_OK);
        CHECK(f.writes == 1 && f.reads == 1);
        CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].support_lists == 0);
        CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].result == boundary_results[k]);
        LACKS("capability=reported"); LACKS("PPP"); LACKS("private");
    }
    f = (struct fake){0};
    ADD(&f, "AT+CGDATA=?\r\r\n+CGDATA: (\"pri"); ADD(&f, "vate\r");
    ADD(&f, "\n+CGDATA: (\"PPP\")\r\n"); ADD(&f, "\")\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.writes == 1 && f.reads == 4);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].support_lists == 0);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].result == STATUS_RESULT_OK);
    LACKS("capability=reported"); LACKS("PPP"); LACKS("private");
    f = (struct fake){0};
    ADD(&f, "AT+CGDATA=?\r\r\nERROR\r"); ADD(&f, "\n+CGDATA: (\"PPP\")\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.writes == 1 && f.reads == 1);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].result == STATUS_RESULT_ERROR);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].support_lists == 0);
    LACKS("capability=reported"); LACKS("PPP");
    f = (struct fake){0};
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.reads == 1 && r.stop == STATUS_STOP_TIMEOUT);
    HAS("status=timeout bytes=0"); HAS("result=none support_lists=0"); HAS("stop=timeout");
    /* An earlier foreign reply before the own echo is skipped unread; only the list after
     * the own echo is counted. (An earlier own-prefix list is a mismatch, next case.) */
    f = (struct fake){0};
    ADD(&f, "+CEREG: 2,5,\"1A2B\"\r\nOK\r\nAT+CGDATA=?\r\r\n+CGDATA: (\"PPP\")\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 1 && r.stop == STATUS_STOP_NONE);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].support_lists == 1);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].pre_echo_skipped == 2);
    HAS("l2p=\"PPP\""); LACKS("1A2B"); LACKS("CEREG"); HAS("status cmd=cgdata-test pre_echo_skipped=2\n");
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].attribution == STATUS_ATTRIBUTION_MATCHED &&
          r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].attribution_reason == STATUS_REASON_FORMAT_ONLY);
    /* Own prefix before the own echo is not promoted. */
    f = (struct fake){0};
    ADD(&f, "+CGDATA: (\"PPP\")\r\nAT+CGDATA=?\r\r\n+CGDATA: (\"M-HEX\")\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(r.stop == STATUS_STOP_UNCONFIRMED);
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].support_lists == 0);
    LACKS("l2p="); LACKS("PPP"); LACKS("M-HEX");
    f = (struct fake){0};
    add_reply(&f, "", 0);
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.writes == 1 && f.reads == 1 && f.closes == 1 && r.stop == STATUS_STOP_ZERO_COUNT);
    f = (struct fake){.clock_bad_at = 1};
    ADD(&f, "OK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    /* Real backward steps, not the UINT64_MAX sentinel: before and after the send. */
    f = (struct fake){.now = 1000, .clock_back = 1};
    ADD(&f, "OK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 0 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    f = (struct fake){.now = 1000, .clock_back = 2};
    ADD(&f, "OK\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR);
    CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1 && r.stop == STATUS_STOP_CLOCK_ERROR);
    f = (struct fake){0}; ADD(&f, "AT+CGDATA=?\r\r\nOK\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(r.stop == STATUS_STOP_NONE); HAS("result=ok support_lists=0"); LACKS("l2p=");
    f = (struct fake){0}; ADD(&f, "AT+CGDATA=?\r\r\nERROR\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.reads == 1 && r.stop == STATUS_STOP_FINAL_ERROR); HAS("result=error support_lists=0");
    f = (struct fake){0}; ADD(&f, "AT+CGDATA=?\r\r\n+CME ERROR: private 490154203237518\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(r.stop == STATUS_STOP_FINAL_ERROR); HAS("result=cme-error support_lists=0"); LACKS("private"); LACKS("490154");
    f = (struct fake){0};
    ADD(&f, "AT+CGDATA=?\r\r\n");
    for (unsigned k = 0; k < 5; k++) ADD(&f, "+CGDATA: (\"PPP\")\r\n");
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.reads == 4 && f.writes == 1 && r.stop == STATUS_STOP_READ_CAP);
    char full[512]; memset(full, 'x', sizeof(full));
    f = (struct fake){0};
    for (unsigned k = 0; k < 4; k++) add_reply(&f, full, sizeof(full));
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.reads == 4);
    HAS("partial=1 length=2048"); LACKS("xxxx");
    f = (struct fake){.read_advance = 700};
    for (unsigned k = 0; k < 6; k++) ADD(&f, "x");
    CHECK(run(&f, &r) == DJI_OK);
    CHECK(f.reads == 3 && f.timeouts[0] == 2000 && f.timeouts[1] == 1300 && f.timeouts[2] == 600);
    f = (struct fake){.validate_advance = 2000};
    CHECK(run(&f, &r) == DJI_OK); CHECK(f.reads == 0 && f.writes == 1);
    int errors[] = {AT_PROBE_TIMEOUT, DJI_GONE, DJI_ACCESS, DJI_UNSUPPORTED};
    for (size_t k = 0; k < sizeof(errors) / sizeof(errors[0]); k++) {
        f = (struct fake){.write_status = errors[k]}; ADD(&f, "OK\r\n");
        CHECK(run(&f, &r) == (errors[k] == AT_PROBE_TIMEOUT ? DJI_SYSTEM_ERROR : errors[k]));
        CHECK(f.writes == 1 && f.reads == 0 && f.closes == 1 && r.commands_sent == 0);
        HAS("bytes=0 delivery=unknown");
    }
    f = (struct fake){.read_status = DJI_GONE};
    CHECK(run(&f, &r) == DJI_GONE); CHECK(f.reads == 1 && f.closes == 1);
    f = (struct fake){.oversize = 1}; ADD(&f, "private\r\n");
    CHECK(run(&f, &r) == DJI_SYSTEM_ERROR); LACKS("private");
    CHECK(r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].attribution == STATUS_ATTRIBUTION_UNPROVEN &&
          r.commands[STATUS_PROBE_DATA_SUPPORT_INDEX].attribution_reason == STATUS_REASON_IO_ERROR);
    HAS("data-support attribution=unproven reason=io-error\n");
    f = (struct fake){.cancel_initial = 1};
    CHECK(run(&f, &r) == DJI_CANCELLED); CHECK(f.prepares == 0);
    f = (struct fake){.cancel_before_write = 1};
    CHECK(run(&f, &r) == DJI_CANCELLED); CHECK(f.writes == 0 && f.closes == 1);
    f = (struct fake){.cancel_at_read = 1}; ADD(&f, "+CGDATA: (\"PPP\")\r\n");
    CHECK(run(&f, &r) == DJI_CANCELLED); LACKS("l2p=");
    f = (struct fake){.cancel_at_read = 1, .close_status = DJI_SYSTEM_ERROR}; ADD(&f, "x");
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    f = (struct fake){.write_status = AT_PROBE_TIMEOUT, .close_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &r) == DJI_CLOSE_ERROR);
    f = (struct fake){.open_status = DJI_BUSY};
    CHECK(run(&f, &r) == DJI_BUSY); CHECK(f.writes == 0 && f.closes == 0);
    f = (struct fake){.prepare_status = DJI_ACCESS};
    CHECK(run(&f, &r) == DJI_ACCESS); CHECK(f.opens == 0 && f.writes == 0);
    for (unsigned k = 1; k <= 3; k++) {
        f = (struct fake){.fail_validation = k};
        CHECK(run(&f, &r) == DJI_GONE);
        CHECK(f.writes == (k == 3 ? 1u : 0u) && f.reads == 0);
    }
    printf("data support probe checks=%u failures=%u (fake transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
