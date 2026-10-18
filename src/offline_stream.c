#include "offline_stream.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int queue_valid(const struct offline_queue *q) {
    return q && q->storage && q->capacity && q->capacity <= OFFLINE_QUEUE_MAX &&
           q->head < q->capacity && q->length <= q->capacity &&
           q->delivered <= q->accepted && q->accepted - q->delivered == q->length;
}

int offline_queue_init(struct offline_queue *q, unsigned char *storage, size_t capacity) {
    if (!q || !storage || !capacity || capacity > OFFLINE_QUEUE_MAX) return OFFLINE_INVALID;
    *q = (struct offline_queue){.storage = storage, .capacity = capacity};
    return OFFLINE_OK;
}

int offline_queue_enqueue(struct offline_queue *q, const unsigned char *data, size_t length) {
    if (!queue_valid(q) || (!data && length)) return OFFLINE_INVALID;
    if (length > q->capacity - q->length) return OFFLINE_FULL;
    if (length > UINT64_MAX - q->accepted) return OFFLINE_OVERFLOW;
    size_t tail = (q->head + q->length) % q->capacity;
    size_t first = q->capacity - tail;
    if (first > length) first = length;
    if (first) memcpy(q->storage + tail, data, first);
    if (length > first) memcpy(q->storage, data + first, length - first);
    q->length += length;
    q->accepted += length;
    return OFFLINE_OK;
}

const unsigned char *offline_queue_peek(const struct offline_queue *q, size_t *length) {
    if (!length) return NULL;
    *length = 0;
    if (!queue_valid(q) || !q->length) return NULL;
    *length = q->capacity - q->head;
    if (*length > q->length) *length = q->length;
    return q->storage + q->head;
}

int offline_queue_consume(struct offline_queue *q, size_t length) {
    size_t available = 0;
    if (!queue_valid(q)) return OFFLINE_INVALID;
    (void)offline_queue_peek(q, &available);
    if (length > available) return OFFLINE_INVALID;
    if (length > UINT64_MAX - q->delivered) return OFFLINE_OVERFLOW;
    q->head = (q->head + length) % q->capacity;
    q->length -= length;
    q->delivered += length;
    return OFFLINE_OK;
}

static int fail(struct offline_stream *s, enum offline_error error) {
    if (s->error == OFFLINE_ERROR_NONE) s->error = error;
    s->recovery_unknown = 1;
    if (s->state != OFFLINE_CLOSED) s->state = OFFLINE_RECOVERY_UNKNOWN;
    return OFFLINE_INVALID;
}

static int direction_valid(enum offline_direction direction) {
    return direction == OFFLINE_RX || direction == OFFLINE_TX;
}

int offline_stream_init(struct offline_stream *s, const struct offline_backend *backend,
                        unsigned char *rx, size_t rx_capacity,
                        unsigned char *tx, size_t tx_capacity) {
    if (!s || !backend || !backend->stdio_write || !backend->usb_write ||
        !backend->now_ms || !backend->close || !backend->release || !rx || !tx ||
        !rx_capacity || !tx_capacity || rx_capacity > OFFLINE_QUEUE_MAX ||
        tx_capacity > OFFLINE_QUEUE_MAX) return OFFLINE_INVALID;
    uintptr_t r = (uintptr_t)rx, t = (uintptr_t)tx;
    if (rx_capacity > UINTPTR_MAX - r || tx_capacity > UINTPTR_MAX - t ||
        (r < t + tx_capacity && t < r + rx_capacity)) return OFFLINE_INVALID;
    struct offline_backend copy = *backend;
    *s = (struct offline_stream){.backend = copy, .state = OFFLINE_CONTROL_WAIT};
    (void)offline_queue_init(&s->rx, rx, rx_capacity);
    (void)offline_queue_init(&s->tx, tx, tx_capacity);
    return OFFLINE_OK;
}

static int line_equals(const struct offline_stream *s, const char *label) {
    size_t length = strlen(label);
    return s->line_length == length && !memcmp(s->control, label, length);
}

