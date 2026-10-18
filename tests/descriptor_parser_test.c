#include "../src/descriptor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
static unsigned failures;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static struct descriptor_config fixture;
static struct descriptor_config parsed;

static int parse(const uint8_t *bytes, size_t n) {
    const char *error = NULL;
    int result = descriptor_parse(bytes, n, &parsed, &error);
    CHECK((result == 0) == (error == NULL));
    return result;
}

static void file_case(const uint8_t *bytes, size_t n, int expected_success) {
    char path[] = "/tmp/dji-descriptor-test.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    FILE *file = fdopen(fd, "wb");
    CHECK(file != NULL);
    if (!file) { close(fd); unlink(path); return; }
    CHECK(fwrite(bytes, 1, n, file) == n);
    CHECK(fclose(file) == 0);
    const char *error = NULL;
    int result = descriptor_load_fixture(path, &parsed, &error);
    CHECK((result == 0) == expected_success);
    CHECK((result == 0) == (error == NULL));
    CHECK(unlink(path) == 0);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *error = NULL;
    CHECK(descriptor_load_fixture(argv[1], &fixture, &error) == 0);
    if (failures) return 1;
    CHECK(fixture.length == 209);
    CHECK(fixture.configuration_value == 1);
    CHECK(fixture.interface_count == 5);
    CHECK(fixture.alt_count == 5);
    CHECK(fixture.endpoint_count == 14);
    CHECK(fixture.record_count == 32);
    for (size_t i = 0; i < fixture.alt_count; i++) {
        CHECK(fixture.alts[i].interface_id == i);
        CHECK(fixture.alts[i].alternate == 0);
        CHECK(fixture.alts[i].endpoint_count == (i == 0 ? 2u : 3u));
    }
    CHECK(fixture.endpoints[0].address == 0x81);
    CHECK(fixture.endpoints[0].direction_in == 1);
    CHECK(fixture.endpoints[0].transfer_type == 2);
    CHECK(fixture.endpoints[0].max_packet == 512);
    CHECK(fixture.endpoints[1].direction_in == 0);
    CHECK(fixture.endpoints[2].transfer_type == 3);
    CHECK(fixture.endpoints[2].max_packet == 10);
    size_t unknown = 0;
    for (size_t i = 0; i < fixture.record_count; i++) {
        if (fixture.records[i].type == 0x24) unknown++;
    }
    CHECK(unknown == 12);
    CHECK(parse(fixture.raw, fixture.length) == 0);
    CHECK(memcmp(parsed.raw, fixture.raw, fixture.length) == 0);

    /* Every prefix tests actual supplied buffer length, including initial headers. */
    for (size_t n = 0; n < fixture.length; n++)
        CHECK(parse(fixture.raw, n) != 0);
    CHECK(descriptor_parse(NULL, 9, &parsed, &error) != 0);
    CHECK(descriptor_parse(fixture.raw, fixture.length, NULL, &error) != 0);
    CHECK(descriptor_parse(fixture.raw, fixture.length, &parsed, NULL) == 0);

    uint8_t changed[DESCRIPTOR_LIMIT + 1];
    memcpy(changed, fixture.raw, fixture.length);
    changed[2] = 208;
    CHECK(parse(changed, fixture.length) != 0);
    changed[2] = 210;
    CHECK(parse(changed, fixture.length) != 0);
    changed[2] = 0;
    changed[3] = 0x10;
    CHECK(parse(changed, fixture.length) != 0);
    changed[2] = 1;
    CHECK(parse(changed, fixture.length) != 0);
    CHECK(parse(changed, DESCRIPTOR_LIMIT + 1) != 0);

    /* Malformed bLength at every descriptor, not just one sample. */
    for (size_t i = 0; i < fixture.record_count; i++) {
        size_t off = fixture.records[i].offset;
        memcpy(changed, fixture.raw, fixture.length);
        changed[off] = 0;
        CHECK(parse(changed, fixture.length) != 0);
        changed[off] = 1;
        CHECK(parse(changed, fixture.length) != 0);
        changed[off] = 255;
        CHECK(parse(changed, fixture.length) != 0);
    }
    memcpy(changed, fixture.raw, fixture.length);
    changed[1] = 4;
    CHECK(parse(changed, fixture.length) != 0);
    memcpy(changed, fixture.raw, fixture.length);
    changed[4] = 4;
    CHECK(parse(changed, fixture.length) != 0);
    changed[4] = 6;
    CHECK(parse(changed, fixture.length) != 0);
    memcpy(changed, fixture.raw, fixture.length);
    changed[13] = 1;
    CHECK(parse(changed, fixture.length) != 0);
    changed[13] = 3;
    CHECK(parse(changed, fixture.length) != 0);
    memcpy(changed, fixture.raw, fixture.length);
    changed[33] = 0; /* IF1 becomes a duplicate IF0/alt0 pair. */
    CHECK(parse(changed, fixture.length) != 0);
    memcpy(changed, fixture.raw, fixture.length);
    changed[27] = 0x81; /* IF0's second endpoint duplicates its first. */
    CHECK(parse(changed, fixture.length) != 0);
    changed[27] = 0x80;
    CHECK(parse(changed, fixture.length) != 0);
    changed[27] = 0xf1;
    CHECK(parse(changed, fixture.length) != 0);
    memcpy(changed, fixture.raw, fixture.length);
    changed[41 + 1] = 0x99; /* Unknown descriptors preserve bytes and metadata. */
    CHECK(parse(changed, fixture.length) == 0);
    CHECK(parsed.records[5].type == 0x99);
    CHECK(memcmp(parsed.raw, changed, fixture.length) == 0);

    uint8_t alts[] = {
        9,2,41,0,1,1,0,0x80,0,
        9,4,0,0,1,0xff,0,0,0, 7,5,0x81,2,0,2,0,
        9,4,0,1,1,0xff,0,0,0, 7,5,0x81,2,0,0x12,0
    };
    CHECK(parse(alts, sizeof(alts)) == 0);
    CHECK(parsed.interface_count == 1);
    CHECK(parsed.alt_count == 2);
    CHECK(parsed.alts[0].endpoint_count == 1);
    CHECK(parsed.alts[1].endpoint_count == 1);
    CHECK(parsed.endpoints[1].max_packet == 512);
    CHECK(parsed.endpoints[1].raw_max_packet == 0x1200);
    alts[28] = 0;
    CHECK(parse(alts, sizeof(alts)) != 0);

    uint8_t before_interface[] = {9,2,25,0,1,1,0,0x80,0,
                                 7,5,0x81,2,0,2,0,
                                 9,4,0,0,1,0xff,0,0,0};
    CHECK(parse(before_interface, sizeof(before_interface)) != 0);
    uint8_t short_interface[] = {9,2,17,0,1,1,0,0x80,0, 8,4,0,0,0,0xff,0,0};
    CHECK(parse(short_interface, sizeof(short_interface)) != 0);
    uint8_t short_endpoint[] = {9,2,24,0,1,1,0,0x80,0,
                              9,4,0,0,1,0xff,0,0,0, 6,5,0x81,2,0,2};
    CHECK(parse(short_endpoint, sizeof(short_endpoint)) != 0);
    uint8_t trailing_header[] = {9,2,19,0,1,1,0,0x80,0, 9,4,0,0,0,0xff,0,0,0, 2};
    CHECK(parse(trailing_header, sizeof(trailing_header)) != 0);
    uint8_t unknown_first[] = {9,2,21,0,1,1,0,0x80,0, 3,0x99,0xff,
                              9,4,0,0,0,0xff,0,0,0};
    CHECK(parse(unknown_first, sizeof(unknown_first)) == 0);
    CHECK(parsed.records[1].type == 0x99);
    CHECK(parsed.raw[11] == 0xff);
    unknown_first[10] = 2;
    CHECK(parse(unknown_first, sizeof(unknown_first)) != 0);

    /* Exact 4096-byte input is valid; one more byte is rejected. */
    uint8_t minimal[] = {9,2,18,0,1,1,0,0x80,0, 9,4,0,0,0,0xff,0,0,0};
    memcpy(changed, minimal, sizeof(minimal));
    changed[2] = 0;
    changed[3] = 0x10;
    for (size_t i = sizeof(minimal); i < DESCRIPTOR_LIMIT; i += 2) {
        changed[i] = 2;
        changed[i + 1] = 0x99;
    }
    CHECK(parse(changed, DESCRIPTOR_LIMIT) == 0);
    CHECK(parsed.length == DESCRIPTOR_LIMIT);
    CHECK(parse(changed, DESCRIPTOR_LIMIT + 1) != 0);

    static const uint8_t valid_text[] = "09 02 12 00 01 01 00 80 00\n09 04 00 00 00 FF 00 00 00\t\r";
    file_case(valid_text, sizeof(valid_text) - 1, 1);
    static const uint8_t odd[] = "090";
    file_case(odd, sizeof(odd) - 1, 0);
    static const uint8_t invalid[] = "0x09";
    file_case(invalid, sizeof(invalid) - 1, 0);
    static const uint8_t embedded_nul[] = {'0','9',0,'0','2'};
    file_case(embedded_nul, sizeof(embedded_nul), 0);
    static const uint8_t high_ascii[] = {'0','9',0xff};
    file_case(high_ascii, sizeof(high_ascii), 0);
    file_case(valid_text, 0, 0);
    memset(changed, ' ', sizeof(changed));
    memcpy(changed, valid_text, sizeof(valid_text) - 1);
    file_case(changed, DESCRIPTOR_LIMIT, 1);
    file_case(changed, DESCRIPTOR_LIMIT + 1, 0);
    CHECK(descriptor_load_fixture("/nonexistent-dji-fixture", &parsed, &error) != 0);
    CHECK(descriptor_load_fixture(NULL, &parsed, &error) != 0);

    printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
