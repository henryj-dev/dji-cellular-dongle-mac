#include "at_probe.h"
#include "discovery.h"
#include "payload_analysis.h"

#include <string.h>

/* The only bytes this program may ever send. Not derived from argv, files or replies. */
static const uint8_t AT_PROBE[3] = {'A', 'T', '\r'};
static const uint8_t ATI_PROBE[4] = {'A', 'T', 'I', '\r'};
/* Read-only status queries; none of them changes modem state. */
static const uint8_t CPIN_PROBE[9] = {'A', 'T', '+', 'C', 'P', 'I', 'N', '?', '\r'};
static const uint8_t CSQ_PROBE[7] = {'A', 'T', '+', 'C', 'S', 'Q', '\r'};
static const uint8_t COPS_PROBE[9] = {'A', 'T', '+', 'C', 'O', 'P', 'S', '?', '\r'};
static const uint8_t CEREG_PROBE[10] = {'A', 'T', '+', 'C', 'E', 'R', 'E', 'G', '?', '\r'};
/* Read-only PDP context query; never a set command. */
static const uint8_t CGDCONT_PROBE[12] = {'A', 'T', '+', 'C', 'G', 'D', 'C', 'O', 'N', 'T', '?', '\r'};
/* Capability test only: AT(2) + +(1) + CGDATA(6) + =?(2) + CR(1) = 12 bytes. */
static const uint8_t CGDATA_TEST_PROBE[12] = {'A', 'T', '+', 'C', 'G', 'D', 'A', 'T', 'A', '=', '?', '\r'};

const uint8_t *at_probe_command_bytes(enum at_probe_command command, uint32_t *size) {
    switch (command) {
    case AT_PROBE_COMMAND_AT: *size = (uint32_t)sizeof(AT_PROBE); return AT_PROBE;
    case AT_PROBE_COMMAND_ATI: *size = (uint32_t)sizeof(ATI_PROBE); return ATI_PROBE;
    case AT_PROBE_COMMAND_CPIN: *size = (uint32_t)sizeof(CPIN_PROBE); return CPIN_PROBE;
    case AT_PROBE_COMMAND_CSQ: *size = (uint32_t)sizeof(CSQ_PROBE); return CSQ_PROBE;
    case AT_PROBE_COMMAND_COPS: *size = (uint32_t)sizeof(COPS_PROBE); return COPS_PROBE;
    case AT_PROBE_COMMAND_CEREG: *size = (uint32_t)sizeof(CEREG_PROBE); return CEREG_PROBE;
    case AT_PROBE_COMMAND_CGDCONT: *size = (uint32_t)sizeof(CGDCONT_PROBE); return CGDCONT_PROBE;
    case AT_PROBE_COMMAND_CGDATA_TEST: *size = (uint32_t)sizeof(CGDATA_TEST_PROBE); return CGDATA_TEST_PROBE;
    }
    *size = 0;
    return NULL;
}

int at_probe_is_fixed_command(const uint8_t *bytes, uint32_t size) {
    for (int command = AT_PROBE_COMMAND_AT; command <= AT_PROBE_COMMAND_CGDATA_TEST; command++) {
        uint32_t fixed_size = 0;
        const uint8_t *fixed = at_probe_command_bytes((enum at_probe_command)command, &fixed_size);
        if (fixed && bytes == fixed && size == fixed_size) return 1;
    }
    return 0;
}

size_t at_probe_append(uint8_t *dest, size_t cap, size_t used, const uint8_t *src, size_t size,
                       int *truncated) {
    if (used > cap) used = cap;
    size_t room = cap - used, take = size < room ? size : room;
    if (take != 0) memcpy(dest + used, src, take);
    if (take < size) *truncated = 1;
    return used + take;
}

static int ati_allowed(uint8_t ch) {
    static const char symbols[] = " .,:;-_/+()=#";
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) return 1;
    return ch != 0 && memchr(symbols, ch, sizeof(symbols) - 1) != NULL;
}