/* Exactly "CONNECT" or "CONNECT <1..10 digits>" (V.250 rate suffix). */
static int connect_line(const struct offline_stream *s) {
    if (s->line_length < 7 || memcmp(s->control, "CONNECT", 7)) return 0;
    if (s->line_length == 7) return 1;
    if (s->control[7] != ' ' || s->line_length < 9 || s->line_length > 18) return 0;
    for (size_t i = 8; i < s->line_length; i++)
        if (s->control[i] < '0' || s->control[i] > '9') return 0;
    return 1;
}
static enum offline_modem_result modem_result(struct offline_stream *s) {
    if (line_equals(s, "ERROR")) return OFFLINE_MODEM_ERROR;
    if (line_equals(s, "NO CARRIER")) return OFFLINE_MODEM_NO_CARRIER;
    if (line_equals(s, "NO DIALTONE")) return OFFLINE_MODEM_NO_DIALTONE;
    if (line_equals(s, "BUSY")) return OFFLINE_MODEM_BUSY;
    if (line_equals(s, "NO ANSWER")) return OFFLINE_MODEM_NO_ANSWER;
    enum offline_modem_result kind = OFFLINE_MODEM_NONE;
    if (s->line_length >= 11 && !memcmp(s->control, "+CME ERROR:", 11)) kind = OFFLINE_MODEM_CME;
    if (s->line_length >= 11 && !memcmp(s->control, "+CMS ERROR:", 11)) kind = OFFLINE_MODEM_CMS;
    if (!kind) return kind;
    /* Keep only a 1..5 digit numeric code; verbose text is never retained. */
    size_t i = 11, start;
    while (i < s->line_length && s->control[i] == ' ') i++;
    unsigned value = 0;
    for (start = i; i < s->line_length && i - start < 5 &&
         s->control[i] >= '0' && s->control[i] <= '9'; i++)
        value = value * 10 + (unsigned)(s->control[i] - '0');
    if (i > start && i == s->line_length) { s->modem_code = value; s->modem_code_valid = 1; }
    return kind;
}
static int control_byte(struct offline_stream *s, unsigned char byte) {
    if (s->control_bytes == OFFLINE_CONTROL_MAX) return fail(s, OFFLINE_ERROR_LIMIT);
    s->control_bytes++;
    if (s->pending_cr && byte == '\r' && !s->echo_seen && line_equals(s, OFFLINE_CGDATA_COMMAND)) {
        /* One echo of the fixed hook ends with a bare CR; this CR starts the
         * result's CRLF, so pending_cr stays set. */
        s->echo_seen = 1; s->line_length = 0;
    } else if (s->pending_cr) {
        if (byte != '\n') return fail(s, OFFLINE_ERROR_CONTROL);
        s->pending_cr = 0;
        if (connect_line(s)) s->state = OFFLINE_DATA_STREAM;
        else if ((s->modem_result = modem_result(s)) != OFFLINE_MODEM_NONE)
            return fail(s, OFFLINE_ERROR_RESULT);
        s->line_length = 0;
    } else if (byte == '\r') s->pending_cr = 1;
    else if (byte < 0x20 || byte > 0x7e) return fail(s, OFFLINE_ERROR_CONTROL);
    else s->control[s->line_length++] = byte;
    if (s->control_bytes == OFFLINE_CONTROL_MAX && s->state == OFFLINE_CONTROL_WAIT)
        return fail(s, OFFLINE_ERROR_LIMIT);
    return OFFLINE_OK;
}

