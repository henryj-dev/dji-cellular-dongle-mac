#include "../src/payload_analysis.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

static void expect(const uint8_t *bytes, size_t size, enum payload_candidate candidate,
                   enum payload_completeness completeness) {
    struct payload_analysis result = payload_classify(bytes, size);
    CHECK(result.bytes == size);
    CHECK(result.examined);
    if (result.candidate != candidate || result.completeness != completeness)
        fprintf(stderr, "classification mismatch bytes=%zu expected=%d/%d actual=%d/%d\n",
                size, candidate, completeness, result.candidate, result.completeness);
    CHECK(result.candidate == candidate);
    CHECK(result.completeness == completeness);
    CHECK(result.printable_bytes + result.control_bytes + result.high_bytes == size);
}

int main(void) {
    /* Synthetic CTL response with one TLV, not a recorded device payload. */
    const uint8_t ctl[] = {1, 16, 0, 0x80, 0, 0, 1, 1, 1, 0, 5, 0, 1, 2, 0, 0, 0};
    const uint8_t service[] = {1, 12, 0, 0x80, 1, 1, 2, 1, 0, 1, 0, 0, 0};
    expect(ctl, sizeof(ctl), PAYLOAD_QMUX_QMI, PAYLOAD_COMPLETE);
    expect(service, sizeof(service), PAYLOAD_QMUX_QMI, PAYLOAD_COMPLETE);
    expect(NULL, 0, PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    CHECK(!payload_classify(NULL, 1).examined);
    CHECK(!payload_classify(ctl, PAYLOAD_ANALYSIS_LIMIT + 1).examined);
    for (size_t i = 0; i < sizeof(ctl); i++) {
        /* Exact-sized object catches any access beyond actual size under ASan. */
        uint8_t prefix[i ? i : 1];
        memcpy(prefix, ctl, i);
        expect(prefix, i, i >= 6 ? PAYLOAD_QMUX_QMI : PAYLOAD_UNKNOWN,
               i >= 6 ? PAYLOAD_INCOMPLETE : PAYLOAD_NOT_APPLICABLE);
    }
    for (size_t i = 0; i < sizeof(service); i++)
        expect(service, i, i >= 6 ? PAYLOAD_QMUX_QMI : PAYLOAD_UNKNOWN,
               i >= 6 ? PAYLOAD_INCOMPLETE : PAYLOAD_NOT_APPLICABLE);
    uint8_t altered[sizeof(ctl) + 1];
    memcpy(altered, ctl, sizeof(ctl));
    altered[sizeof(ctl)] = 0;
    expect(altered, sizeof(altered), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[1] = 15;
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[1] = 17;
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[1] = 16; altered[10] = 6;
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[10] = 5; altered[13] = 3;
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[13] = 2; altered[14] = 1; /* TLV length is little endian. */
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[14] = 0; altered[1] = 0;
    expect(altered, 6, PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[1] = 16; altered[2] = 1;
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    altered[0] = 2;
    expect(altered, sizeof(ctl), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    /* Declared frame has only 1/2 bytes for a TLV header: impossible, not incomplete. */
    for (uint8_t tail = 1; tail <= 2; tail++) {
        memcpy(altered, ctl, sizeof(ctl));
        altered[1] = (uint8_t)(11 + tail); altered[10] = tail;
        expect(altered, (size_t)12 + tail, PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    }
    uint8_t boundary[PAYLOAD_ANALYSIS_LIMIT] = {1, 0xff, 0x0f, 0x80, 1, 1, 2, 1, 0, 1, 0, 0xf3, 0x0f, 1, 0xf0, 0x0f};
    expect(boundary, sizeof(boundary), PAYLOAD_QMUX_QMI, PAYLOAD_COMPLETE);
    for (size_t i = 13; i < sizeof(boundary); i++)
        expect(boundary, i, PAYLOAD_QMUX_QMI, PAYLOAD_INCOMPLETE);
    uint8_t empty_ctl[] = {1, 11, 0, 0x80, 0, 0, 1, 1, 1, 0, 0, 0};
    expect(empty_ctl, sizeof(empty_ctl), PAYLOAD_QMUX_QMI, PAYLOAD_COMPLETE);
    /* Fragments are independent; neither a suffix nor printable data proves a protocol. */
    expect(ctl + 9, sizeof(ctl) - 9, PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    /* A suffix can accidentally resemble an incomplete header: still format-only. */
    expect(ctl + 6, sizeof(ctl) - 6, PAYLOAD_QMUX_QMI, PAYLOAD_INCOMPLETE);
    const char *tokens[] = {"\r\nOK\r\n", "\r\nERROR\r\n", "\r\n+CME ERROR: synthetic-private-value\r\n"};
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        size_t length = strlen(tokens[i]);
        expect((const uint8_t *)tokens[i], length, PAYLOAD_AT_FIXED_TOKEN, PAYLOAD_COMPLETE);
        for (size_t j = 0; j < length; j++)
            expect((const uint8_t *)tokens[i], j, PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    }
    const char *unknown[] = {"OK", "ERROR",
        "\r\nOK\r\nERROR\r\n", "\r\n+CME ERROR: x\nvalue\r\n",
        "synthetic-private-value", "\nOK\n", "\r\nO", "K\r\n", "\r\nOK\r\nextra"};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++)
        expect((const uint8_t *)unknown[i], strlen(unknown[i]), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);

    /* Synthetic lines, not recorded device data. Whole read only; first-char class only. */
    const struct { const char *text; enum payload_leading leading; } lines[] = {
        {"\r\n+synthetic-plus-line\r\n", PAYLOAD_LEADING_PLUS},
        {"\r\n^synthetic-caret-line\r\n", PAYLOAD_LEADING_CARET},
        {"\r\nSynthetic-upper-line\r\n", PAYLOAD_LEADING_UPPER},
        {"\r\n7synthetic-digit-line\r\n", PAYLOAD_LEADING_DIGIT},
        {"\r\n synthetic-other-line\r\n", PAYLOAD_LEADING_OTHER},
        {"\r\nsynthetic-lower-line\r\n", PAYLOAD_LEADING_OTHER},
        {"\r\n~\r\n", PAYLOAD_LEADING_OTHER},
        {"\r\nZ\r\n", PAYLOAD_LEADING_UPPER},
        /* Not one of the exact AT tokens, so only the generic line shape applies. */
        {"\r\nOKsuffix\r\n", PAYLOAD_LEADING_UPPER},
        {"\r\nprefixOK\r\n", PAYLOAD_LEADING_OTHER},
        {"\r\n+CME ERROR:\r\n", PAYLOAD_LEADING_PLUS}};
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        size_t length = strlen(lines[i].text);
        expect((const uint8_t *)lines[i].text, length, PAYLOAD_CRLF_LINE, PAYLOAD_COMPLETE);
        struct payload_analysis line = payload_classify((const uint8_t *)lines[i].text, length);
        CHECK(line.line_length == length - 4 && line.leading == lines[i].leading);
        for (size_t j = 0; j < length; j++) {
            uint8_t prefix[j ? j : 1];
            memcpy(prefix, lines[i].text, j);
            expect(prefix, j, PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
        }
    }
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        struct payload_analysis token = payload_classify((const uint8_t *)tokens[i], strlen(tokens[i]));
        CHECK(token.candidate == PAYLOAD_AT_FIXED_TOKEN && token.line_length == 0 &&
              token.leading == PAYLOAD_LEADING_NONE);
    }
    const char *not_lines[] = {"\r\n\r\n", "\r\r", "\n\n", "\rA\r", "\nA\n", "\r\nA\r", "\r\nA\n",
        "\rA\r\n", "\nA\r\n", "x\r\nA\r\n", "\r\nA\r\nx", " \r\nA\r\n", "\r\nA\r\n ",
        "\r\nA\r\n\r\nB\r\n", "\r\nA\r\nB\r\n", "\r\nA\rB\r\n", "\r\nA\nB\r\n", "\r\nA\tB\r\n",
        "\r\n\r\nA\r\n", "\r\nA\r\n\r\n", "\r\n\r\n\r\n"};
    for (size_t i = 0; i < sizeof(not_lines) / sizeof(not_lines[0]); i++)
        expect((const uint8_t *)not_lines[i], strlen(not_lines[i]), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    const uint8_t rejected_bytes[] = {0x00, 0x1b, 0x7f, 0x80, 0xff};
    for (size_t i = 0; i < sizeof(rejected_bytes); i++) {
        uint8_t line[] = {'\r', '\n', 'A', rejected_bytes[i], 'B', '\r', '\n'};
        expect(line, sizeof(line), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
        uint8_t single[] = {'\r', '\n', rejected_bytes[i], '\r', '\n'};
        expect(single, sizeof(single), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);
    }
    /* A line exactly at the 4096-byte analysis limit is still one read. */
    uint8_t long_line[PAYLOAD_ANALYSIS_LIMIT];
    memset(long_line, 'x', sizeof(long_line));
    memcpy(long_line, "\r\n", 2);
    memcpy(long_line + sizeof(long_line) - 2, "\r\n", 2);
    expect(long_line, sizeof(long_line), PAYLOAD_CRLF_LINE, PAYLOAD_COMPLETE);
    CHECK(payload_classify(long_line, sizeof(long_line)).line_length == PAYLOAD_ANALYSIS_LIMIT - 4);
    const uint8_t mixed[] = {0, 0x1f, 0x20, 0x7e, 0x7f, 0x80, 0xff};
    struct payload_analysis result = payload_classify(mixed, sizeof(mixed));
    CHECK(result.printable_bytes == 2 && result.control_bytes == 3 && result.high_bytes == 2);
    memset(boundary, 0xa5, sizeof(boundary));
    expect(boundary, sizeof(boundary), PAYLOAD_UNKNOWN, PAYLOAD_NOT_APPLICABLE);

    FILE *output = tmpfile();
    CHECK(output != NULL);
    if (output) {
        for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++)
            payload_analyze_read((const uint8_t *)tokens[i], strlen(tokens[i]), output);
        payload_analyze_read((const uint8_t *)unknown[4], strlen(unknown[4]), output);
        for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
            payload_analyze_read((const uint8_t *)lines[i].text, strlen(lines[i].text), output);
        payload_analyze_read(ctl, sizeof(ctl), output);
        payload_analyze_read(ctl, 8, output);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        char text[8192];
        size_t length = fread(text, 1, sizeof(text) - 1, output);
        text[length] = '\0';
        CHECK(strstr(text, "synthetic-private-value") == NULL);
        CHECK(strstr(text, "CME") == NULL && strstr(text, "ERROR") == NULL && strstr(text, "OK") == NULL);
        CHECK(strstr(text, "candidate=at-fixed-token") != NULL);
        CHECK(strstr(text, "candidate=qmux-qmi") != NULL);
        CHECK(strstr(text, "candidate=unknown") != NULL);
        CHECK(strstr(text, "completeness=incomplete") != NULL);
        CHECK(strstr(text, "evidence=format-only protocol=unconfirmed") != NULL);
        CHECK(strstr(text, "candidate=crlf-line completeness=complete") != NULL);
        CHECK(strstr(text, "line_length=20 leading=plus") != NULL);
        CHECK(strstr(text, "leading=caret") != NULL && strstr(text, "leading=upper") != NULL);
        CHECK(strstr(text, "leading=digit") != NULL && strstr(text, "leading=other") != NULL);
        CHECK(strstr(text, "line_length=1 leading=upper") != NULL);
        CHECK(strstr(text, "synthetic") == NULL && strstr(text, "suffix") == NULL &&
              strstr(text, "prefix") == NULL && strstr(text, "Synthetic") == NULL);
        CHECK(strchr(text, '\r') == NULL && strchr(text, '~') == NULL && strchr(text, '^') == NULL &&
              strchr(text, '+') == NULL);
        /* Every crlf-line label is followed by the unchanged evidence suffix. */
        for (const char *at = strstr(text, "candidate=crlf-line"); at; at = strstr(at + 1, "candidate=crlf-line")) {
            const char *end = strchr(at, '\n');
            const char *evidence = strstr(at, " evidence=format-only protocol=unconfirmed\n");
            CHECK(end && evidence && evidence < end);
        }
        CHECK(fclose(output) == 0);
    }
    printf("payload analysis checks=%u failures=%u (synthetic bytes, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