static enum ati_result ati_line_result(const uint8_t *line, size_t length) {
    static const char cme[] = "+CME ERROR:";
    if (length == 2 && memcmp(line, "OK", 2) == 0) return ATI_RESULT_OK;
    if (length == 5 && memcmp(line, "ERROR", 5) == 0) return ATI_RESULT_ERROR;
    if (length >= sizeof(cme) - 1 && memcmp(line, cme, sizeof(cme) - 1) == 0) return ATI_RESULT_ERROR;
    return ATI_RESULT_NONE;
}

/* Allowed-only line. A long digit run is replaced as a whole and never partially shown.
 * The joined buffer's first and last bytes are unknown-length boundaries (queued data
 * before the write, deadline/limit/error/cancel/truncation after it), so a digit run
 * touching either is always masked as <digits:N+>, whatever its visible length. */
static void ati_print_text(unsigned number, const uint8_t *line, size_t length,
                           int at_start, int at_end, FILE *stream) {
    char out[AT_PROBE_ATI_MAX_TEXT + 1];
    size_t used = 0, index = 0;
    int cut = 0;
    while (index < length && !cut) {
        size_t run = 0;
        while (index + run < length && line[index + run] >= '0' && line[index + run] <= '9') run++;
        int boundary = run != 0 && ((at_start && index == 0) || (at_end && index + run == length));
        if (run >= AT_PROBE_ATI_DIGIT_RUN || boundary) {
            char marker[32];
            int written = snprintf(marker, sizeof(marker), boundary ? "<digits:%zu+>" : "<digits:%zu>", run);
            if (written < 0 || used + (size_t)written > AT_PROBE_ATI_MAX_TEXT) { cut = 1; break; }
            memcpy(out + used, marker, (size_t)written);
            used += (size_t)written;
            index += run;
            continue;
        }
        size_t span = run ? run : 1;
        for (size_t k = 0; k < span; k++) {
            if (used >= AT_PROBE_ATI_MAX_TEXT) { cut = 1; break; }
            out[used++] = (char)line[index + k];
        }
        index += span;
    }
    out[used] = '\0';
    fprintf(stream, "ati line=%u text=\"%s\"%s%s\n", number, out, cut ? " cut=1" : "",
            at_end ? " partial=1" : "");
}

void ati_print_lines(const uint8_t *bytes, size_t size, FILE *stream, struct ati_summary *summary) {
    memset(summary, 0, sizeof(*summary));
    if (!bytes) return;
    size_t index = 0;
    while (index < size) {
        if (bytes[index] == '\r' || bytes[index] == '\n') { index++; continue; }
        size_t start = index;
        while (index < size && bytes[index] != '\r' && bytes[index] != '\n') index++;
        const uint8_t *line = bytes + start;
        size_t length = index - start;
        /* No CR/LF after this line: it reaches the unknown end boundary. */
        const int at_start = start == 0, at_end = index == size;
        const char *partial = at_end ? " partial=1" : "";
        enum ati_result result = ati_line_result(line, length);
        if (result != ATI_RESULT_NONE) summary->result = result;
        if (summary->lines >= AT_PROBE_ATI_MAX_LINES) { summary->omitted++; continue; }
        unsigned number = ++summary->lines;
        if (!stream) continue;
        if (length == 3 && memcmp(line, "ATI", 3) == 0) {
            fprintf(stream, "ati line=%u echo=1%s\n", number, partial);
            continue;
        }
        if (result != ATI_RESULT_NONE) {
            fprintf(stream, "ati line=%u result=%s%s\n", number, result == ATI_RESULT_OK ? "ok" : "error",
                    partial);
            continue;
        }
        int allowed = 1;
        for (size_t k = 0; k < length && allowed; k++) allowed = ati_allowed(line[k]);
        if (!allowed) fprintf(stream, "ati line=%u redacted=disallowed length=%zu%s\n", number, length, partial);
        else ati_print_text(number, line, length, at_start, at_end, stream);
    }
}

