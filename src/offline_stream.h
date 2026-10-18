#ifndef DJI_OFFLINE_STREAM_H
#define DJI_OFFLINE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define OFFLINE_CONTROL_MAX 2048u
#define OFFLINE_QUEUE_MAX 65536u

enum offline_result { OFFLINE_OK, OFFLINE_FULL, OFFLINE_INVALID, OFFLINE_OVERFLOW };
enum offline_io_code { OFFLINE_IO_OK, OFFLINE_IO_INTR, OFFLINE_IO_AGAIN,
                       OFFLINE_IO_EOF, OFFLINE_IO_ERROR };
enum offline_direction { OFFLINE_RX, OFFLINE_TX };
enum offline_state { OFFLINE_CONTROL_WAIT, OFFLINE_DATA_STREAM, OFFLINE_STOPPING,
                     OFFLINE_RECOVERY_UNKNOWN, OFFLINE_CLOSED };
enum offline_error { OFFLINE_ERROR_NONE, OFFLINE_ERROR_ARGUMENT,
    OFFLINE_ERROR_CONTROL, OFFLINE_ERROR_LIMIT, OFFLINE_ERROR_RESULT,
    OFFLINE_ERROR_IO, OFFLINE_ERROR_COUNT, OFFLINE_ERROR_OVERFLOW,
    OFFLINE_ERROR_TIMEOUT, OFFLINE_ERROR_CLOCK, OFFLINE_ERROR_CLOSE };
/* Final result line that ended CONTROL_WAIT with OFFLINE_ERROR_RESULT. */
enum offline_modem_result { OFFLINE_MODEM_NONE, OFFLINE_MODEM_ERROR, OFFLINE_MODEM_NO_CARRIER,
    OFFLINE_MODEM_NO_DIALTONE, OFFLINE_MODEM_BUSY, OFFLINE_MODEM_NO_ANSWER,
    OFFLINE_MODEM_CME, OFFLINE_MODEM_CMS };
/* The only echo line accepted before CONNECT: the fixed CGDATA hook. With ATE1
 * the modem returns "<command>\r" and then "\r\nCONNECT\r\n" (bare CR, no LF). */
#define OFFLINE_CGDATA_COMMAND "AT+CGDATA=\"PPP\",1"

/* Caller-owned, non-overlapping storage, 1..OFFLINE_QUEUE_MAX bytes. No allocation.
 * Enqueue is all-or-nothing. Peek returns one contiguous span; consume advances
 * only that span. accepted/delivered counters are checked before every operation.
 * Enqueue input must not overlap queue storage. Fields are visible for stack
 * allocation, not for caller mutation after init. */
struct offline_queue {
    unsigned char *storage;
    size_t capacity, head, length;
    uint64_t accepted, delivered;
};
int offline_queue_init(struct offline_queue *queue, unsigned char *storage, size_t capacity);
int offline_queue_enqueue(struct offline_queue *queue, const unsigned char *data, size_t length);
const unsigned char *offline_queue_peek(const struct offline_queue *queue, size_t *length);
int offline_queue_consume(struct offline_queue *queue, size_t length);

struct offline_io { enum offline_io_code code; size_t count; };
struct offline_backend {
    void *context;
    struct offline_io (*stdio_write)(void *context, const unsigned char *data, size_t length);
    struct offline_io (*usb_write)(void *context, const unsigned char *data, size_t length);
    uint64_t (*now_ms)(void *context);
    int (*close)(void *context);
    void (*release)(void *context);
};

/* Offline policy only. No USB/stdio backend, threads, PPP parser or launcher.
 * All calls are serialized and callbacks must not reenter. Storage and callback
 * context live through release. init validates configuration before taking ownership;
 * never reinitialize a live stream. close/release each represent one owned resource.
 * CRLF printable-ASCII lines only; exact CONNECT terminates control parsing.
 * rx is USB -> stdio, tx is stdio -> USB; ready gates both writes, not rx buffering.
 * Unknown recovery is sticky even after CLOSED; cleanup never proves AT/PDP recovery. */
struct offline_stream {
    struct offline_queue rx, tx;
    struct offline_backend backend;
    enum offline_state state;
    enum offline_error error;
    unsigned char control[OFFLINE_CONTROL_MAX];
    size_t control_bytes, line_length, in_flight;
    uint64_t deadline_ms, last_ms;
    int pending_cr, ready, recovery_unknown, drain_done, joined, cleanup_done;
    /* Diagnostics only: fixed labels and a 1..5 digit CME/CMS code, never text. */
    enum offline_modem_result modem_result;
    unsigned modem_code;
    int modem_code_valid, echo_seen;
};
int offline_stream_init(struct offline_stream *stream, const struct offline_backend *backend,
                        unsigned char *rx_storage, size_t rx_capacity,
                        unsigned char *tx_storage, size_t tx_capacity);
/* Read completion event, not a real read. consumed is always an exact input offset.
 * USB errors ignore data/count/requested entirely and make recovery unknown.
 * Successful counts must be 1..requested and backed by readable storage.
 * Retryable stdio read errors ignore data/count and leave state/offset unchanged.
 * Retain input[consumed..count), drain space, and resubmit that suffix BEFORE the
 * next read: the core does not own unaccepted bytes. A failed control byte is
 * included in consumed; no input is accepted after failure or drain completion.
 * During STOPPING only opaque termination-exchange bytes should be supplied;
 * the caller, not this byte core, separates these from new application traffic. */
int offline_stream_input(struct offline_stream *stream, enum offline_direction direction,
                         enum offline_io_code code, const unsigned char *data,
                         size_t count, size_t requested, size_t *consumed);
int offline_stream_ready(struct offline_stream *stream);
/* At most one synchronous callback per step. stdio partial successes consume only
 * count; INTR/AGAIN consume nothing. USB success must report the entire request;
 * ANY failed USB write (including INTR/AGAIN) is unknown, never retried. */
int offline_stream_step(struct offline_stream *stream, enum offline_direction direction);
/* Normal data-state cancellation starts a bounded drain, not a restoration claim.
 * Cancellation while still waiting for CONNECT makes recovery unknown.
 * poll enforces a nondecreasing fake clock, including after callback completion.
 * Timeout may leave bytes queued; unknown disables all further I/O. */
int offline_stream_stop(struct offline_stream *stream, uint64_t drain_ms);
int offline_stream_poll(struct offline_stream *stream);
int offline_stream_drain_complete(struct offline_stream *stream);
/* Explicit in-flight/join events model worker lifetime without creating threads.
 * No new begin after drain completion/unknown. Complete remains legal after failure.
 * join requires zero in-flight and a finished drain or failure. poll then cleans up. */
int offline_stream_begin(struct offline_stream *stream);
int offline_stream_complete(struct offline_stream *stream);
int offline_stream_join(struct offline_stream *stream);
/* Fixed labels and counts only; returns required bytes excluding NUL, or -1.
 * A short buffer is NUL-terminated when capacity>0. No raw/payload diagnostics. */
int offline_stream_diagnostic(const struct offline_stream *stream, char *buffer, size_t capacity);

#endif
