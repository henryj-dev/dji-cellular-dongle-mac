#include "status_probe.h"
#include "discovery.h"

#include <string.h>

static const char *const NAMES[STATUS_PROBE_SLOTS] = {"cpin", "csq", "cops", "cereg", "cgdcont", "cgdata-test", "cesq"};
/* Echo text (command without CR) and the response prefix each command owns. */
static const char *const ECHOES[STATUS_PROBE_SLOTS] = {"AT+CPIN?", "AT+CSQ", "AT+COPS?", "AT+CEREG?", "AT+CGDCONT?", "AT+CGDATA=?", "AT+CESQ"};
static const char *const PREFIXES[STATUS_PROBE_SLOTS] = {"+CPIN:", "+CSQ:", "+COPS:", "+CEREG:", "+CGDCONT:", "+CGDATA:", "+CESQ:"};
static const enum at_probe_command SLOT_COMMANDS[STATUS_PROBE_SLOTS] = {
    AT_PROBE_COMMAND_CPIN, AT_PROBE_COMMAND_CSQ, AT_PROBE_COMMAND_COPS, AT_PROBE_COMMAND_CEREG,
    AT_PROBE_COMMAND_CGDCONT, AT_PROBE_COMMAND_CGDATA_TEST, AT_PROBE_COMMAND_CESQ};
static const char CME[] = "+CME ERROR:";

enum at_probe_command status_probe_command(unsigned index) {
    static const enum at_probe_command order[STATUS_PROBE_COMMANDS] = {
        AT_PROBE_COMMAND_CPIN, AT_PROBE_COMMAND_CSQ, AT_PROBE_COMMAND_COPS, AT_PROBE_COMMAND_CEREG};
    return index < STATUS_PROBE_COMMANDS ? order[index] : AT_PROBE_COMMAND_AT;
}

const char *status_probe_name(unsigned index) {
    return index < STATUS_PROBE_COMMANDS ? NAMES[index] : "unknown";
}

static int starts_with(const uint8_t *line, size_t length, const char *text) {
    size_t n = strlen(text);
    return length >= n && memcmp(line, text, n) == 0;
}

static int equals(const uint8_t *line, size_t length, const char *text) {
    return length == strlen(text) && memcmp(line, text, length) == 0;
}

static int is_digit(uint8_t ch) { return ch >= '0' && ch <= '9'; }
static int is_upper(uint8_t ch) { return ch >= 'A' && ch <= 'Z'; }
static int is_alpha(uint8_t ch) { return is_upper(ch) || (ch >= 'a' && ch <= 'z'); }

/* Total digits anywhere in the field, not only the longest run ("123456-654321" counts 12). */
static size_t digit_count(const uint8_t *text, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) count += is_digit(text[i]) != 0;
    return count;
}

/* Consumes 1..max digits at *pos and stops at a non-digit. Empty or longer fields fail. */
static int take_number(const uint8_t *text, size_t length, size_t *pos, size_t max,
                       size_t *start, size_t *count) {
    *start = *pos;
    while (*pos < length && is_digit(text[*pos])) (*pos)++;
    *count = *pos - *start;
    return *count >= 1 && *count <= max;
}

static int take_char(const uint8_t *text, size_t length, size_t *pos, uint8_t ch) {
    if (*pos >= length || text[*pos] != ch) return 0;
    (*pos)++;
    return 1;
}

static void skip_spaces(const uint8_t *text, size_t length, size_t *pos) {
    while (*pos < length && text[*pos] == ' ') (*pos)++;
}

static enum status_result final_result(const uint8_t *line, size_t length) {
    if (equals(line, length, "OK")) return STATUS_RESULT_OK;
    if (equals(line, length, "ERROR")) return STATUS_RESULT_ERROR;
    if (starts_with(line, length, CME)) return STATUS_RESULT_CME_ERROR;
    return STATUS_RESULT_NONE;
}

int status_has_final_line(const uint8_t *bytes, size_t size) {
    if (!bytes) return 0;
    size_t index = 0;
    while (index < size) {
        if (bytes[index] == '\r' || bytes[index] == '\n') { index++; continue; }
        size_t start = index;
        while (index < size && bytes[index] != '\r' && bytes[index] != '\n') index++;
        if (index < size && final_result(bytes + start, index - start) != STATUS_RESULT_NONE) return 1;
    }
    return 0;
}

/* Text fields are printed only when every byte passes the class check, the length is
 * bounded and the field holds fewer than 7 digits in total. */
