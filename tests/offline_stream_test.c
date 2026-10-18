#include "../src/offline_stream.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

struct fake {
    struct offline_stream *stream;
    unsigned char received[8192], sent[8192];
    size_t received_length, sent_length, partial;
    struct offline_io stdio_result, usb_result;
    unsigned stdio_calls, usb_calls, closes, releases;
    uint64_t now, write_delay;
    int override_stdio, override_usb, close_error;
};

static struct offline_io fake_write(struct fake *f, const unsigned char *data,
                                    size_t length, int usb) {
    CHECK(f->stream->in_flight > 0 && f->stream->ready && !f->stream->cleanup_done);
    struct offline_io result = {OFFLINE_IO_OK, length};
    if (!usb && f->partial && result.count > f->partial) result.count = f->partial;
    if (usb ? f->override_usb : f->override_stdio)
        result = usb ? f->usb_result : f->stdio_result;
    if (usb) f->usb_calls++; else f->stdio_calls++;
    if (result.code == OFFLINE_IO_OK && result.count && result.count <= length) {
        unsigned char *output = usb ? f->sent : f->received;
        size_t *used = usb ? &f->sent_length : &f->received_length;
        CHECK(result.count <= sizeof(f->sent) - *used);
        if (result.count <= sizeof(f->sent) - *used) {
            memcpy(output + *used, data, result.count);
            *used += result.count;
        }
    }
    CHECK(f->write_delay <= UINT64_MAX - f->now);
    f->now += f->write_delay;
    return result;
}
static struct offline_io fake_stdio(void *context, const unsigned char *data, size_t length) {
    return fake_write(context, data, length, 0);
}
static struct offline_io fake_usb(void *context, const unsigned char *data, size_t length) {
    return fake_write(context, data, length, 1);
}
static uint64_t fake_now(void *context) { return ((struct fake *)context)->now; }
static int fake_close(void *context) {
    struct fake *f = context;
    CHECK(!f->stream->in_flight && f->stream->joined && f->stream->cleanup_done);
    CHECK(f->closes == 0 && f->releases == 0);
    f->closes++;
    return f->close_error;
}
static void fake_release(void *context) {
    struct fake *f = context;
    CHECK(f->closes == 1 && f->releases == 0 && !f->stream->in_flight);
    f->releases++;
}
static struct offline_backend backend(struct fake *f) {
    return (struct offline_backend){f, fake_stdio, fake_usb, fake_now, fake_close, fake_release};
}
static void init(struct offline_stream *s, struct fake *f, unsigned char *rx, size_t rx_cap,
                 unsigned char *tx, size_t tx_cap) {
    *f = (struct fake){.stream = s};
    struct offline_backend b = backend(f);
    CHECK(offline_stream_init(s, &b, rx, rx_cap, tx, tx_cap) == OFFLINE_OK);
}
static void connect_stream(struct offline_stream *s) {
    const unsigned char response[] = "CONNECT\r\n";
    size_t consumed = 99;
    CHECK(offline_stream_input(s, OFFLINE_RX, OFFLINE_IO_OK, response,
          sizeof(response) - 1, sizeof(response) - 1, &consumed) == OFFLINE_OK);
    CHECK(consumed == sizeof(response) - 1 && s->state == OFFLINE_DATA_STREAM);
}
static void finish_unknown(struct offline_stream *s, struct fake *f) {
    CHECK(s->recovery_unknown && s->state == OFFLINE_RECOVERY_UNKNOWN);
    CHECK(offline_stream_join(s) == OFFLINE_OK);
    for (unsigned i = 0; i < 3; i++) CHECK(offline_stream_poll(s) == OFFLINE_INVALID);
    CHECK(s->state == OFFLINE_CLOSED && s->recovery_unknown && f->closes == 1 && f->releases == 1);
}
static void finish_normal(struct offline_stream *s, struct fake *f) {
    CHECK(offline_stream_stop(s, 100) == OFFLINE_OK);
    CHECK(offline_stream_drain_complete(s) == OFFLINE_OK);
    CHECK(offline_stream_join(s) == OFFLINE_OK);
    for (unsigned i = 0; i < 3; i++) CHECK(offline_stream_poll(s) == OFFLINE_OK);
    CHECK(s->state == OFFLINE_CLOSED && !s->recovery_unknown && f->closes == 1 && f->releases == 1);
}