enum at_probe_reply at_probe_classify(const uint8_t *bytes, size_t size) {
    static const uint8_t ok[] = {'\r', '\n', 'O', 'K', '\r', '\n'};
    static const uint8_t echo_ok[] = {'A', 'T', '\r', '\r', '\n', 'O', 'K', '\r', '\n'};
    if (!bytes || size == 0) return AT_REPLY_UNKNOWN;
    if (size == sizeof(ok) && memcmp(bytes, ok, sizeof(ok)) == 0) return AT_REPLY_OK;
    if (size == sizeof(echo_ok) && memcmp(bytes, echo_ok, sizeof(echo_ok)) == 0) return AT_REPLY_ECHO_OK;
    /* Reuse the existing whole-read rules; OK was matched exactly above. */
    struct payload_analysis analysis = payload_classify(bytes, size);
    if (analysis.candidate == PAYLOAD_AT_FIXED_TOKEN) return AT_REPLY_ERROR;
    if (analysis.candidate == PAYLOAD_CRLF_LINE) return AT_REPLY_CRLF_LINE;
    return AT_REPLY_UNKNOWN;
}

const char *at_probe_reply_label(enum at_probe_reply reply) {
    switch (reply) {
    case AT_REPLY_NONE: return "none";
    case AT_REPLY_OK: return "ok";
    case AT_REPLY_ECHO_OK: return "echo-ok";
    case AT_REPLY_ERROR: return "error";
    case AT_REPLY_CRLF_LINE: return "crlf-line";
    default: return "unknown";
    }
}

int at_probe_run(const struct at_probe_backend *b, struct at_probe_result *result, FILE *stream) {
    return at_probe_run_command(b, AT_PROBE_COMMAND_AT, result, stream);
}