static int text_ok(const uint8_t *text, size_t length, size_t max, int (*allowed)(uint8_t)) {
    if (length == 0 || length > max || digit_count(text, length) >= STATUS_PROBE_DIGIT_RUN) return 0;
    for (size_t i = 0; i < length; i++) if (!allowed(text[i])) return 0;
    return 1;
}
/* CME text: letters-only words separated by spaces. Any digit in a word redacts it. */
static int cme_char(uint8_t ch) { return is_alpha(ch) || ch == ' '; }
static int cpin_char(uint8_t ch) { return is_upper(ch) || is_digit(ch) || ch == ' '; }
/* Operator names: no quote, comma or control byte, so the field structure cannot be forged. */
static int oper_char(uint8_t ch) {
    return is_alpha(ch) || is_digit(ch) || ch == ' ' || ch == '-' || ch == '.' || ch == '&' ||
           ch == '+' || ch == '/' || ch == '\'' || ch == '_' || ch == '(' || ch == ')';
}
/* Known 3GPP PDP types, exact case. Anything else is reported only as other + length. */
static int pdp_type_known(const uint8_t *t, size_t n) {
    static const char *const types[] = {"IP", "IPV6", "IPV4V6", "PPP", "X.25", "OSPIH", "NON-IP",
                                        "ETHERNET", "UNSTRUCTURED"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (equals(t, n, types[i])) return 1;
    return 0;
}
static int is_hex(uint8_t ch) { return is_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }
/* Address-shaped APN: digits and dots only (IPv4-like), or hex/digits/dots/colons with a colon (IPv6-like). */
static int apn_address_shaped(const uint8_t *v, size_t n) {
    int dotted = n != 0, colon_form = n != 0, colons = 0;
    for (size_t k = 0; k < n; k++) {
        if (!is_digit(v[k]) && v[k] != '.') dotted = 0;
        if (!is_hex(v[k]) && v[k] != '.' && v[k] != ':') colon_form = 0;
        colons += v[k] == ':';
    }
    return dotted || (colon_form && colons > 0);
}
static int apn_char(uint8_t ch) { return is_alpha(ch) || is_digit(ch) || ch == '.' || ch == '-' || ch == '_'; }

static int print_cpin(FILE *s, const char *head, const uint8_t *v, size_t n) {
    size_t pos = 0;
    skip_spaces(v, n, &pos);
    /* A CPIN code is a status word (READY, SIM PIN, ...): digits-only values are never codes. */
    int has_upper = 0;
    for (size_t k = pos; k < n; k++) has_upper |= is_upper(v[k]);
    if (!has_upper || !text_ok(v + pos, n - pos, STATUS_PROBE_CPIN_MAX, cpin_char)) return 0;
    fprintf(s, "%s cpin=\"%.*s\"\n", head, (int)(n - pos), (const char *)v + pos);
    return 1;
}

/* Up to 3 digits, already checked by take_number. */
static unsigned number_value(const uint8_t *v, size_t start, size_t count) {
    unsigned value = 0;
    for (size_t k = 0; k < count; k++) value = value * 10u + (unsigned)(v[start + k] - '0');
    return value;
}

/* Keeps the numbers of a fully parsed line, replacing any earlier line's. */
static void keep_values(struct status_command_result *command, const uint8_t *v, const size_t *starts,
                        const size_t *counts, unsigned count) {
    for (unsigned k = 0; k < count && k < STATUS_PROBE_VALUES; k++)
        command->value[k] = number_value(v, starts[k], counts[k]);
    command->values = count;
}

static int print_csq(FILE *s, const char *head, const uint8_t *v, size_t n,
                     struct status_command_result *command) {
    size_t pos = 0, starts[2], counts[2];
    skip_spaces(v, n, &pos);
    if (!take_number(v, n, &pos, 3, &starts[0], &counts[0]) || !take_char(v, n, &pos, ',') ||
        !take_number(v, n, &pos, 3, &starts[1], &counts[1]) || pos != n) return 0;
    keep_values(command, v, starts, counts, 2);
    fprintf(s, "%s rssi=%.*s ber=%.*s\n", head, (int)counts[0], (const char *)v + starts[0],
            (int)counts[1], (const char *)v + starts[1]);
    return 1;
}

/* +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>, six 1..3 digit numbers. */
static int print_cesq(FILE *s, const char *head, const uint8_t *v, size_t n,
                      struct status_command_result *command) {
    size_t pos = 0, starts[6], counts[6];
    skip_spaces(v, n, &pos);
    for (unsigned k = 0; k < 6; k++)
        if ((k && !take_char(v, n, &pos, ',')) || !take_number(v, n, &pos, 3, &starts[k], &counts[k])) return 0;
    if (pos != n) return 0;
    keep_values(command, v, starts, counts, 6);
    fprintf(s, "%s rxlev=%u ber=%u rscp=%u ecno=%u rsrq=%u rsrp=%u\n", head, command->value[0],
            command->value[1], command->value[2], command->value[3], command->value[4], command->value[5]);
    return 1;
}

static int print_cops(FILE *s, const char *head, const uint8_t *v, size_t n) {
    size_t pos = 0, m, mn, f, fn, o, on, a, an = 0;
    skip_spaces(v, n, &pos);
    if (!take_number(v, n, &pos, 2, &m, &mn)) return 0;
    if (pos == n) {
        fprintf(s, "%s mode=%.*s\n", head, (int)mn, (const char *)v + m);
        return 1;
    }
    if (!take_char(v, n, &pos, ',') || !take_number(v, n, &pos, 2, &f, &fn) ||
        !take_char(v, n, &pos, ',') || !take_char(v, n, &pos, '"')) return 0;
    o = pos;
    while (pos < n && v[pos] != '"') pos++;
    on = pos - o;
    if (!take_char(v, n, &pos, '"') || !text_ok(v + o, on, STATUS_PROBE_OPER_MAX, oper_char)) return 0;
    if (pos != n && (!take_char(v, n, &pos, ',') || !take_number(v, n, &pos, 2, &a, &an) || pos != n))
        return 0;
    fprintf(s, "%s mode=%.*s format=%.*s oper=\"%.*s\"", head, (int)mn, (const char *)v + m,
            (int)fn, (const char *)v + f, (int)on, (const char *)v + o);
    if (an) fprintf(s, " act=%.*s", (int)an, (const char *)v + a);
    fputc('\n', s);
    return 1;
}

/* Only n and stat are ever printed. Location (tac/ci) and later fields are counted, never shown. */
static int print_cereg(FILE *s, const char *head, const uint8_t *v, size_t n) {
    size_t pos = 0, a, an, b, bn, extra = 0;
    skip_spaces(v, n, &pos);
    if (!take_number(v, n, &pos, 2, &a, &an) || !take_char(v, n, &pos, ',') ||
        !take_number(v, n, &pos, 2, &b, &bn)) return 0;
    if (pos != n && v[pos] != ',') return 0;
    for (; pos < n; pos++) if (v[pos] == ',') extra++;
    fprintf(s, "%s n=%.*s stat=%.*s extra_fields=%zu\n", head, (int)an, (const char *)v + a,
            (int)bn, (const char *)v + b, extra);
    return 1;
}

/* Quoted field at *pos: returns content start/length; no quote, comma or byte inside is decoded. */
static int take_quoted(const uint8_t *v, size_t n, size_t *pos, size_t *start, size_t *length) {
    if (!take_char(v, n, pos, '"')) return 0;
    *start = *pos;
    while (*pos < n && v[*pos] != '"') (*pos)++;
    *length = *pos - *start;
    return take_char(v, n, pos, '"');
}

/* cid, PDP type and APN only. The PDP address is never printed: presence and length only.
 * Later fields (d_comp, h_comp, ...) are counted, never shown. */
static int print_cgdcont(FILE *s, const char *head, const uint8_t *v, size_t n) {
    size_t pos = 0, c, cn, t, tn, a, an, d = 0, dn = 0, extra = 0;
    int address = 0, quoted = 0;
    skip_spaces(v, n, &pos);
    if (!take_number(v, n, &pos, 2, &c, &cn) || !take_char(v, n, &pos, ',') ||
        !take_quoted(v, n, &pos, &t, &tn) ||
        !take_char(v, n, &pos, ',') || !take_quoted(v, n, &pos, &a, &an)) return 0;
    if (pos != n) {
        if (!take_char(v, n, &pos, ',') || !take_quoted(v, n, &pos, &d, &dn)) return 0;
        address = 1;
        /* Anything after the address must start with a comma; each comma outside quotes
         * opens one more field. Unbalanced quotes break the structure. */
        if (pos != n && v[pos] != ',') return 0;
        for (; pos < n; pos++) {
            if (v[pos] == '"') quoted = !quoted;
            else if (v[pos] == ',' && !quoted) extra++;
        }
        if (quoted) return 0;
    }
    int apn_ok = an <= STATUS_PROBE_APN_MAX && digit_count(v + a, an) < STATUS_PROBE_DIGIT_RUN &&
                 !apn_address_shaped(v + a, an);
    for (size_t k = 0; k < an && apn_ok; k++) apn_ok = apn_char(v[a + k]);
    fprintf(s, "%s cid=%.*s", head, (int)cn, (const char *)v + c);
    if (pdp_type_known(v + t, tn)) fprintf(s, " type=\"%.*s\"", (int)tn, (const char *)v + t);
    else fprintf(s, " type=other type_length=%zu", tn);
    if (apn_ok) fprintf(s, " apn=\"%.*s\"", (int)an, (const char *)v + a);
    else fprintf(s, " apn=redacted apn_length=%zu", an);
    fprintf(s, " addr_present=%d addr_length=%zu extra_fields=%zu\n", address && dn != 0, address ? dn : 0u, extra);
    return 1;
}

/* Validate the whole quoted list before printing anything. Names in output are source
 * constants only; unknown/empty/control/broken lists never expose even a known prefix.
 * This is a capability report, not evidence that a PPP connection works. */
static int print_cgdata_test(FILE *s, const char *head, const uint8_t *v, size_t n) {
    static const char *const known[] = {"PPP", "M-HEX", "M-RAW_IP"};
    size_t pos = 0, start, length;
    unsigned mask = 0, count = 0;
    skip_spaces(v, n, &pos);
    if (!take_char(v, n, &pos, '(')) return 0;
    for (;;) {
        skip_spaces(v, n, &pos);
        if (!take_quoted(v, n, &pos, &start, &length)) return 0;
        unsigned k = 0;
        while (k < sizeof(known) / sizeof(known[0]) && !equals(v + start, length, known[k])) k++;
        if (k == sizeof(known) / sizeof(known[0])) return 0;
        mask |= 1u << k;
        count++;
        skip_spaces(v, n, &pos);
        if (take_char(v, n, &pos, ')')) break;
        if (!take_char(v, n, &pos, ',')) return 0;
    }
    skip_spaces(v, n, &pos);
    if (pos != n) return 0;
    fprintf(s, "%s capability=reported entries=%u l2p=\"", head, count);
    int comma = 0;
    for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        if (!(mask & (1u << k))) continue;
        fprintf(s, "%s%s", comma ? "," : "", known[k]);
        comma = 1;
    }
    fputs("\" ppp_connection=unproven\n", s);
    return 1;
}