static void queue_tests(void) {
    unsigned char storage[5], data[] = {0, 1, 2, 3, 4, 5};
    struct offline_queue q;
    size_t length = 99;
    CHECK(offline_queue_init(NULL, storage, 5) == OFFLINE_INVALID);
    CHECK(offline_queue_init(&q, NULL, 5) == OFFLINE_INVALID);
    CHECK(offline_queue_init(&q, storage, 0) == OFFLINE_INVALID);
    CHECK(offline_queue_init(&q, storage, SIZE_MAX) == OFFLINE_INVALID);
    CHECK(offline_queue_init(&q, storage, OFFLINE_QUEUE_MAX + 1) == OFFLINE_INVALID);
    CHECK(offline_queue_init(&q, storage, sizeof(storage)) == OFFLINE_OK);
    CHECK(offline_queue_peek(&q, &length) == NULL && length == 0);
    CHECK(offline_queue_peek(&q, NULL) == NULL);
    CHECK(offline_queue_enqueue(&q, NULL, 0) == OFFLINE_OK);
    CHECK(offline_queue_enqueue(&q, NULL, 1) == OFFLINE_INVALID);
    CHECK(offline_queue_enqueue(&q, data, SIZE_MAX) == OFFLINE_FULL);
    CHECK(offline_queue_enqueue(&q, data, 4) == OFFLINE_OK);
    CHECK(offline_queue_enqueue(&q, data, 2) == OFFLINE_FULL && q.length == 4 && q.accepted == 4);
    CHECK(offline_queue_consume(&q, 5) == OFFLINE_INVALID && q.length == 4);
    CHECK(offline_queue_consume(&q, 3) == OFFLINE_OK);
    CHECK(offline_queue_enqueue(&q, data + 4, 2) == OFFLINE_OK);
    const unsigned char *span = offline_queue_peek(&q, &length);
    CHECK(span && length == 2 && span[0] == 3 && span[1] == 4);
    CHECK(offline_queue_consume(&q, 3) == OFFLINE_INVALID);
    CHECK(offline_queue_consume(&q, 2) == OFFLINE_OK);
    span = offline_queue_peek(&q, &length);
    CHECK(span && length == 1 && span[0] == 5);
    CHECK(offline_queue_consume(&q, 1) == OFFLINE_OK && q.accepted == 6 && q.delivered == 6);
    q.accepted = q.delivered = UINT64_MAX;
    CHECK(offline_queue_enqueue(&q, data, 1) == OFFLINE_OVERFLOW && q.length == 0);
    CHECK(offline_queue_enqueue(&q, NULL, 0) == OFFLINE_OK);
    q.accepted = UINT64_MAX; q.delivered = UINT64_MAX - 1; q.length = 1;
    CHECK(offline_queue_consume(&q, 1) == OFFLINE_OK && q.delivered == UINT64_MAX);
    q.head = q.capacity;
    CHECK(offline_queue_enqueue(&q, data, 1) == OFFLINE_INVALID);
    CHECK(offline_queue_peek(&q, &length) == NULL && length == 0);
    CHECK(offline_queue_consume(&q, 0) == OFFLINE_INVALID);
    q.head = 0; q.length = q.capacity + 1;
    CHECK(offline_queue_enqueue(&q, data, 1) == OFFLINE_INVALID);
    q.length = 0; q.delivered = 0; q.accepted = 1;
    CHECK(offline_queue_consume(&q, 0) == OFFLINE_INVALID);
}