int offline_stream_input(struct offline_stream *s, enum offline_direction direction,
                         enum offline_io_code code, const unsigned char *data,
                         size_t count, size_t requested, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!s) return OFFLINE_INVALID;
    if (!consumed || !direction_valid(direction)) return fail(s, OFFLINE_ERROR_ARGUMENT);
    if (s->state == OFFLINE_CLOSED || s->recovery_unknown || s->drain_done) return OFFLINE_INVALID;
    /* Check the USB status before touching returned buffer or size. */
    if (code != OFFLINE_IO_OK) {
        if (direction == OFFLINE_TX && (code == OFFLINE_IO_INTR || code == OFFLINE_IO_AGAIN))
            return OFFLINE_OK;
        return fail(s, OFFLINE_ERROR_IO);
    }
    if (!data || !count || count > requested) return fail(s, OFFLINE_ERROR_COUNT);
    if (s->state == OFFLINE_STOPPING && offline_stream_poll(s) != OFFLINE_OK)
        return OFFLINE_INVALID;
    if (direction == OFFLINE_TX && (s->state == OFFLINE_CONTROL_WAIT || !s->ready))
        return OFFLINE_FULL;
    while (*consumed < count && s->state == OFFLINE_CONTROL_WAIT) {
        unsigned char byte = data[*consumed];
        (*consumed)++;
        if (control_byte(s, byte) != OFFLINE_OK) return OFFLINE_INVALID;
    }
    if (*consumed == count) return OFFLINE_OK;
    struct offline_queue *q = direction == OFFLINE_RX ? &s->rx : &s->tx;
    if (!queue_valid(q)) return fail(s, OFFLINE_ERROR_ARGUMENT);
    size_t length = count - *consumed, space = q->capacity - q->length;
    if (length > space) length = space;
    int result = offline_queue_enqueue(q, data + *consumed, length);
    if (result != OFFLINE_OK) return fail(s, OFFLINE_ERROR_OVERFLOW);
    *consumed += length;
    return *consumed == count ? OFFLINE_OK : OFFLINE_FULL;
}

int offline_stream_ready(struct offline_stream *s) {
    if (!s || s->recovery_unknown || s->state != OFFLINE_DATA_STREAM) return OFFLINE_INVALID;
    s->ready = 1;
    return OFFLINE_OK;
}

int offline_stream_begin(struct offline_stream *s) {
    if (!s || s->state == OFFLINE_CLOSED || s->recovery_unknown || s->drain_done || s->joined)
        return OFFLINE_INVALID;
    if (s->in_flight == SIZE_MAX) return fail(s, OFFLINE_ERROR_OVERFLOW);
    s->in_flight++;
    return OFFLINE_OK;
}

int offline_stream_complete(struct offline_stream *s) {
    if (!s || !s->in_flight || s->state == OFFLINE_CLOSED) return OFFLINE_INVALID;
    s->in_flight--;
    return OFFLINE_OK;
}

int offline_stream_poll(struct offline_stream *s) {
    if (!s) return OFFLINE_INVALID;
    if (s->state == OFFLINE_STOPPING && !s->drain_done) {
        uint64_t now = s->backend.now_ms(s->backend.context);
        if (now < s->last_ms) (void)fail(s, OFFLINE_ERROR_CLOCK);
        else if (now >= s->deadline_ms) (void)fail(s, OFFLINE_ERROR_TIMEOUT);
        s->last_ms = now;
    }
    if (!s->cleanup_done && s->joined && !s->in_flight &&
        (s->recovery_unknown || s->drain_done)) {
        s->cleanup_done = 1;
        if (s->backend.close(s->backend.context)) (void)fail(s, OFFLINE_ERROR_CLOSE);
        s->backend.release(s->backend.context);
        s->state = OFFLINE_CLOSED;
    }
    return s->recovery_unknown ? OFFLINE_INVALID : OFFLINE_OK;
}