int at_probe_run_command(const struct at_probe_backend *b, enum at_probe_command command,
                         struct at_probe_result *result, FILE *stream) {
    uint32_t probe_size = 0;
    const uint8_t *probe = command <= AT_PROBE_COMMAND_ATI ? at_probe_command_bytes(command, &probe_size) : NULL;
    if (!b || !result || !probe || !b->now_ms || !b->cancelled || !b->prepare || !b->validate || !b->open ||
        !b->write || !b->read || !b->close || !b->release) return DJI_SYSTEM_ERROR;
    const int ati = command == AT_PROBE_COMMAND_ATI;
    uint8_t collected[AT_PROBE_ATI_BUFFER];
    memset(result, 0, sizeof(*result));
    result->reply = AT_REPLY_NONE;
    const unsigned interface_id = AT_PROBE_INTERFACE;
    int status = DJI_OK, opened = 0, prepared = 0;
    uint8_t out_pipe = 0, in_pipe = 0;
    uint8_t buffer[AT_PROBE_READ_SIZE];
    if (b->cancelled(b->context)) { status = DJI_CANCELLED; goto done; }
    prepared = 1;
    status = b->prepare(b->context, interface_id);
    if (status != DJI_OK) goto release;
    status = b->validate(b->context, interface_id, 0, &out_pipe, &in_pipe);
    if (status != DJI_OK) goto release;
    if (b->cancelled(b->context)) { status = DJI_CANCELLED; goto release; }
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
    /* No IN read before the write. Last cancellation check right before sending. */
    if (b->cancelled(b->context)) { status = DJI_CANCELLED; goto release; }
    result->writes++;
    status = b->write(b->context, out_pipe, probe, probe_size, AT_PROBE_WRITE_MS);
    if (status != DJI_OK) {
        /* SDK returns no byte count: delivery is unknown. No retry, clear-stall or read. */
        if (stream) fprintf(stream, "probe interface=%u out=0x%02x in=0x%02x write=%s bytes=0 delivery=unknown\n",
                            interface_id, AT_PROBE_OUT_ADDRESS, AT_PROBE_IN_ADDRESS,
                            status == AT_PROBE_TIMEOUT ? "timeout" : "error");
        if (status == AT_PROBE_TIMEOUT) status = DJI_SYSTEM_ERROR;
        goto release;
    }
    result->sent = 1;
    if (stream) fprintf(stream, "probe interface=%u out=0x%02x in=0x%02x write=ok bytes=%u\n",
                        interface_id, AT_PROBE_OUT_ADDRESS, AT_PROBE_IN_ADDRESS, (unsigned)probe_size);
    uint64_t deadline = b->now_ms(b->context) + AT_PROBE_READ_TOTAL_MS;
    for (unsigned index = 0; index < AT_PROBE_READ_LIMIT; index++) {
        if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
        uint8_t check_out = 0, check_in = 0;
        status = b->validate(b->context, interface_id, 1, &check_out, &check_in);
        if (status == DJI_OK && (check_out != out_pipe || check_in != in_pipe)) status = DJI_GONE;
        if (status != DJI_OK) break;
        if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
        uint64_t now = b->now_ms(b->context);
        if (now >= deadline) break;
        uint32_t timeout = (uint32_t)(deadline - now);
        uint32_t size = (uint32_t)sizeof(buffer);
        result->read_attempts++;
        status = b->read(b->context, in_pipe, buffer, &size, timeout);
        /* SDK: on error size/buffer are invalid and never inspected. */
        if (status == AT_PROBE_TIMEOUT) {
            result->timeouts++;
            if (stream) fprintf(stream, "probe read=%u status=timeout bytes=0\n", result->read_attempts);
            status = DJI_OK;
            break;
        }
        if (status != DJI_OK) break;
        if (size > sizeof(buffer)) { status = DJI_SYSTEM_ERROR; break; }
        if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
        result->reads++;
        if (ati) {
            /* Only successful reads are joined; nothing beyond the 2048-byte buffer is written. */
            result->ati_bytes = at_probe_append(collected, sizeof(collected), result->ati_bytes,
                                                buffer, size, &result->truncated);
            if (stream) fprintf(stream, "probe read=%u bytes=%u\n", result->read_attempts, size);
            if (size == 0) break;
            continue;
        }
        enum at_probe_reply reply = at_probe_classify(buffer, size);
        if (result->reply == AT_REPLY_NONE ||
            (result->reply == AT_REPLY_UNKNOWN && reply != AT_REPLY_UNKNOWN)) result->reply = reply;
        if (stream) {
            fprintf(stream, "probe read=%u bytes=%u probe_reply=%s\n", result->read_attempts, size,
                    at_probe_reply_label(reply));
            if (size != 0) payload_analyze_read(buffer, size, stream);
        }
        if (size == 0) break;
    }
release:
    if (status == DJI_OK && b->cancelled(b->context)) status = DJI_CANCELLED;
    if (opened) {
        int close_status = b->close(b->context);
        if (close_status != DJI_OK) status = DJI_CLOSE_ERROR;
    }
    if (prepared) b->release(b->context);
done:
    if (ati) {
        ati_print_lines(collected, result->ati_bytes, stream, &result->ati);
        static const char *const results[] = {"none", "ok", "error"};
        unsigned index = result->ati.result <= ATI_RESULT_ERROR ? (unsigned)result->ati.result : 0u;
        if (stream && result->ati.omitted)
            fprintf(stream, "ati lines_omitted=%u\n", result->ati.omitted);
        if (stream) fprintf(stream, "probe status=%d sent=%u reads=%u lines=%u result=%s truncated=%d evidence=format-only protocol=unconfirmed\n",
                            status, result->sent, result->reads, result->ati.lines, results[index],
                            result->truncated ? 1 : 0);
        return status;
    }
    if (stream) fprintf(stream, "probe status=%d sent=%u reads=%u reply=%s evidence=format-only protocol=unconfirmed\n",
                        status, result->sent, result->reads, at_probe_reply_label(result->reply));
    return status;
}