/* Collection facts from the read loop; they only ever downgrade attribution. */
#define REPLY_IO_ERROR 1u
#define REPLY_TRUNCATED 2u
/* Run-only policy; the public direct parser retains its strict whole-reply semantics. */
#define REPLY_SKIP_PRE_ECHO 4u

static const char *const STOPS[] = {"none", "timeout", "io-error", "cancelled", "clock-error",
    "time-budget", "read-cap", "zero-count", "oversize", "write-error", "unconfirmed", "final-error"};

static const char *const ATTRIBUTIONS[] = {"unproven", "mismatch", "matched"};
static const char *const REASONS[] = {
    "not-analyzed", "format-only", "foreign-echo", "dup-echo", "final-before-echo", "stale-before-echo",
    "foreign-prefix", "multi-final", "io-error", "truncated", "omitted", "no-echo", "partial", "no-final",
    "trailing", "cms-error", "unlisted-prefix", "other-line"};

static int other_listed_prefix(unsigned index, const uint8_t *line, size_t length) {
    for (unsigned k = 0; k < STATUS_PROBE_SLOTS; k++)
        if (k != index && starts_with(line, length, PREFIXES[k])) return 1;
    return 0;
}

/* Decides attribution over the whole joined reply before anything is printed, so the
 * printed lines and the attribution line can never disagree. Mismatch reasons win over
 * unproven ones; flags and caps only ever downgrade. An unlisted + line is not asserted to
 * belong to another command; it only prevents MATCHED.
 * With REPLY_SKIP_PRE_ECHO (run path only) every terminated line before the first own echo
 * is skipped unread, except that an own-prefix line there still means stale-before-echo.
 * Returns 1 when a terminated final line follows the first own echo (run completion). */
