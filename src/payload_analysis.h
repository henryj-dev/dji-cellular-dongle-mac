#ifndef DJI_PAYLOAD_ANALYSIS_H
#define DJI_PAYLOAD_ANALYSIS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PAYLOAD_ANALYSIS_LIMIT 4096u

enum payload_candidate {
    PAYLOAD_UNKNOWN,
    PAYLOAD_QMUX_QMI,
    PAYLOAD_AT_FIXED_TOKEN,
    PAYLOAD_CRLF_LINE
};

/* First-character class of a crlf-line candidate; no other content is kept. */
enum payload_leading {
    PAYLOAD_LEADING_NONE,
    PAYLOAD_LEADING_PLUS,
    PAYLOAD_LEADING_CARET,
    PAYLOAD_LEADING_UPPER,
    PAYLOAD_LEADING_DIGIT,
    PAYLOAD_LEADING_OTHER
};

enum payload_completeness {
    PAYLOAD_NOT_APPLICABLE,
    PAYLOAD_INCOMPLETE,
    PAYLOAD_COMPLETE
};

struct payload_analysis {
    size_t bytes;
    size_t printable_bytes;
    size_t control_bytes;
    size_t high_bytes;
    int examined;
    enum payload_candidate candidate;
    enum payload_completeness completeness;
    size_t line_length;
    enum payload_leading leading;
};

/* Borrowed successful-read bytes only. No retention, assembly or value decoding.
 * Inputs above the fixed limit are not inspected, not even as a prefix. */
struct payload_analysis payload_classify(const uint8_t *bytes, size_t size);
/* Only counts and compile-time labels may reach output, never input bytes. */
void payload_analysis_print(const struct payload_analysis *analysis, FILE *stream);
void payload_analyze_read(const uint8_t *bytes, size_t size, FILE *stream);

#endif