static void parser_tests(void) {
    unsigned char rx[64], tx[64];
    struct offline_stream s;
    struct fake f;
    const unsigned char control[] = "\r\nECHO\r\nOK\r\nCONNECT\r\n";
    const unsigned char tail[] = {'\n', 0, 0xff, '~', '}', 'C', 'O', 'N', 'N', 'E', 'C', 'T', '\r', '\n'};
    unsigned char input[128];
    size_t control_length = sizeof(control) - 1;
    memcpy(input, control, control_length);
    memcpy(input + control_length, tail, sizeof(tail));
    size_t total = control_length + sizeof(tail);
    /* Every pair of split positions: surrounding lines, delimiter and binary tail. */
    for (size_t a = 0; a <= total; a++) for (size_t b = a; b <= total; b++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        size_t cuts[] = {0, a, b, total};
        for (unsigned j = 1; j < 4; j++) {
            size_t n = cuts[j] - cuts[j - 1], used = 77;
            if (!n) continue;
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
                  input + cuts[j - 1], n, n, &used) == OFFLINE_OK && used == n);
            if (cuts[j] < control_length) CHECK(s.state == OFFLINE_CONTROL_WAIT);
            CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_OK && f.stdio_calls == 0);
        }
        CHECK(s.state == OFFLINE_DATA_STREAM && s.control_bytes == control_length);
        CHECK(s.rx.length == sizeof(tail) && !memcmp(rx, tail, sizeof(tail)));
        CHECK(offline_stream_ready(&s) == OFFLINE_OK);
        CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_OK);
        CHECK(f.received_length == sizeof(tail) && !memcmp(f.received, tail, sizeof(tail)));
        finish_normal(&s, &f);
    }
    /* Every partition, not only one split, of CONNECT\r\n. */
    const unsigned char connect[] = "CONNECT\r\n";
    for (unsigned mask = 0; mask < 256; mask++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        size_t start = 0;
        for (size_t end = 1; end <= sizeof(connect) - 1; end++) {
            if (end < sizeof(connect) - 1 && !(mask & (1u << (end - 1)))) continue;
            size_t used = 77, n = end - start;
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, connect + start,
                  n, n, &used) == OFFLINE_OK && used == n);
            CHECK(s.state == (end == sizeof(connect) - 1 ? OFFLINE_DATA_STREAM : OFFLINE_CONTROL_WAIT));
            start = end;
        }
        finish_normal(&s, &f);
    }
    /* A V.250 rate suffix is a CONNECT; the echo of the fixed hook precedes it once. */
    const char *connects[] = {"CONNECT 115200\r\n", "CONNECT 1\r\n", "CONNECT 1234567890\r\n",
        "\r\nCONNECT 150000000\r\n", OFFLINE_CGDATA_COMMAND "\r\r\nCONNECT\r\n",
        OFFLINE_CGDATA_COMMAND "\r\r\nCONNECT 150000000\r\n",
        OFFLINE_CGDATA_COMMAND "\r\r\n+URC: 1\r\n\r\nCONNECT\r\n"};
    for (size_t i = 0; i < sizeof(connects) / sizeof(connects[0]); i++) {
        size_t n = strlen(connects[i]);
        for (size_t split = 1; split < n; split++) {
            init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
            size_t used;
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
                  (const unsigned char *)connects[i], split, split, &used) == OFFLINE_OK && used == split);
            CHECK(s.state == OFFLINE_CONTROL_WAIT);
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
                  (const unsigned char *)connects[i] + split, n - split, n - split, &used) == OFFLINE_OK);
            CHECK(used == n - split && s.state == OFFLINE_DATA_STREAM && !s.rx.length);
            CHECK(s.echo_seen == !strncmp(connects[i], OFFLINE_CGDATA_COMMAND, strlen(OFFLINE_CGDATA_COMMAND)));
            finish_normal(&s, &f);
        }
    }
    /* Only one exact echo; any other bare CR, or a second echo, is a control error. */
    const char *bad_echo[] = {"AT+CGDATA=\"PPP\",2\r\r\nCONNECT\r\n", "AT\r\r\nCONNECT\r\n",
        OFFLINE_CGDATA_COMMAND "\r" OFFLINE_CGDATA_COMMAND "\r\r\n",
        /* A second complete bare-CR echo after the first: rejected only by echo_seen. */
        OFFLINE_CGDATA_COMMAND "\r\r\n" OFFLINE_CGDATA_COMMAND "\r\r\nCONNECT\r\n",
        OFFLINE_CGDATA_COMMAND "\r\rX", "at+cgdata=\"ppp\",1\r\r\n"};
    for (size_t i = 0; i < sizeof(bad_echo) / sizeof(bad_echo[0]); i++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        size_t used, n = strlen(bad_echo[i]);
        CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
              (const unsigned char *)bad_echo[i], n, n, &used) == OFFLINE_INVALID);
        CHECK(s.error == OFFLINE_ERROR_CONTROL && s.state != OFFLINE_DATA_STREAM && !s.rx.length);
        finish_unknown(&s, &f);
    }
    const char *not_results[] = {"XCONNECT\r\n", "CONNECTX\r\n", " CONNECT\r\n", "CONNECT \r\n",
        "CONNECT  1\r\n", "CONNECT 1x\r\n", "CONNECT 12345678901\r\n", "CONNECT -1\r\n",
        "connect\r\n", "NO CARRIERX\r\n", "ERRORX\r\n", "CONNECT", "CONNECT\r",
        OFFLINE_CGDATA_COMMAND "\r\n"};
    for (size_t i = 0; i < sizeof(not_results) / sizeof(not_results[0]); i++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        size_t used, n = strlen(not_results[i]);
        CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
              (const unsigned char *)not_results[i], n, n, &used) == OFFLINE_OK);
        CHECK(used == n && s.state == OFFLINE_CONTROL_WAIT && !s.rx.length);
    }
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
    size_t partial_used;
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
          (const unsigned char *)"CONNECT\r", 8, 8, &partial_used) == OFFLINE_OK);
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_CONTROL);
    finish_unknown(&s, &f);
    const char *bad[] = {"CONNECT\n", "CONNECT\rX", "\r\r", "\nCONNECT\r\n", "\tCONNECT\r\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        size_t used, n = strlen(bad[i]);
        CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
              (const unsigned char *)bad[i], n, n, &used) == OFFLINE_INVALID);
        CHECK(used > 0 && used <= n && s.error == OFFLINE_ERROR_CONTROL && !s.rx.length);
        finish_unknown(&s, &f);
    }
    const char *errors[] = {"ERROR\r\n", "NO CARRIER\r\n", "BUSY\r\n", "NO DIALTONE\r\n",
        "NO ANSWER\r\n", "+CME ERROR: private\r\n", "+CMS ERROR: 5\r\n", "+CME ERROR: 30\r\n",
        "+CME ERROR:123456\r\n", OFFLINE_CGDATA_COMMAND "\r\r\nNO CARRIER\r\n"};
    const enum offline_modem_result kinds[] = {OFFLINE_MODEM_ERROR, OFFLINE_MODEM_NO_CARRIER,
        OFFLINE_MODEM_BUSY, OFFLINE_MODEM_NO_DIALTONE, OFFLINE_MODEM_NO_ANSWER, OFFLINE_MODEM_CME,
        OFFLINE_MODEM_CMS, OFFLINE_MODEM_CME, OFFLINE_MODEM_CME, OFFLINE_MODEM_NO_CARRIER};
    /* Only a 1..5 digit code survives; text and longer numbers are dropped. */
    const int codes[] = {-1, -1, -1, -1, -1, -1, 5, 30, -1, -1};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        size_t used, n = strlen(errors[i]);
        CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
              (const unsigned char *)errors[i], n, n, &used) == OFFLINE_INVALID);
        CHECK(s.error == OFFLINE_ERROR_RESULT && s.modem_result == kinds[i]);
        CHECK(codes[i] < 0 ? !s.modem_code_valid : s.modem_code_valid && s.modem_code == (unsigned)codes[i]);
        finish_unknown(&s, &f);
    }
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        size_t n = strlen(errors[i]);
        for (size_t split = 1; split < n; split++) {
            init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
            size_t used;
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
                  (const unsigned char *)errors[i], split, split, &used) == OFFLINE_OK && used == split);
            CHECK(s.state == OFFLINE_CONTROL_WAIT);
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
                  (const unsigned char *)errors[i] + split, n - split, n - split, &used) == OFFLINE_INVALID);
            CHECK(used == n - split && s.error == OFFLINE_ERROR_RESULT);
            CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, connect, 9, 9, &used) == OFFLINE_INVALID && used == 0);
            finish_unknown(&s, &f);
        }
    }
    for (unsigned byte = 0; byte < 256; byte++) {
        if (byte >= 0x20 && byte <= 0x7e) continue;
        if (byte == '\r') continue;
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        unsigned char value = (unsigned char)byte;
        size_t used;
        CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, &value, 1, 1, &used) == OFFLINE_INVALID);
        CHECK(used == 1 && s.error == OFFLINE_ERROR_CONTROL);
    }
    unsigned char bound[OFFLINE_CONTROL_MAX + 1];
    /* 2039 printable bytes plus CRLF plus seven-char CONNECT plus CRLF = 2050;
     * choose 2037 so exact CONNECT end lands at 2048. */
    memset(bound, 'A', sizeof(bound));
    memcpy(bound + OFFLINE_CONTROL_MAX - 11, "\r\nCONNECT\r\n", 11);
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
    size_t used;
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, bound, OFFLINE_CONTROL_MAX,
          OFFLINE_CONTROL_MAX, &used) == OFFLINE_OK);
    CHECK(s.state == OFFLINE_DATA_STREAM && s.control_bytes == OFFLINE_CONTROL_MAX && used == OFFLINE_CONTROL_MAX);
    finish_normal(&s, &f);
    memset(bound, 'A', sizeof(bound));
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, bound, sizeof(bound),
          sizeof(bound), &used) == OFFLINE_INVALID);
    CHECK(s.control_bytes == OFFLINE_CONTROL_MAX && s.error == OFFLINE_ERROR_LIMIT && used == OFFLINE_CONTROL_MAX);
    finish_unknown(&s, &f);
}

