#ifndef DJI_RECONNECT_H
#define DJI_RECONNECT_H

#include <stdint.h>

#define RECONNECT_WAIT_MS 90000u
#define RECONNECT_POLL_MS 100u

/* Snapshot is registry metadata only: ID 0 means absent, never open or USB I/O. */
struct reconnect_backend {
    void *context;
    uint64_t (*now_ms)(void *context);
    int (*cancelled)(void *context);
    int (*snapshot)(void *context, uint64_t *registry_id);
    int (*sleep_ms)(void *context, uint32_t milliseconds);
};

/* Captures the baseline once; accepts only a different, nonzero singleton ID. */
int reconnect_wait(const struct reconnect_backend *backend, uint64_t *expected_id);

#endif