static int analyze_reply(unsigned index, const uint8_t *bytes, size_t size, unsigned flags,
                         struct status_command_result *command) {
    size_t seen = 0, echoes = 0, finals = 0, skipped = 0;
    int foreign_echo = 0, final_before = 0, stale_before = 0, foreign = 0, partial = 0, trailing = 0,
        cms = 0, unlisted = 0, other = 0, final_after = 0;
    const int skip = (flags & REPLY_SKIP_PRE_ECHO) != 0;
    size_t at = 0, binary = 0, own_echoes = 0;
    for (size_t i = 0; i < size; i++)
        if (bytes[i] != '\r' && bytes[i] != '\n' && (bytes[i] < 0x20 || bytes[i] > 0x7e)) binary++;
    while (at < size) {
        if (bytes[at] == '\r' || bytes[at] == '\n') { at++; continue; }
        size_t start = at;
        while (at < size && bytes[at] != '\r' && bytes[at] != '\n') at++;
        const uint8_t *line = bytes + start;
        size_t length = at - start;
        if (at != size && equals(line, length, ECHOES[index])) own_echoes++;
        if (skip && at != size && echoes == 0 && !equals(line, length, ECHOES[index])) {
            /* Earlier output is never attributed to this request; only an own prefix,
             * which could be this reply arriving without its echo, blocks MATCHED. */
            stale_before |= starts_with(line, length, PREFIXES[index]);
            skipped++;
            continue;
        }
        seen++;
        if (at == size) { partial = 1; break; }
        const int before = echoes == 0;
        if (finals) trailing = 1;
        if (equals(line, length, ECHOES[index])) echoes++;
        else if (starts_with(line, length, "AT")) foreign_echo = 1;
        else if (final_result(line, length) != STATUS_RESULT_NONE) {
            finals++;
            final_before |= before;
            final_after |= !before;
        }
        else if (starts_with(line, length, PREFIXES[index])) stale_before |= before;
        else if (other_listed_prefix(index, line, length)) { stale_before |= before; foreign = 1; }
        else if (starts_with(line, length, "+CMS ERROR:")) { stale_before |= before; cms = 1; }
        /* An unknown + URC is no direct evidence of another reply, wherever it sits. */
        else if (line[0] == '+') unlisted = 1;
        else { stale_before |= before; other = 1; }
    }
    enum status_attribution_reason reason;
    if (foreign_echo) reason = STATUS_REASON_FOREIGN_ECHO;
    else if (echoes > 1) reason = STATUS_REASON_DUP_ECHO;
    else if (echoes && final_before) reason = STATUS_REASON_FINAL_BEFORE_ECHO;
    else if (echoes && stale_before) reason = STATUS_REASON_STALE_BEFORE_ECHO;
    else if (foreign) reason = STATUS_REASON_FOREIGN_PREFIX;
    else if (finals > 1) reason = STATUS_REASON_MULTI_FINAL;
    else if (flags & REPLY_IO_ERROR) reason = STATUS_REASON_IO_ERROR;
    else if (flags & REPLY_TRUNCATED) reason = STATUS_REASON_TRUNCATED;
    else if (command->omitted || command->lines > STATUS_PROBE_MAX_LINES ||
             seen > STATUS_PROBE_MAX_LINES - command->lines) reason = STATUS_REASON_OMITTED;
    else if (!echoes) reason = STATUS_REASON_NO_ECHO;
    else if (partial) reason = STATUS_REASON_PARTIAL;
    else if (!finals) reason = STATUS_REASON_NO_FINAL;
    else if (trailing) reason = STATUS_REASON_TRAILING;
    else if (cms) reason = STATUS_REASON_CMS_ERROR;
    else if (unlisted) reason = STATUS_REASON_UNLISTED_PREFIX;
    else if (other) reason = STATUS_REASON_OTHER_LINE;
    else reason = STATUS_REASON_FORMAT_ONLY;
    /* Assigned, not added, so repeated scans of the same joined bytes never accumulate. */
    command->pre_echo_skipped = (unsigned)(skipped > UINT32_MAX ? UINT32_MAX : skipped);
    command->own_echoes = (unsigned)(own_echoes > UINT32_MAX ? UINT32_MAX : own_echoes);
    command->binary = (unsigned)(binary > UINT32_MAX ? UINT32_MAX : binary);
    command->attribution_reason = reason;
    command->attribution = reason == STATUS_REASON_FORMAT_ONLY ? STATUS_ATTRIBUTION_MATCHED
                           : reason <= STATUS_REASON_MULTI_FINAL ? STATUS_ATTRIBUTION_MISMATCH
                                                                 : STATUS_ATTRIBUTION_UNPROVEN;
    return final_after;
}