static void byte_stream_tests(void) {
    unsigned char raw[512], rx[17], tx[17];
    for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)i;
    /* Each capacity and read chunk combination exercises wrap/merge/partial output. */
    for (size_t combination = 0; combination < sizeof(rx) * 31; combination++) {
        size_t capacity = combination / 31 + 1, chunk = combination % 31 + 1;
        struct offline_stream s;
        struct fake f;
        init(&s, &f, rx, capacity, tx, capacity);
        unsigned char first[521];
        memcpy(first, "CONNECT\r\n", 9); memcpy(first + 9, raw, sizeof(raw));
        size_t used;
        CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, first, sizeof(first),
              sizeof(first), &used) == OFFLINE_FULL);
        CHECK(used == 9 + capacity && s.rx.length == capacity);
        CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_OK && f.stdio_calls == 0);
        size_t tx_used = 99;
        CHECK(offline_stream_input(&s, OFFLINE_TX, OFFLINE_IO_OK, raw, 1, 1,
              &tx_used) == OFFLINE_FULL && tx_used == 0);
        CHECK(offline_stream_ready(&s) == OFFLINE_OK);
        f.partial = 3;
        size_t offset = used - 9;
        for (unsigned direction = 0; direction < 2; direction++) {
            if (direction == OFFLINE_TX) offset = 0;
            unsigned iterations = 0;
            while (offset < sizeof(raw) || (direction == OFFLINE_RX ? s.rx.length : s.tx.length)) {
                CHECK(++iterations < 2000);
                if (iterations >= 2000) break;
                if (offset < sizeof(raw)) {
                    size_t n = sizeof(raw) - offset;
                    if (n > chunk) n = chunk;
                    int result = offline_stream_input(&s, (enum offline_direction)direction,
                        OFFLINE_IO_OK, raw + offset, n, n, &used);
                    CHECK((result == OFFLINE_OK || result == OFFLINE_FULL) && used <= n);
                    CHECK((result == OFFLINE_OK) == (used == n));
                    offset += used;
                }
                unsigned calls = f.stdio_calls + f.usb_calls;
                CHECK(offline_stream_step(&s, (enum offline_direction)direction) == OFFLINE_OK);
                CHECK(f.stdio_calls + f.usb_calls <= calls + 1);
            }
        }
        CHECK(f.received_length == sizeof(raw) && !memcmp(f.received, raw, sizeof(raw)));
        CHECK(f.sent_length == sizeof(raw) && !memcmp(f.sent, raw, sizeof(raw)));
        CHECK(s.rx.accepted == sizeof(raw) && s.rx.delivered == sizeof(raw));
        CHECK(s.tx.accepted == sizeof(raw) && s.tx.delivered == sizeof(raw));
        finish_normal(&s, &f);
    }
    const unsigned char frames[] = {0x7e, 0xff, 0x03, 0x7d, 0x20, 0x7e, 0x7e, 0x7d, 0x5e, 0, 0x7e};
    for (size_t split = 1; split < sizeof(frames); split++) {
        struct offline_stream s;
        struct fake f;
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
        CHECK(offline_stream_ready(&s) == OFFLINE_OK);
        for (unsigned direction = 0; direction < 2; direction++) {
            size_t used;
            CHECK(offline_stream_input(&s, (enum offline_direction)direction, OFFLINE_IO_OK,
                  frames, split, split, &used) == OFFLINE_OK && used == split);
            CHECK(offline_stream_input(&s, (enum offline_direction)direction, OFFLINE_IO_OK,
                  frames + split, sizeof(frames) - split, sizeof(frames) - split, &used) == OFFLINE_OK);
            CHECK(offline_stream_step(&s, (enum offline_direction)direction) == OFFLINE_OK);
        }
        CHECK(f.received_length == sizeof(frames) && !memcmp(f.received, frames, sizeof(frames)));
        CHECK(f.sent_length == sizeof(frames) && !memcmp(f.sent, frames, sizeof(frames)));
        finish_normal(&s, &f);
    }
}