int offline_stream_step(struct offline_stream *s, enum offline_direction direction) {
    if (!s) return OFFLINE_INVALID;
    if (!direction_valid(direction)) return fail(s, OFFLINE_ERROR_ARGUMENT);
    if (offline_stream_poll(s) != OFFLINE_OK || s->state == OFFLINE_CLOSED || s->drain_done)
        return OFFLINE_INVALID;
    if (!s->ready || s->state == OFFLINE_CONTROL_WAIT) return OFFLINE_OK;
    struct offline_queue *q = direction == OFFLINE_RX ? &s->rx : &s->tx;
    size_t requested = 0;
    if (!queue_valid(q)) return fail(s, OFFLINE_ERROR_ARGUMENT);
    const unsigned char *data = offline_queue_peek(q, &requested);
    if (!requested) return OFFLINE_OK;
    if (requested > UINT64_MAX - q->delivered) return fail(s, OFFLINE_ERROR_OVERFLOW);
    if (offline_stream_begin(s) != OFFLINE_OK) return OFFLINE_INVALID;
    struct offline_io result = direction == OFFLINE_RX ?
        s->backend.stdio_write(s->backend.context, data, requested) :
        s->backend.usb_write(s->backend.context, data, requested);
    (void)offline_stream_complete(s);
    if (result.code != OFFLINE_IO_OK) {
        if (direction == OFFLINE_RX &&
            (result.code == OFFLINE_IO_INTR || result.code == OFFLINE_IO_AGAIN))
            return offline_stream_poll(s);
        return fail(s, OFFLINE_ERROR_IO);
    }
    if (!result.count || result.count > requested ||
        (direction == OFFLINE_TX && result.count != requested))
        return fail(s, OFFLINE_ERROR_COUNT);
    if (offline_queue_consume(q, result.count) != OFFLINE_OK)
        return fail(s, OFFLINE_ERROR_OVERFLOW);
    return offline_stream_poll(s);
}

int offline_stream_stop(struct offline_stream *s, uint64_t drain_ms) {
    if (!s || s->state == OFFLINE_CLOSED || s->recovery_unknown) return OFFLINE_INVALID;
    if (s->state == OFFLINE_CONTROL_WAIT) return fail(s, OFFLINE_ERROR_CONTROL);
    if (s->state != OFFLINE_DATA_STREAM) return OFFLINE_INVALID;
    uint64_t now = s->backend.now_ms(s->backend.context);
    if (!drain_ms || drain_ms > UINT64_MAX - now) return fail(s, OFFLINE_ERROR_OVERFLOW);
    s->last_ms = now;
    s->deadline_ms = now + drain_ms;
    s->state = OFFLINE_STOPPING;
    return OFFLINE_OK;
}

int offline_stream_drain_complete(struct offline_stream *s) {
    if (!s || s->state != OFFLINE_STOPPING || s->recovery_unknown) return OFFLINE_INVALID;
    if (offline_stream_poll(s) != OFFLINE_OK) return OFFLINE_INVALID;
    if (s->rx.length || s->tx.length || s->in_flight) return OFFLINE_FULL;
    s->drain_done = 1;
    return OFFLINE_OK;
}

int offline_stream_join(struct offline_stream *s) {
    if (!s || s->state == OFFLINE_CLOSED || s->in_flight ||
        (!s->drain_done && !s->recovery_unknown)) return OFFLINE_INVALID;
    s->joined = 1;
    return OFFLINE_OK;
}

int offline_stream_diagnostic(const struct offline_stream *s, char *buffer, size_t capacity) {
    static const char *const states[] = {"control-wait", "data-stream", "stopping", "recovery-unknown", "closed"};
    static const char *const errors[] = {"none", "argument", "control", "limit", "result", "io", "count", "overflow", "timeout", "clock", "close"};
    if (!s || (!buffer && capacity) || (unsigned)s->state >= sizeof(states) / sizeof(states[0]) ||
        (unsigned)s->error >= sizeof(errors) / sizeof(errors[0])) return -1;
    return snprintf(buffer, capacity,
        "offline state=%s error=%s recovery=%s control=%zu rx_accepted=%" PRIu64
        " rx_delivered=%" PRIu64 " rx_queued=%zu tx_accepted=%" PRIu64
        " tx_delivered=%" PRIu64 " tx_queued=%zu in_flight=%zu cleanup=%d",
        states[s->state], errors[s->error], s->recovery_unknown ? "unknown" : "unproven",
        s->control_bytes, s->rx.accepted, s->rx.delivered, s->rx.length,
        s->tx.accepted, s->tx.delivered, s->tx.length, s->in_flight, s->cleanup_done);
}
