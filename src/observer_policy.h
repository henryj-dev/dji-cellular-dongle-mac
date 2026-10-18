#ifndef DJI_OBSERVER_POLICY_H
#define DJI_OBSERVER_POLICY_H

#include "descriptor.h"
#include <stdint.h>

#define OBSERVER_INTERFACES 5u
#define OBSERVER_INTERFACE_MS 2000u
#define OBSERVER_TOTAL_MS 15000u
#define OBSERVER_BYTE_LIMIT 65536u
#define OBSERVER_READ_LIMIT 8u
#define OBSERVER_BUFFER_SIZE 8192u

/* Internal transport statuses, never evidence of a recognized protocol. */
#define OBSERVER_TIMEOUT 100

struct observer_backend {
    void *context;
    uint64_t (*now_ms)(void *context);
    int (*cancelled)(void *context);
    int (*prepare)(void *context, unsigned interface_id);
    int (*validate)(void *context, unsigned interface_id, int opened, uint8_t *bulk_pipe);
    int (*open)(void *context);
    int (*read)(void *context, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout_ms);
    int (*close)(void *context);
    void (*release)(void *context);
};

struct observer_result {
    size_t bytes;
    unsigned reads;
    unsigned timeouts;
    uint64_t duration_ms;
};

/* Requires a successfully parsed configuration. Exact approved cached fixture only. */
int observer_config_allowed(const struct descriptor_config *config);
int observer_pipe_matches(const struct descriptor_config *config, unsigned interface_id,
                          unsigned endpoint_index, uint8_t address, uint8_t type,
                          uint16_t max_packet, uint8_t interval);
/* Injectable I/O is used by genuine offline unit tests, not runtime success stubs. */
/* Optional callback borrows only successful bytes, never error buffers.
 * With a callback each read is capped at 4096; other observation bounds stay unchanged. */
int observer_run(const struct observer_backend *backend, struct observer_result *result,
                 FILE *stream, void (*on_read)(const uint8_t *, size_t, FILE *));
#endif
