#include "payload_analysis.h"

#include <string.h>

static size_t little16(const uint8_t *bytes) {
    return (size_t)bytes[0] | ((size_t)bytes[1] << 8);
}

/* libqmi 1.36.0 qmi-message.c/.h: marker 1, packed QMUX header 5,
 * CTL header 6 / service header 7, TLV header 3, all lengths LE.
 * Outer length excludes the marker. No identifiers or TLV values decoded.
 * Require a whole QMUX header; a marker alone is not enough evidence. */
static enum payload_completeness qmux_candidate(const uint8_t *bytes, size_t size) {
    if (size < 6 || bytes[0] != 1) return PAYLOAD_NOT_APPLICABLE;
    size_t total = little16(bytes + 1) + 1;
    size_t header = bytes[4] == 0 ? 12u : 13u;
    if (total < header || size > total) return PAYLOAD_NOT_APPLICABLE;
    if (size < header) return PAYLOAD_INCOMPLETE;
    if (little16(bytes + header - 2) != total - header) return PAYLOAD_NOT_APPLICABLE;
    size_t offset = header;
    while (offset < total) {
        if (total - offset < 3) return PAYLOAD_NOT_APPLICABLE;
        if (size - offset < 3) return PAYLOAD_INCOMPLETE;
        size_t value_size = little16(bytes + offset + 1);
        if (value_size > total - offset - 3) return PAYLOAD_NOT_APPLICABLE;
        offset += 3;
        if (value_size > size - offset) return PAYLOAD_INCOMPLETE;
        offset += value_size;
    }
    return size == total ? PAYLOAD_COMPLETE : PAYLOAD_INCOMPLETE;
}

/* Whole CRLF-delimited result lines only, never a substring or ASCII guess.
 * A CME error value is deliberately opaque and never returned or printed. */
static int at_fixed_token(const uint8_t *bytes, size_t size) {
    if (size < 6 || memcmp(bytes, "\r\n", 2) != 0 ||
        memcmp(bytes + size - 2, "\r\n", 2) != 0) return 0;
    bytes += 2;
    size -= 4;
    if ((size == 2 && memcmp(bytes, "OK", 2) == 0) ||
        (size == 5 && memcmp(bytes, "ERROR", 5) == 0)) return 1;
    static const char cme[] = "+CME ERROR:";
    if (size <= sizeof(cme) - 1 || memcmp(bytes, cme, sizeof(cme) - 1) != 0) return 0;
    for (size_t i = sizeof(cme) - 1; i < size; i++) {
        if (bytes[i] < 0x20 || bytes[i] > 0x7e) return 0;
    }
    return 1;
}

/* Exactly CR LF, one or more printable ASCII bytes, CR LF in this one read.
 * Only the line length and the first character's class are returned. */
static int crlf_line(const uint8_t *bytes, size_t size, size_t *length,
                     enum payload_leading *leading) {
    if (size < 5 || memcmp(bytes, "\r\n", 2) != 0 ||
        memcmp(bytes + size - 2, "\r\n", 2) != 0) return 0;
    for (size_t i = 2; i < size - 2; i++) {
        if (bytes[i] < 0x20 || bytes[i] > 0x7e) return 0;
    }
    uint8_t first = bytes[2];
    *length = size - 4;
    if (first == '+') *leading = PAYLOAD_LEADING_PLUS;
    else if (first == '^') *leading = PAYLOAD_LEADING_CARET;
    else if (first >= 'A' && first <= 'Z') *leading = PAYLOAD_LEADING_UPPER;
    else if (first >= '0' && first <= '9') *leading = PAYLOAD_LEADING_DIGIT;
    else *leading = PAYLOAD_LEADING_OTHER;
    return 1;
}

struct payload_analysis payload_classify(const uint8_t *bytes, size_t size) {
    struct payload_analysis result = {.bytes = size};
    if ((!bytes && size != 0) || size > PAYLOAD_ANALYSIS_LIMIT) return result;
    result.examined = 1;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] >= 0x20 && bytes[i] <= 0x7e) result.printable_bytes++;
        else if (bytes[i] < 0x20 || bytes[i] == 0x7f) result.control_bytes++;
        else result.high_bytes++;
    }
    if (size == 0) return result;
    result.completeness = qmux_candidate(bytes, size);
    if (result.completeness != PAYLOAD_NOT_APPLICABLE) result.candidate = PAYLOAD_QMUX_QMI;
    else if (at_fixed_token(bytes, size)) {
        result.candidate = PAYLOAD_AT_FIXED_TOKEN;
        result.completeness = PAYLOAD_COMPLETE;
    } else if (crlf_line(bytes, size, &result.line_length, &result.leading)) {
        result.candidate = PAYLOAD_CRLF_LINE;
        result.completeness = PAYLOAD_COMPLETE;
    }
    return result;
}

void payload_analysis_print(const struct payload_analysis *analysis, FILE *stream) {
    if (!analysis || !stream) return;
    const char *candidate = "unknown", *complete = "not-applicable";
    if (analysis->candidate == PAYLOAD_QMUX_QMI) candidate = "qmux-qmi";
    else if (analysis->candidate == PAYLOAD_AT_FIXED_TOKEN) candidate = "at-fixed-token";
    else if (analysis->candidate == PAYLOAD_CRLF_LINE) candidate = "crlf-line";
    if (analysis->completeness == PAYLOAD_COMPLETE) complete = "complete";
    else if (analysis->completeness == PAYLOAD_INCOMPLETE) complete = "incomplete";
    fprintf(stream, "analysis bytes=%zu examined=%s candidate=%s completeness=%s printable=%zu control=%zu high=%zu",
            analysis->bytes, analysis->examined ? "yes" : "no", candidate, complete,
            analysis->printable_bytes, analysis->control_bytes, analysis->high_bytes);
    if (analysis->candidate == PAYLOAD_CRLF_LINE) {
        static const char *const leading[] = {"other", "plus", "caret", "upper", "digit", "other"};
        unsigned index = analysis->leading <= PAYLOAD_LEADING_OTHER ? (unsigned)analysis->leading : 0u;
        fprintf(stream, " line_length=%zu leading=%s", analysis->line_length, leading[index]);
    }
    fputs(" evidence=format-only protocol=unconfirmed\n", stream);
}

void payload_analyze_read(const uint8_t *bytes, size_t size, FILE *stream) {
    struct payload_analysis result = payload_classify(bytes, size);
    payload_analysis_print(&result, stream);
}