static void io_tests(void) {
    unsigned char rx[16], tx[16], data[] = {0, 1, 2, 3};
    struct offline_stream s;
    struct fake f;
    size_t used;
    /* All failed USB reads, including retry-like statuses, ignore poisoned size/pointer. */
    const enum offline_io_code errors[] = {OFFLINE_IO_INTR, OFFLINE_IO_AGAIN, OFFLINE_IO_EOF, OFFLINE_IO_ERROR, (enum offline_io_code)99};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx));
        CHECK(offline_stream_input(&s, OFFLINE_RX, errors[i], (const unsigned char *)(uintptr_t)1,
              SIZE_MAX, 0, &used) == OFFLINE_INVALID);
        CHECK(used == 0 && s.error == OFFLINE_ERROR_IO && s.control_bytes == 0 && !s.rx.length);
        finish_unknown(&s, &f);
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
        CHECK(offline_stream_ready(&s) == OFFLINE_OK);
        CHECK(offline_stream_input(&s, OFFLINE_TX, OFFLINE_IO_OK, data, 4, 4, &used) == OFFLINE_OK);
        f.override_usb = 1; f.usb_result = (struct offline_io){errors[i], SIZE_MAX};
        CHECK(offline_stream_step(&s, OFFLINE_TX) == OFFLINE_INVALID);
        CHECK(s.tx.length == 4 && s.tx.delivered == 0 && f.usb_calls == 1);
        for (unsigned j = 0; j < 3; j++) CHECK(offline_stream_step(&s, OFFLINE_TX) == OFFLINE_INVALID);
        CHECK(f.usb_calls == 1 && s.recovery_unknown);
        finish_unknown(&s, &f);
    }
    for (unsigned direction = 0; direction < 2; direction++) {
        for (unsigned kind = 0; kind < 3; kind++) {
            init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
            CHECK(offline_stream_ready(&s) == OFFLINE_OK);
            size_t count = kind == 0 ? 0 : kind == 1 ? 5 : 1;
            CHECK(offline_stream_input(&s, (enum offline_direction)direction, OFFLINE_IO_OK,
                  kind == 2 ? NULL : data, count, 4, &used) == OFFLINE_INVALID);
            CHECK(used == 0 && s.error == OFFLINE_ERROR_COUNT);
            finish_unknown(&s, &f);
        }
        for (unsigned kind = 0; kind < 4; kind++) {
            init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
            CHECK(offline_stream_ready(&s) == OFFLINE_OK);
            CHECK(offline_stream_input(&s, (enum offline_direction)direction, OFFLINE_IO_OK,
                  data, 4, 4, &used) == OFFLINE_OK);
            struct offline_io result = {kind == 2 ? OFFLINE_IO_ERROR : OFFLINE_IO_OK,
                kind == 0 ? 0 : kind == 1 ? 5 : kind == 2 ? SIZE_MAX : 2};
            f.override_stdio = f.override_usb = 1;
            f.stdio_result = f.usb_result = result;
            int status = offline_stream_step(&s, (enum offline_direction)direction);
            if (direction == OFFLINE_RX && kind == 3) {
                CHECK(status == OFFLINE_OK && s.rx.length == 2 && s.rx.delivered == 2);
                f.override_stdio = 0;
                CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_OK);
                CHECK(f.received_length == 4 && !memcmp(f.received, data, 4));
                finish_normal(&s, &f);
            } else {
                CHECK(status == OFFLINE_INVALID && s.recovery_unknown);
                CHECK((direction == OFFLINE_RX ? s.rx.delivered : s.tx.delivered) == 0);
                finish_unknown(&s, &f);
            }
        }
    }
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    CHECK(offline_stream_ready(&s) == OFFLINE_OK);
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, data, 4, 4, &used) == OFFLINE_OK);
    f.override_stdio = 1;
    for (unsigned i = 0; i < 4; i++) {
        f.stdio_result = (struct offline_io){i % 2 ? OFFLINE_IO_AGAIN : OFFLINE_IO_INTR, SIZE_MAX};
        unsigned calls = f.stdio_calls;
        CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_OK);
        CHECK(f.stdio_calls == calls + 1 && s.rx.head == 0 && s.rx.length == 4 && !s.rx.delivered);
        CHECK(offline_stream_input(&s, OFFLINE_TX, f.stdio_result.code,
              (const unsigned char *)(uintptr_t)1, SIZE_MAX, 0, &used) == OFFLINE_OK && used == 0);
        CHECK(s.state == OFFLINE_DATA_STREAM && !s.recovery_unknown);
    }
    f.override_stdio = 0; f.partial = 1;
    for (unsigned i = 0; i < 4; i++) CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_OK);
    CHECK(f.received_length == 4 && !memcmp(f.received, data, 4));
    finish_normal(&s, &f);
    for (unsigned direction = 0; direction < 2; direction++) {
        init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
        CHECK(offline_stream_input(&s, (enum offline_direction)direction, OFFLINE_IO_EOF,
              NULL, 0, 0, &used) == OFFLINE_INVALID && used == 0);
        finish_unknown(&s, &f);
    }
}