/* Shared by status_print_lines and run_slots: attribution first, then filtered lines,
 * then the one fixed attribution line. */
static void print_reply(unsigned index, const uint8_t *bytes, size_t size, unsigned flags, FILE *stream,
                        struct status_command_result *command) {
    if (!bytes || index >= STATUS_PROBE_SLOTS || !command) return;
    analyze_reply(index, bytes, size, flags, command);
    const int withhold = command->attribution == STATUS_ATTRIBUTION_MISMATCH;
    const int skip = (flags & REPLY_SKIP_PRE_ECHO) != 0;
    int echo_seen = 0;
    size_t at = 0;
    /* Only the capability test fails closed after a malformed list. Joined reads
     * are parsed once, so this also covers CR/LF fragments across USB reads. */
    int support_blocked = 0;
    while (at < size) {
        if (bytes[at] == '\r' || bytes[at] == '\n') { at++; continue; }
        size_t start = at;
        while (at < size && bytes[at] != '\r' && bytes[at] != '\n') at++;
        const uint8_t *line = bytes + start;
        size_t length = at - start;
        const int partial = at == size;
        const int own_echo = !partial && equals(line, length, ECHOES[index]);
        /* Same predicate as analyze_reply: skipped lines are never parsed, counted as
         * lines, used as a result or printed; analyze_reply already counted them. */
        if (skip && !echo_seen && !partial && !own_echo) continue;
        echo_seen |= own_echo;
        enum status_result result = partial ? STATUS_RESULT_NONE : final_result(line, length);
        const int after_terminal = index == STATUS_PROBE_DATA_SUPPORT_INDEX &&
                                   command->result != STATUS_RESULT_NONE;
        if (result != STATUS_RESULT_NONE && !after_terminal) command->result = result;
        if (command->lines >= STATUS_PROBE_MAX_LINES) { command->omitted++; continue; }
        unsigned number = ++command->lines;
        if (!stream) continue;
        char head[48];
        if (index == STATUS_PROBE_PDP_INDEX) snprintf(head, sizeof(head), "pdp line=%u", number);
        else if (index == STATUS_PROBE_DATA_SUPPORT_INDEX) snprintf(head, sizeof(head), "data-support line=%u", number);
        else snprintf(head, sizeof(head), "status cmd=%s line=%u", NAMES[index], number);
        /* A mismatched own-prefix line is withheld even after a terminal line. */
        if (withhold && !partial && starts_with(line, length, PREFIXES[index])) {
            fprintf(stream, "%s withheld=1 length=%zu\n", head, length);
            continue;
        }
        /* Nothing after the first terminal line can be response evidence. */
        if (after_terminal) { fprintf(stream, "%s redacted=1 length=%zu\n", head, length); continue; }
        /* Unterminated tail: its length is unknown, so it is never parsed. */
        if (partial) { fprintf(stream, "%s partial=1 length=%zu\n", head, length); continue; }
        if (equals(line, length, ECHOES[index])) { fprintf(stream, "%s echo=1\n", head); continue; }
        if (result == STATUS_RESULT_OK || result == STATUS_RESULT_ERROR) {
            fprintf(stream, "%s result=%s\n", head, result == STATUS_RESULT_OK ? "ok" : "error");
            continue;
        }
        if (result == STATUS_RESULT_CME_ERROR) {
            if (index == STATUS_PROBE_DATA_SUPPORT_INDEX) {
                fprintf(stream, "%s result=cme-error\n", head);
                continue;
            }
            size_t pos = sizeof(CME) - 1;
            skip_spaces(line, length, &pos);
            const uint8_t *text = line + pos;
            size_t text_length = length - pos, code_start = 0, code_digits = 0, code_pos = 0;
            int has_alpha = 0;
            for (size_t k = 0; k < text_length; k++) has_alpha |= is_alpha(text[k]);
            /* Numeric error code: exactly one 1..3 digit number and nothing else. */
            if (take_number(text, text_length, &code_pos, 3, &code_start, &code_digits) && code_pos == text_length)
                fprintf(stream, "%s result=cme-error cme=%.*s\n", head, (int)code_digits, (const char *)text);
            else if (has_alpha && text_ok(text, text_length, STATUS_PROBE_CME_MAX, cme_char))
                fprintf(stream, "%s result=cme-error cme=\"%.*s\"\n", head, (int)text_length, (const char *)text);
            else fprintf(stream, "%s result=cme-error cme=redacted\n", head);
            continue;
        }
        size_t prefix = strlen(PREFIXES[index]);
        if (starts_with(line, length, PREFIXES[index])) {
            const uint8_t *value = line + prefix;
            command->value_lines++;
            size_t value_length = length - prefix;
            int printed = 0;
            if (index == 0) printed = print_cpin(stream, head, value, value_length);
            else if (index == 1) printed = print_csq(stream, head, value, value_length, command);
            else if (index == 2) printed = print_cops(stream, head, value, value_length);
            else if (index == 3) printed = print_cereg(stream, head, value, value_length);
            else if (index == STATUS_PROBE_PDP_INDEX) printed = print_cgdcont(stream, head, value, value_length);
            else if (index == STATUS_PROBE_CESQ_INDEX) printed = print_cesq(stream, head, value, value_length, command);
            else {
                if (!support_blocked) printed = print_cgdata_test(stream, head, value, value_length);
                /* No resynchronization after unknown or malformed support syntax:
                 * a following +CGDATA line may be inside a broken quoted value. */
                if (!printed) support_blocked = 1;
            }
            if (printed && index == STATUS_PROBE_PDP_INDEX) command->contexts++;
            if (printed && index == STATUS_PROBE_DATA_SUPPORT_INDEX) command->support_lists++;
            if (!printed) fprintf(stream, "%s redacted=1 length=%zu\n", head, length);
            continue;
        }
        if (line[0] == '+') fprintf(stream, "%s unsolicited=1 length=%zu\n", head, length);
        else fprintf(stream, "%s other=1 length=%zu\n", head, length);
    }
    if (!stream) return;
    const unsigned a = command->attribution, r = command->attribution_reason;
    const char *label = a < sizeof(ATTRIBUTIONS) / sizeof(ATTRIBUTIONS[0]) ? ATTRIBUTIONS[a] : "unproven";
    const char *why = r < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[r] : "not-analyzed";
    if (index == STATUS_PROBE_PDP_INDEX) fprintf(stream, "pdp attribution=%s reason=%s\n", label, why);
    else if (index == STATUS_PROBE_DATA_SUPPORT_INDEX)
        fprintf(stream, "data-support attribution=%s reason=%s\n", label, why);
    else fprintf(stream, "status cmd=%s attribution=%s reason=%s\n", NAMES[index], label, why);
}

