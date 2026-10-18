#ifndef DJI_AT_PROBE_H
#define DJI_AT_PROBE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Approved single probes: interface 2 only, one of two fixed source constants
 * (AT\r or ATI\r) chosen by CLI enum, sent at most once. */
#define AT_PROBE_INTERFACE 2u
#define AT_PROBE_OUT_ADDRESS 0x03u
#define AT_PROBE_IN_ADDRESS 0x84u
#define AT_PROBE_MAX_PACKET 512u
#define AT_PROBE_WRITE_MS 1000u
#define AT_PROBE_READ_TOTAL_MS 2000u
#define AT_PROBE_READ_LIMIT 4u
#define AT_PROBE_READ_SIZE 512u
/* --probe-ati only: successful reads joined in order, then split into lines. */
#define AT_PROBE_ATI_BUFFER 2048u
#define AT_PROBE_ATI_MAX_LINES 16u
#define AT_PROBE_ATI_MAX_TEXT 80u
#define AT_PROBE_ATI_DIGIT_RUN 8u

/* Internal transport status for a timed-out write or read; same value as the observer. */
#define AT_PROBE_TIMEOUT 100
/* now_ms failure sentinel shared with the synchronous USB backend. */
#define AT_PROBE_CLOCK_ERROR (UINT64_MAX / 2)

enum at_probe_command {
    AT_PROBE_COMMAND_AT,
    AT_PROBE_COMMAND_ATI,
    /* --probe-status only, sent in this fixed order. */
    AT_PROBE_COMMAND_CPIN,
    AT_PROBE_COMMAND_CSQ,
    AT_PROBE_COMMAND_COPS,
    AT_PROBE_COMMAND_CEREG,
    /* --probe-pdp only. */
    AT_PROBE_COMMAND_CGDCONT,
    /* --probe-data-support only: test command, never starts a data connection. */
    AT_PROBE_COMMAND_CGDATA_TEST
};

enum ati_result {
    ATI_RESULT_NONE,
    ATI_RESULT_OK,
    ATI_RESULT_ERROR
};

struct ati_summary {
    unsigned lines;
    unsigned omitted;
    enum ati_result result;
};

enum at_probe_reply {
    AT_REPLY_NONE,
    AT_REPLY_UNKNOWN,
    AT_REPLY_OK,
    AT_REPLY_ECHO_OK,
    AT_REPLY_ERROR,
    AT_REPLY_CRLF_LINE
};

/* Same transport contract as the observer, plus one bulk OUT write.
 * validate fills both pipe refs only when opened; it must recheck identity. */
struct at_probe_backend {
    void *context;
    uint64_t (*now_ms)(void *context);
    int (*cancelled)(void *context);
    int (*prepare)(void *context, unsigned interface_id);
    int (*validate)(void *context, unsigned interface_id, int opened,
                    uint8_t *out_pipe, uint8_t *in_pipe);
    int (*open)(void *context);
    int (*write)(void *context, uint8_t pipe, const uint8_t *bytes, uint32_t size, uint32_t timeout_ms);
    int (*read)(void *context, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout_ms);
    int (*close)(void *context);
    void (*release)(void *context);
};

struct at_probe_result {
    unsigned writes;
    unsigned sent;
    unsigned reads;
    unsigned read_attempts;
    unsigned timeouts;
    enum at_probe_reply reply;
    size_t ati_bytes;
    int truncated;
    struct ati_summary ati;
};

/* One borrowed successful read; nothing is retained or combined across reads. */
enum at_probe_reply at_probe_classify(const uint8_t *bytes, size_t size);
const char *at_probe_reply_label(enum at_probe_reply reply);
/* Fixed source bytes for a command, or NULL for an unknown enum value. */
const uint8_t *at_probe_command_bytes(enum at_probe_command command, uint32_t *size);
/* True only when bytes/size are exactly one entry of the fixed source table (pointer and length). */
int at_probe_is_fixed_command(const uint8_t *bytes, uint32_t size);
/* Appends at most cap - used bytes; sets *truncated when anything is dropped. Returns new used. */
size_t at_probe_append(uint8_t *dest, size_t cap, size_t used, const uint8_t *src, size_t size,
                       int *truncated);
/* CR/LF split, empty lines skipped, max 16 printed lines of max 80 chars.
 * Only lines of A-Z a-z 0-9 space . , : ; - _ / + ( ) = # are printed, with
 * 8+ digit runs replaced by <digits:N>. Digit runs touching the first or last byte
 * of bytes are always replaced by <digits:N+>; an unterminated last line gets partial=1.
 * Echo/OK/ERROR/+CME ERROR print labels only. */
void ati_print_lines(const uint8_t *bytes, size_t size, FILE *stream, struct ati_summary *summary);
/* --probe-at: output carries fixed labels and counts only, never reply bytes. */
int at_probe_run(const struct at_probe_backend *backend, struct at_probe_result *result, FILE *stream);
int at_probe_run_command(const struct at_probe_backend *backend, enum at_probe_command command,
                         struct at_probe_result *result, FILE *stream);

#endif