static void lifecycle_tests(void) {
    unsigned char rx[16], tx[16], exchange[] = {0x7e, 0x7d, 0, 0xff};
    struct offline_stream s;
    struct fake f;
    size_t used;
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    CHECK(offline_stream_ready(&s) == OFFLINE_OK);
    CHECK(offline_stream_begin(&s) == OFFLINE_OK);
    CHECK(offline_stream_begin(&s) == OFFLINE_OK && s.in_flight == 2);
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_OK && s.state == OFFLINE_STOPPING);
    CHECK(offline_stream_stop(&s, 50) == OFFLINE_INVALID && s.deadline_ms == 10);
    CHECK(offline_stream_join(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_poll(&s) == OFFLINE_OK && f.closes == 0);
    CHECK(offline_stream_complete(&s) == OFFLINE_OK && s.in_flight == 1);
    for (unsigned direction = 0; direction < 2; direction++) {
        CHECK(offline_stream_input(&s, (enum offline_direction)direction, OFFLINE_IO_OK,
              exchange, sizeof(exchange), sizeof(exchange), &used) == OFFLINE_OK && used == sizeof(exchange));
        CHECK(offline_stream_drain_complete(&s) == OFFLINE_FULL);
        CHECK(offline_stream_step(&s, (enum offline_direction)direction) == OFFLINE_OK);
    }
    CHECK(f.received_length == sizeof(exchange) && !memcmp(f.received, exchange, sizeof(exchange)));
    CHECK(f.sent_length == sizeof(exchange) && !memcmp(f.sent, exchange, sizeof(exchange)));
    CHECK(offline_stream_drain_complete(&s) == OFFLINE_FULL);
    CHECK(offline_stream_complete(&s) == OFFLINE_OK);
    CHECK(offline_stream_complete(&s) == OFFLINE_INVALID);
    f.now = 9;
    CHECK(offline_stream_drain_complete(&s) == OFFLINE_OK);
    CHECK(offline_stream_begin(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_INVALID);
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, exchange, 4, 4, &used) == OFFLINE_INVALID && used == 0);
    CHECK(offline_stream_poll(&s) == OFFLINE_OK && f.closes == 0);
    CHECK(offline_stream_join(&s) == OFFLINE_OK);
    CHECK(offline_stream_join(&s) == OFFLINE_OK);
    CHECK(f.closes == 0 && f.releases == 0);
    f.now = UINT64_MAX; /* completed drain is not timed out while waiting for join/cleanup */
    for (unsigned i = 0; i < 3; i++) CHECK(offline_stream_poll(&s) == OFFLINE_OK);
    CHECK(s.state == OFFLINE_CLOSED && !s.recovery_unknown && f.closes == 1 && f.releases == 1);
    CHECK(offline_stream_join(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_ready(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_INVALID);
    CHECK(offline_stream_input(&s, OFFLINE_TX, OFFLINE_IO_OK, exchange, 4, 4, &used) == OFFLINE_INVALID && used == 0);
    CHECK(f.usb_calls == 1);

    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    CHECK(offline_stream_ready(&s) == OFFLINE_OK);
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, exchange, 4, 4, &used) == OFFLINE_OK);
    CHECK(offline_stream_begin(&s) == OFFLINE_OK);
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_OK);
    f.now = 10;
    CHECK(offline_stream_poll(&s) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_TIMEOUT);
    CHECK(s.rx.length == 4 && !s.rx.delivered && f.closes == 0);
    CHECK(offline_stream_join(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_begin(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_complete(&s) == OFFLINE_OK);
    CHECK(offline_stream_drain_complete(&s) == OFFLINE_INVALID);
    finish_unknown(&s, &f);
    CHECK(offline_stream_ready(&s) == OFFLINE_INVALID && f.stdio_calls == 0);

    /* A callback completes after budget expiry: account its success, then fail. */
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    CHECK(offline_stream_ready(&s) == OFFLINE_OK);
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, exchange, 4, 4, &used) == OFFLINE_OK);
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_OK);
    f.write_delay = 10;
    CHECK(offline_stream_step(&s, OFFLINE_RX) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_TIMEOUT);
    CHECK(s.rx.delivered == 4 && !s.in_flight && f.stdio_calls == 1);
    finish_unknown(&s, &f);

    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    f.now = 100;
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_OK);
    f.now = 99;
    CHECK(offline_stream_poll(&s) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_CLOCK);
    finish_unknown(&s, &f);
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    f.now = UINT64_MAX - 4;
    CHECK(offline_stream_stop(&s, 5) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_OVERFLOW);
    finish_unknown(&s, &f);
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    f.now = UINT64_MAX - 4;
    CHECK(offline_stream_stop(&s, 4) == OFFLINE_OK && s.deadline_ms == UINT64_MAX);
    f.now = UINT64_MAX;
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, exchange, 4, 4,
          &used) == OFFLINE_INVALID && used == 0 && s.error == OFFLINE_ERROR_TIMEOUT);
    finish_unknown(&s, &f);
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    CHECK(offline_stream_stop(&s, 0) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_OVERFLOW);
    finish_unknown(&s, &f);
    init(&s, &f, rx, sizeof(rx), tx, sizeof(tx)); connect_stream(&s);
    CHECK(offline_stream_stop(&s, 10) == OFFLINE_OK);
    CHECK(offline_stream_drain_complete(&s) == OFFLINE_OK);
    CHECK(offline_stream_join(&s) == OFFLINE_OK);
    f.close_error = 1;
    CHECK(offline_stream_poll(&s) == OFFLINE_INVALID);
    CHECK(s.state == OFFLINE_CLOSED && s.recovery_unknown && s.error == OFFLINE_ERROR_CLOSE);
    for (unsigned i = 0; i < 3; i++) CHECK(offline_stream_poll(&s) == OFFLINE_INVALID);
    CHECK(f.closes == 1 && f.releases == 1);
}