void status_print_lines(unsigned index, const uint8_t *bytes, size_t size, FILE *stream,
                        struct status_command_result *command) {
    print_reply(index, bytes, size, 0, stream, command);
}

static const char *result_label(enum status_result result) {
    switch (result) {
    case STATUS_RESULT_OK: return "ok";
    case STATUS_RESULT_ERROR: return "error";
    case STATUS_RESULT_CME_ERROR: return "cme-error";
    default: return "none";
    }
}

/* One clock sample, or NULL when the clock cannot be trusted. */
static const uint64_t *clock_sample(const struct at_probe_backend *b, uint64_t *sample,
                                    const uint64_t *previous) {
    *sample = b->now_ms(b->context);
    if (*sample == AT_PROBE_CLOCK_ERROR || *sample == UINT64_MAX) return NULL;
    if (previous && *sample < *previous) return NULL;
    return sample;
}

/* Runs the listed fixed slots in order, each sent at most once.
 * Any slot that is not confirmed (own echo, then one final line, MATCHED, final OK)
 * ends the run: later slots are never sent. After a read timeout, or any other failure,
 * the same run does no further read, write or validate; only close/release run. */
static int run_slots(const struct at_probe_backend *b, struct status_probe_result *result, FILE *stream,
                     const unsigned *slots, unsigned count) {
    if (!slots || !count) return DJI_SYSTEM_ERROR;
    for (unsigned k = 0; k < count; k++) if (slots[k] >= STATUS_PROBE_SLOTS) return DJI_SYSTEM_ERROR;
    const unsigned first = slots[0];
    if (!b || !result || !b->now_ms || !b->cancelled || !b->prepare || !b->validate || !b->open ||
        !b->write || !b->read || !b->close || !b->release) return DJI_SYSTEM_ERROR;
    memset(result, 0, sizeof(*result));
    result->stop_index = STATUS_PROBE_SLOTS;
    const unsigned interface_id = AT_PROBE_INTERFACE;
    int status = DJI_OK, opened = 0, prepared = 0;
    uint8_t out_pipe = 0, in_pipe = 0;
    uint8_t buffer[AT_PROBE_READ_SIZE];
    uint8_t collected[AT_PROBE_ATI_BUFFER];
    if (b->cancelled(b->context)) { status = DJI_CANCELLED; result->stop = STATUS_STOP_CANCELLED; goto done; }
    prepared = 1;
    /* A failure before the first slot never leaves STOP_NONE ("all slots done"). */
    result->stop = STATUS_STOP_IO_ERROR;
    result->stop_index = first;
    status = b->prepare(b->context, interface_id);
    if (status != DJI_OK) goto release;
    status = b->validate(b->context, interface_id, 0, &out_pipe, &in_pipe);
    if (status != DJI_OK) goto release;
    if (b->cancelled(b->context)) { status = DJI_CANCELLED; result->stop = STATUS_STOP_CANCELLED; goto release; }
    status = b->open(b->context);
    if (status != DJI_OK) goto release;
    opened = 1;
    out_pipe = in_pipe = 0;
    status = b->validate(b->context, interface_id, 1, &out_pipe, &in_pipe);
    if (status == DJI_OK && (out_pipe == 0 || in_pipe == 0 || out_pipe == in_pipe))
        status = DJI_INVALID_DESCRIPTOR;
    if (status != DJI_OK) {
        if (stream) fputs("probe write=skipped reason=pipe-mismatch bytes=0\n", stream);
        goto release;
    }
    uint64_t clock = 0;
    if (!clock_sample(b, &clock, NULL)) { status = DJI_SYSTEM_ERROR; result->stop = STATUS_STOP_CLOCK_ERROR; goto release; }
    for (unsigned position = 0; position < count; position++) {
        const unsigned index = slots[position];
        struct status_command_result *command = &result->commands[index];
        result->stop_index = index;
        if (position) {
            uint8_t check_out = 0, check_in = 0;
            status = b->validate(b->context, interface_id, 1, &check_out, &check_in);
            if (status == DJI_OK && (check_out != out_pipe || check_in != in_pipe)) status = DJI_GONE;
            if (status != DJI_OK) { result->stop = STATUS_STOP_IO_ERROR; break; }
        }
        /* No IN read before each write. Last cancellation and clock checks right before sending. */
        if (b->cancelled(b->context)) { status = DJI_CANCELLED; result->stop = STATUS_STOP_CANCELLED; break; }
        /* Compare against the previous sample, not the slot being overwritten. */
        uint64_t sample = 0;
        if (!clock_sample(b, &sample, &clock)) {
            status = DJI_SYSTEM_ERROR; result->stop = STATUS_STOP_CLOCK_ERROR; break;
        }
        clock = sample;
        uint32_t size = 0;
        const uint8_t *bytes = at_probe_command_bytes(SLOT_COMMANDS[index], &size);
        if (!bytes) { status = DJI_SYSTEM_ERROR; result->stop = STATUS_STOP_IO_ERROR; break; }
        status = b->write(b->context, out_pipe, bytes, size, AT_PROBE_WRITE_MS);
        if (status != DJI_OK) {
            /* Delivery unknown. No retry, clear-stall, read or later command. */
            if (stream) fprintf(stream, "status cmd=%s write=%s bytes=0 delivery=unknown\n", NAMES[index],
                                status == AT_PROBE_TIMEOUT ? "timeout" : "error");
            if (status == AT_PROBE_TIMEOUT) status = DJI_SYSTEM_ERROR;
            result->stop = STATUS_STOP_WRITE_ERROR;
            break;
        }
        command->sent = 1;
        result->commands_sent++;
        if (stream) fprintf(stream, "status cmd=%s out=0x%02x in=0x%02x write=ok bytes=%u\n", NAMES[index],
                            AT_PROBE_OUT_ADDRESS, AT_PROBE_IN_ADDRESS, size);
        if (!clock_sample(b, &sample, &clock) || sample > UINT64_MAX - AT_PROBE_READ_TOTAL_MS) {
            status = DJI_SYSTEM_ERROR; result->stop = STATUS_STOP_CLOCK_ERROR; break;
        }
        clock = sample;
        size_t used = 0;
        int truncated = 0, read_attempts = 0, complete = 0;
        enum status_stop_reason read_stop = STATUS_STOP_READ_CAP;
        uint64_t deadline = clock + AT_PROBE_READ_TOTAL_MS;
        for (unsigned attempt = 0; attempt < AT_PROBE_READ_LIMIT; attempt++) {
            if (b->cancelled(b->context)) {
                status = DJI_CANCELLED; read_stop = STATUS_STOP_CANCELLED; break;
            }
            uint8_t check_out = 0, check_in = 0;
            status = b->validate(b->context, interface_id, 1, &check_out, &check_in);
            if (status == DJI_OK && (check_out != out_pipe || check_in != in_pipe)) status = DJI_GONE;
            if (status != DJI_OK) { read_stop = STATUS_STOP_IO_ERROR; break; }
            if (b->cancelled(b->context)) {
                status = DJI_CANCELLED; read_stop = STATUS_STOP_CANCELLED; break;
            }
            uint64_t now = 0;
            if (!clock_sample(b, &now, &clock)) {
                status = DJI_SYSTEM_ERROR; read_stop = STATUS_STOP_CLOCK_ERROR; break;
            }
            clock = now;
            if (now >= deadline) { read_stop = STATUS_STOP_TIME_BUDGET; break; }
            uint64_t remaining = deadline - now;
            if (remaining > AT_PROBE_READ_TOTAL_MS) {
                status = DJI_SYSTEM_ERROR; read_stop = STATUS_STOP_CLOCK_ERROR; break;
            }
            uint32_t timeout = (uint32_t)remaining;
            if (timeout == 0) { read_stop = STATUS_STOP_TIME_BUDGET; break; }
            uint32_t read_size = (uint32_t)sizeof(buffer);
            read_attempts++;
            status = b->read(b->context, in_pipe, buffer, &read_size, timeout);
            /* SDK: on error size/buffer are invalid and never inspected.
             * A timeout can leave the data toggle unsynchronized, so it ends the
             * whole run: no further read or write, close/release only. */
            if (status == AT_PROBE_TIMEOUT) {
                if (stream) fprintf(stream, "status cmd=%s read=%d status=timeout bytes=0\n", NAMES[index], read_attempts);
                status = DJI_OK;
                read_stop = STATUS_STOP_TIMEOUT;
                break;
            }
            if (status != DJI_OK) { read_stop = STATUS_STOP_IO_ERROR; break; }
            if (read_size == 0) { read_stop = STATUS_STOP_ZERO_COUNT; break; }
            if (read_size > sizeof(buffer)) { status = DJI_SYSTEM_ERROR; read_stop = STATUS_STOP_OVERSIZE; break; }
            if (b->cancelled(b->context)) { status = DJI_CANCELLED; read_stop = STATUS_STOP_CANCELLED; break; }
            command->reads++;
            used = at_probe_append(collected, sizeof(collected), used, buffer, read_size, &truncated);
            if (stream) fprintf(stream, "status cmd=%s read=%d bytes=%u\n", NAMES[index], read_attempts, read_size);
            /* Completion is own echo then a terminated final line, never an earlier final line. */
            if (analyze_reply(index, collected, used, REPLY_SKIP_PRE_ECHO, command)) {
                complete = 1; read_stop = STATUS_STOP_NONE; break;
            }
        }
        /* Collection facts are known here, before anything about the reply is printed. */
        unsigned flags = (status != DJI_OK ? REPLY_IO_ERROR : 0u) | (truncated ? REPLY_TRUNCATED : 0u) |
                         REPLY_SKIP_PRE_ECHO;
        print_reply(index, collected, used, flags, stream, command);
        command->complete = (unsigned)complete;
        if (stream && command->omitted) fprintf(stream, "status cmd=%s lines_omitted=%u\n", NAMES[index], command->omitted);
        if (stream && truncated) fprintf(stream, "status cmd=%s truncated=1\n", NAMES[index]);
        if (stream && command->pre_echo_skipped)
            fprintf(stream, "status cmd=%s pre_echo_skipped=%u\n", NAMES[index], command->pre_echo_skipped);
        if (status != DJI_OK || !complete) { result->stop = read_stop; break; }
        if (command->attribution != STATUS_ATTRIBUTION_MATCHED) { result->stop = STATUS_STOP_UNCONFIRMED; break; }
        if (command->result != STATUS_RESULT_OK) { result->stop = STATUS_STOP_FINAL_ERROR; break; }
        result->stop = STATUS_STOP_NONE;
        result->stop_index = STATUS_PROBE_SLOTS;
    }
release:
    if (status == DJI_OK && b->cancelled(b->context)) {
        status = DJI_CANCELLED;
        if (result->stop == STATUS_STOP_NONE) result->stop = STATUS_STOP_CANCELLED;
    }
    if (opened) {
        int close_status = b->close(b->context);
        if (close_status != DJI_OK) status = DJI_CLOSE_ERROR;
    }
    if (prepared) b->release(b->context);
done:
    if (stream) {
        for (unsigned position = 0; position < count; position++) {
            const unsigned index = slots[position];
            const struct status_command_result *command = &result->commands[index];
            fprintf(stream, "status cmd=%s sent=%u reads=%u result=%s", NAMES[index], command->sent,
                    command->reads, result_label(command->result));
            if (index == STATUS_PROBE_PDP_INDEX) fprintf(stream, " contexts=%u", command->contexts);
            if (index == STATUS_PROBE_DATA_SUPPORT_INDEX)
                fprintf(stream, " support_lists=%u ppp_connection=unproven", command->support_lists);
            fputc('\n', stream);
        }
        const unsigned stop = result->stop;
        const char *stop_label = stop < sizeof(STOPS) / sizeof(STOPS[0]) ? STOPS[stop] : "unknown";
        fprintf(stream, "probe status=%d commands_sent=%u stop=%s stop_index=%u evidence=format-only protocol=unconfirmed\n",
                status, result->commands_sent, stop_label, result->stop_index);
    }
    return status;
}

