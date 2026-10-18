#include "reconnect.h"
#include "discovery.h"

int reconnect_wait(const struct reconnect_backend *b, uint64_t *expected_id) {
    *expected_id = 0;
    uint64_t start = b->now_ms(b->context), baseline = 0;
    if (b->cancelled(b->context)) return DJI_CANCELLED;
    if (start == UINT64_MAX) return DJI_SYSTEM_ERROR;
    int status = b->snapshot(b->context, &baseline);
    if (b->cancelled(b->context)) return DJI_CANCELLED;
    if (status != DJI_OK) return status;
    /* The count also bounds polling if a broken clock stops advancing. */
    for (unsigned poll = 0; poll < RECONNECT_WAIT_MS / RECONNECT_POLL_MS; poll++) {
        if (b->cancelled(b->context)) return DJI_CANCELLED;
        uint64_t now = b->now_ms(b->context);
        if (now == UINT64_MAX || now < start) return DJI_SYSTEM_ERROR;
        if (now - start >= RECONNECT_WAIT_MS) return DJI_RECONNECT_TIMEOUT;
        uint64_t remaining = RECONNECT_WAIT_MS - (now - start);
        uint32_t delay = remaining < RECONNECT_POLL_MS ? (uint32_t)remaining : RECONNECT_POLL_MS;
        status = b->sleep_ms(b->context, delay);
        if (b->cancelled(b->context)) return DJI_CANCELLED;
        if (status != DJI_OK) return status;
        now = b->now_ms(b->context);
        if (now == UINT64_MAX || now < start) return DJI_SYSTEM_ERROR;
        if (now - start >= RECONNECT_WAIT_MS) return DJI_RECONNECT_TIMEOUT;
        uint64_t current = 0;
        status = b->snapshot(b->context, &current);
        if (b->cancelled(b->context)) return DJI_CANCELLED;
        if (status != DJI_OK) return status;
        now = b->now_ms(b->context);
        if (now == UINT64_MAX || now < start) return DJI_SYSTEM_ERROR;
        if (now - start >= RECONNECT_WAIT_MS) return DJI_RECONNECT_TIMEOUT;
        if (current != 0 && current != baseline) {
            *expected_id = current;
            return DJI_OK;
        }
    }
    return DJI_RECONNECT_TIMEOUT;
}

int discovery_generation_matches(uint64_t expected_id, uint64_t current_id) {
    return expected_id != 0 && current_id == expected_id ? DJI_OK : DJI_GONE;
}
