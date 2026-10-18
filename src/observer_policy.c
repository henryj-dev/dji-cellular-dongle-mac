#include "observer_policy.h"
#include "discovery.h"
#include "payload_analysis.h"

#include <inttypes.h>
#include <string.h>

int observer_config_allowed(const struct descriptor_config *config) {
    static const char expected[] =
        "0902d100050100a0fa0904000002ffffff000705810200020007050102000200"
        "0904010003ff00000005240010010524010000042402020524060000070583030a0009"
        "07058202000200070502020002000904020003ff000000052400100105240100000424"
        "02020524060000070585030a000907058402000200070503020002000904030003ff"
        "00000005240010010524010000042402020524060000070587030a0009070586020002"
        "00070504020002000904040003ffffff000705890308000907058802000200070505"
        "02000200";
    static const char digits[] = "0123456789abcdef";
    if (!config || config->length != 209 || config->configuration_value != 1 ||
        config->interface_count != 5 || config->alt_count != 5 || config->endpoint_count != 14)
        return 0;
    for (size_t i = 0; i < config->length; i++) {
        if (digits[config->raw[i] >> 4] != expected[i * 2] ||
            digits[config->raw[i] & 15] != expected[i * 2 + 1]) return 0;
    }
    for (size_t i = 0; i < 5; i++) {
        if (config->alts[i].interface_id != i || config->alts[i].alternate != 0 ||
            config->alts[i].endpoint_count != (i == 0 ? 2u : 3u)) return 0;
    }
    return 1;
}

int observer_pipe_matches(const struct descriptor_config *config, unsigned interface_id,
                          unsigned endpoint_index, uint8_t address, uint8_t type,
                          uint16_t max_packet, uint8_t interval) {
    if (!observer_config_allowed(config) || interface_id >= 5) return 0;
    const struct descriptor_alt *alt = &config->alts[interface_id];
    if (endpoint_index >= alt->endpoint_count) return 0;
    const struct descriptor_endpoint *ep = &config->endpoints[alt->first_endpoint + endpoint_index];
    return ep->address == address && ep->transfer_type == type &&
           ep->max_packet == max_packet && ep->interval == interval;
}

int observer_run(const struct observer_backend *b, struct observer_result *result, FILE *stream,
                 void (*on_read)(const uint8_t *, size_t, FILE *)) {
    if (!b || !result || !b->now_ms || !b->cancelled || !b->prepare || !b->validate ||
        !b->open || !b->read || !b->close || !b->release) return DJI_SYSTEM_ERROR;
    memset(result, 0, sizeof(*result));
    uint64_t start = b->now_ms(b->context), total_deadline = start + OBSERVER_TOTAL_MS;
    int status = DJI_OK;
    unsigned char buffer[on_read ? PAYLOAD_ANALYSIS_LIMIT : OBSERVER_BUFFER_SIZE];
    for (unsigned interface_id = 0; interface_id < OBSERVER_INTERFACES; interface_id++) {
        if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
        uint64_t now = b->now_ms(b->context);
        if (now >= total_deadline || result->bytes >= OBSERVER_BYTE_LIMIT) break;
        uint64_t deadline = now + OBSERVER_INTERFACE_MS;
        if (deadline > total_deadline) deadline = total_deadline;
        int opened = 0;
        uint8_t pipe = 0;
        status = b->prepare(b->context, interface_id);
        if (status != DJI_OK) goto release;
        status = b->validate(b->context, interface_id, 0, &pipe);
        if (status != DJI_OK) goto release;
        if (b->cancelled(b->context)) { status = DJI_CANCELLED; goto release; }
        if (b->now_ms(b->context) >= deadline) goto release;
        status = b->open(b->context);
        if (status != DJI_OK) goto release;
        opened = 1;
        status = b->validate(b->context, interface_id, 1, &pipe);
        if (status != DJI_OK) goto release;
        if (pipe == 0 || pipe > 3) { status = DJI_INVALID_DESCRIPTOR; goto release; }
        for (unsigned read_index = 0; read_index < OBSERVER_READ_LIMIT; read_index++) {
            if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
            now = b->now_ms(b->context);
            if (now >= deadline || result->bytes >= OBSERVER_BYTE_LIMIT) break;
            status = b->validate(b->context, interface_id, 1, &pipe);
            if (status != DJI_OK) break;
            if (pipe == 0 || pipe > 3) { status = DJI_INVALID_DESCRIPTOR; break; }
            if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
            now = b->now_ms(b->context);
            if (now >= deadline) break;
            uint32_t timeout = (uint32_t)(deadline - now);
            size_t remaining = OBSERVER_BYTE_LIMIT - result->bytes;
            uint32_t requested = remaining < sizeof(buffer) ? (uint32_t)remaining : (uint32_t)sizeof(buffer);
            uint32_t size = requested;
            status = b->read(b->context, pipe, buffer, &size, timeout);
            result->reads++;
            /* SDK: error size/buffer are invalid. Never inspect either on error. */
            if (status == OBSERVER_TIMEOUT) {
                result->timeouts++;
                if (stream) fprintf(stream, "interface=%u pipe=%u status=timeout bytes=0 protocol=unknown\n", interface_id, pipe);
                status = b->validate(b->context, interface_id, 1, &pipe);
                /* No retry, toggle resynchronization or clear-halt after timeout. */
                break;
            }
            if (status != DJI_OK) break;
            if (size > requested) { status = DJI_SYSTEM_ERROR; break; }
            if (b->cancelled(b->context)) { status = DJI_CANCELLED; break; }
            result->bytes += size;
            if (stream) fprintf(stream, "interface=%u pipe=%u status=received bytes=%u protocol=unknown\n", interface_id, pipe, size);
            if (on_read && size != 0) on_read(buffer, size, stream);
            status = b->validate(b->context, interface_id, 1, &pipe);
            if (status != DJI_OK || size == 0) break;
        }
release:
        if (status == DJI_OK && b->cancelled(b->context)) status = DJI_CANCELLED;
        if (opened) {
            int close_status = b->close(b->context);
            if (close_status != DJI_OK) status = DJI_CLOSE_ERROR;
        }
        b->release(b->context);
        if (status != DJI_OK) break;
    }
    result->duration_ms = b->now_ms(b->context) - start;
    if (on_read && stream && result->bytes == 0)
        fputs("analysis status=no-data protocol=unconfirmed\n", stream);
    if (stream) fprintf(stream, "status=%d bytes=%zu reads=%u timeouts=%u duration_ms=%" PRIu64 " protocol=unknown\n",
                        status, result->bytes, result->reads, result->timeouts, result->duration_ms);
    return status;
}
