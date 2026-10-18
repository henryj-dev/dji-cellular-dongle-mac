#ifndef DJI_DESCRIPTOR_H
#define DJI_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DESCRIPTOR_LIMIT 4096u
#define DESCRIPTOR_RECORD_LIMIT (DESCRIPTOR_LIMIT / 2u)
#define DESCRIPTOR_ALT_LIMIT (DESCRIPTOR_LIMIT / 9u)
#define DESCRIPTOR_ENDPOINT_LIMIT (DESCRIPTOR_LIMIT / 7u)

struct descriptor_record {
    uint16_t offset;
    uint8_t length;
    uint8_t type;
};

struct descriptor_endpoint {
    uint8_t address;
    uint8_t direction_in;
    uint8_t transfer_type;
    uint8_t interval;
    uint16_t max_packet;
    uint16_t raw_max_packet;
};

struct descriptor_alt {
    uint8_t interface_id;
    uint8_t alternate;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t declared_endpoints;
    size_t first_endpoint;
    size_t endpoint_count;
};

struct descriptor_config {
    uint8_t raw[DESCRIPTOR_LIMIT];
    size_t length;
    uint8_t configuration_value;
    uint8_t attributes;
    uint8_t max_power;
    size_t interface_count;
    size_t alt_count;
    size_t endpoint_count;
    size_t record_count;
    struct descriptor_record records[DESCRIPTOR_RECORD_LIMIT];
    struct descriptor_alt alts[DESCRIPTOR_ALT_LIMIT];
    struct descriptor_endpoint endpoints[DESCRIPTOR_ENDPOINT_LIMIT];
};

/* Only a successful parse makes the output valid. No reads beyond length. */
int descriptor_parse(const uint8_t *bytes, size_t length,
                     struct descriptor_config *config, const char **error);
/* Strict ASCII hex with whitespace; both file and decoded data are bounded. */
int descriptor_load_fixture(const char *path, struct descriptor_config *config,
                            const char **error);
/* Metadata only: never prints raw descriptors, strings, or payload. */
void descriptor_print(const struct descriptor_config *config, FILE *stream);

#endif