int status_probe_run(const struct at_probe_backend *b, struct status_probe_result *result, FILE *stream) {
    static const unsigned slots[STATUS_PROBE_COMMANDS] = {0, 1, 2, 3};
    return run_slots(b, result, stream, slots, STATUS_PROBE_COMMANDS);
}

int pdp_probe_run(const struct at_probe_backend *b, struct status_probe_result *result, FILE *stream) {
    static const unsigned slots[1] = {STATUS_PROBE_PDP_INDEX};
    return run_slots(b, result, stream, slots, 1);
}

int data_support_probe_run(const struct at_probe_backend *b, struct status_probe_result *result, FILE *stream) {
    static const unsigned slots[1] = {STATUS_PROBE_DATA_SUPPORT_INDEX};
    return run_slots(b, result, stream, slots, 1);
}

static int discard_write(void *cookie, const char *bytes, int size) {
    (void)cookie; (void)bytes;
    return size;
}

int signal_probe_run(const struct at_probe_backend *b, struct status_probe_result *result, FILE *stream) {
    static const unsigned slots[2] = {1, STATUS_PROBE_CESQ_INDEX};
    /* Values are kept while lines are printed, so a sink stands in for no stream. */
    FILE *sink = stream ? stream : funopen(NULL, NULL, discard_write, NULL, NULL);
    if (!sink) {
        if (result) { memset(result, 0, sizeof(*result)); result->stop = STATUS_STOP_IO_ERROR; }
        return DJI_SYSTEM_ERROR;
    }
    int status = run_slots(b, result, sink, slots, 2);
    if (!stream && fclose(sink) != 0 && status == DJI_OK) status = DJI_SYSTEM_ERROR;
    return status;
}