static void validation_diagnostic_tests(void) {
    unsigned char rx[32], tx[32];
    struct offline_stream s;
    struct fake f = {.stream = &s};
    struct offline_backend b = backend(&f);
    CHECK(offline_stream_init(NULL, &b, rx, 32, tx, 32) == OFFLINE_INVALID);
    CHECK(offline_stream_init(&s, NULL, rx, 32, tx, 32) == OFFLINE_INVALID);
    for (unsigned i = 0; i < 5; i++) {
        struct offline_backend bad = b;
        if (i == 0) bad.stdio_write = NULL;
        if (i == 1) bad.usb_write = NULL;
        if (i == 2) bad.now_ms = NULL;
        if (i == 3) bad.close = NULL;
        if (i == 4) bad.release = NULL;
        CHECK(offline_stream_init(&s, &bad, rx, 32, tx, 32) == OFFLINE_INVALID);
    }
    CHECK(offline_stream_init(&s, &b, NULL, 32, tx, 32) == OFFLINE_INVALID);
    CHECK(offline_stream_init(&s, &b, rx, 0, tx, 32) == OFFLINE_INVALID);
    CHECK(offline_stream_init(&s, &b, rx, 32, tx, OFFLINE_QUEUE_MAX + 1) == OFFLINE_INVALID);
    CHECK(offline_stream_init(&s, &b, rx, 32, rx + 1, 31) == OFFLINE_INVALID);
    CHECK(offline_stream_init(&s, &b, (unsigned char *)(uintptr_t)(UINTPTR_MAX - 1), 32,
          tx, 32) == OFFLINE_INVALID);
    size_t used = 99;
    CHECK(offline_stream_input(NULL, OFFLINE_RX, OFFLINE_IO_ERROR, NULL, 0, 0,
          &used) == OFFLINE_INVALID && used == 0);
    CHECK(offline_stream_ready(NULL) == OFFLINE_INVALID);
    CHECK(offline_stream_step(NULL, OFFLINE_RX) == OFFLINE_INVALID);
    CHECK(offline_stream_stop(NULL, 1) == OFFLINE_INVALID);
    CHECK(offline_stream_poll(NULL) == OFFLINE_INVALID);
    CHECK(offline_stream_drain_complete(NULL) == OFFLINE_INVALID);
    CHECK(offline_stream_begin(NULL) == OFFLINE_INVALID);
    CHECK(offline_stream_complete(NULL) == OFFLINE_INVALID);
    CHECK(offline_stream_join(NULL) == OFFLINE_INVALID);
    for (unsigned kind = 0; kind < 3; kind++) {
        init(&s, &f, rx, 32, tx, 32);
        if (kind == 0) CHECK(offline_stream_input(&s, (enum offline_direction)99,
                            OFFLINE_IO_OK, rx, 1, 1, &used) == OFFLINE_INVALID && used == 0);
        if (kind == 1) CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK,
                            rx, 1, 1, NULL) == OFFLINE_INVALID);
        if (kind == 2) CHECK(offline_stream_step(&s, (enum offline_direction)-1) == OFFLINE_INVALID);
        CHECK(s.error == OFFLINE_ERROR_ARGUMENT);
        finish_unknown(&s, &f);
    }
    init(&s, &f, rx, 32, tx, 32);
    CHECK(offline_stream_ready(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_join(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_drain_complete(&s) == OFFLINE_INVALID);
    CHECK(offline_stream_complete(&s) == OFFLINE_INVALID);
    s.in_flight = SIZE_MAX; /* white-box counter boundary; never actually spawn workers */
    CHECK(offline_stream_begin(&s) == OFFLINE_INVALID && s.error == OFFLINE_ERROR_OVERFLOW);
    s.in_flight = 0;
    finish_unknown(&s, &f);
    init(&s, &f, rx, 32, tx, 32); connect_stream(&s);
    s.rx.accepted = s.rx.delivered = UINT64_MAX;
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, tx, 1, 1,
          &used) == OFFLINE_INVALID && used == 0 && s.error == OFFLINE_ERROR_OVERFLOW);
    finish_unknown(&s, &f);
    init(&s, &f, rx, 32, tx, 32); connect_stream(&s);
    const unsigned char private_data[] = "private-marker-CONNECT\r\n\0\xff";
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_OK, private_data,
          sizeof(private_data), sizeof(private_data), &used) == OFFLINE_OK);
    char diagnostic[512], tiny[2];
    int required = offline_stream_diagnostic(&s, diagnostic, sizeof(diagnostic));
    CHECK(required > 0 && (size_t)required == strlen(diagnostic));
    CHECK(strstr(diagnostic, "state=data-stream") && strstr(diagnostic, "recovery=unproven"));
    CHECK(!strstr(diagnostic, "private-marker") && !strstr(diagnostic, "CONNECT") && !strstr(diagnostic, "0xff"));
    CHECK(strstr(diagnostic, "rx_accepted=27") && strstr(diagnostic, "rx_queued=27"));
    CHECK(offline_stream_diagnostic(&s, tiny, sizeof(tiny)) == required && tiny[1] == '\0');
    CHECK(offline_stream_diagnostic(&s, NULL, 0) == required);
    CHECK(offline_stream_diagnostic(&s, NULL, 1) == -1);
    CHECK(offline_stream_diagnostic(NULL, diagnostic, sizeof(diagnostic)) == -1);
    CHECK(offline_stream_input(&s, OFFLINE_RX, OFFLINE_IO_ERROR,
          (const unsigned char *)(uintptr_t)1, SIZE_MAX, 0, &used) == OFFLINE_INVALID);
    CHECK(offline_stream_diagnostic(&s, diagnostic, sizeof(diagnostic)) > 0);
    CHECK(strstr(diagnostic, "error=io") && strstr(diagnostic, "recovery=unknown"));
    CHECK(!strstr(diagnostic, "private-marker") && !strstr(diagnostic, "CONNECT"));
    f.close_error = 1;
    finish_unknown(&s, &f);
    CHECK(s.error == OFFLINE_ERROR_IO); /* first failure label remains sticky */
    CHECK(offline_stream_diagnostic(&s, diagnostic, sizeof(diagnostic)) > 0);
    CHECK(strstr(diagnostic, "state=closed") && strstr(diagnostic, "recovery=unknown"));
    s.error = (enum offline_error)99;
    CHECK(offline_stream_diagnostic(&s, diagnostic, sizeof(diagnostic)) == -1);
    s.error = OFFLINE_ERROR_NONE; s.state = (enum offline_state)-1;
    CHECK(offline_stream_diagnostic(&s, diagnostic, sizeof(diagnostic)) == -1);
}

int main(void) {
    queue_tests();
    parser_tests();
    byte_stream_tests();
    io_tests();
    lifecycle_tests();
    validation_diagnostic_tests();
    printf("offline_stream unit checks=%u failures=%u (fake byte I/O/clock/lifetime events, no device access)\n",
           checks, failures);
    return failures ? 1 : 0;
}
