#include "descriptor.h"

#include <string.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static int reject(const char **error, const char *message) {
    if (error) *error = message;
    return -1;
}

int descriptor_parse(const uint8_t *bytes, size_t length,
                     struct descriptor_config *config, const char **error) {
    if (error) *error = NULL;
    if (!bytes || !config || length < 9)
        return reject(error, "구성 헤더가 없거나 잘렸습니다");
    if (length > DESCRIPTOR_LIMIT)
        return reject(error, "구성 길이가 4096바이트를 넘습니다");
    if (bytes[0] < 9 || bytes[1] != 2 || bytes[0] > length)
        return reject(error, "구성 헤더 형식이 잘못됐습니다");
    size_t total = le16(bytes + 2);
    if (total < bytes[0] || total > DESCRIPTOR_LIMIT || total != length)
        return reject(error, "전체 길이가 실제 버퍼 길이와 다릅니다");
    memset(config, 0, sizeof(*config));
    memcpy(config->raw, bytes, length);
    config->length = length;
    config->configuration_value = bytes[5];
    config->attributes = bytes[7];
    config->max_power = bytes[8];
    config->records[config->record_count++] =
        (struct descriptor_record){0, bytes[0], bytes[1]};
    uint8_t interfaces[256] = {0};
    uint8_t endpoints[256] = {0};
    struct descriptor_alt *current = NULL;
    for (size_t off = bytes[0]; off < total;) {
        if (total - off < 2 || bytes[off] < 2 || bytes[off] > total - off)
            return reject(error, "하위 설명자 헤더 또는 길이가 잘못됐습니다");
        uint8_t len = bytes[off], type = bytes[off + 1];
        if (config->record_count >= DESCRIPTOR_RECORD_LIMIT)
            return reject(error, "설명자 개수 한도를 넘었습니다");
        config->records[config->record_count++] =
            (struct descriptor_record){(uint16_t)off, len, type};
        if (type == 2)
            return reject(error, "구성 내부에 중복 구성 헤더가 있습니다");
        if (type == 4) {
            if (len < 9)
                return reject(error, "인터페이스 설명자가 잘렸습니다");
            if (current && current->endpoint_count != current->declared_endpoints)
                return reject(error, "인터페이스 endpoint 개수가 다릅니다");
            if (config->alt_count >= DESCRIPTOR_ALT_LIMIT)
                return reject(error, "alternate setting 개수 한도를 넘었습니다");
            uint8_t id = bytes[off + 2], alt = bytes[off + 3];
            for (size_t i = 0; i < config->alt_count; i++) {
                if (config->alts[i].interface_id == id && config->alts[i].alternate == alt)
                    return reject(error, "인터페이스와 alternate setting 쌍이 중복됩니다");
            }
            if (!interfaces[id]) {
                interfaces[id] = 1;
                config->interface_count++;
            }
            current = &config->alts[config->alt_count++];
            *current = (struct descriptor_alt){
                .interface_id = id, .alternate = alt,
                .declared_endpoints = bytes[off + 4],
                .class_code = bytes[off + 5], .subclass = bytes[off + 6],
                .protocol = bytes[off + 7], .first_endpoint = config->endpoint_count
            };
            memset(endpoints, 0, sizeof(endpoints));
        } else if (type == 5) {
            if (!current || len < 7)
                return reject(error, "endpoint가 인터페이스보다 앞에 있거나 잘렸습니다");
            uint8_t address = bytes[off + 2];
            if ((address & 0x0f) == 0 || (address & 0x70) != 0)
                return reject(error, "endpoint 주소가 잘못됐습니다");
            if (endpoints[address])
                return reject(error, "같은 alternate setting에 endpoint 주소가 중복됩니다");
            if (current->endpoint_count >= current->declared_endpoints ||
                config->endpoint_count >= DESCRIPTOR_ENDPOINT_LIMIT)
                return reject(error, "endpoint 개수 한도를 넘었습니다");
            endpoints[address] = 1;
            uint16_t packet = le16(bytes + off + 4);
            config->endpoints[config->endpoint_count++] = (struct descriptor_endpoint){
                .address = address, .direction_in = (address & 0x80) != 0,
                .transfer_type = bytes[off + 3] & 3, .interval = bytes[off + 6],
                .max_packet = packet & 0x07ff, .raw_max_packet = packet
            };
            current->endpoint_count++;
        }
        /* Unknown types remain intact in raw[] and records[], without interpretation. */
        off += len;
    }
    if (current && current->endpoint_count != current->declared_endpoints)
        return reject(error, "마지막 인터페이스 endpoint 개수가 다릅니다");
    if (config->interface_count != bytes[4])
        return reject(error, "고유 인터페이스 개수가 선언과 다릅니다");
    return 0;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int descriptor_load_fixture(const char *path, struct descriptor_config *config,
                            const char **error) {
    if (error) *error = NULL;
    if (!path || !config) return reject(error, "fixture 경로 또는 출력이 없습니다");
    FILE *file = fopen(path, "rb");
    if (!file) return reject(error, "fixture 파일을 열지 못했습니다");
    uint8_t text[DESCRIPTOR_LIMIT + 1];
    size_t n = fread(text, 1, sizeof(text), file);
    int failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    if (failed) return reject(error, "fixture 파일을 읽지 못했습니다");
    if (n > DESCRIPTOR_LIMIT) return reject(error, "fixture 파일이 4096바이트를 넘습니다");
    uint8_t decoded[DESCRIPTOR_LIMIT];
    size_t count = 0;
    int high = -1;
    for (size_t i = 0; i < n; i++) {
        int c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int digit = hex_digit(c);
        if (digit < 0) return reject(error, "fixture에는 ASCII hex와 공백만 허용합니다");
        if (high < 0) high = digit;
        else {
            if (count >= sizeof(decoded)) return reject(error, "decoded 길이 한도를 넘었습니다");
            decoded[count++] = (uint8_t)((high << 4) | digit);
            high = -1;
        }
    }
    if (high >= 0) return reject(error, "fixture hex 자릿수가 홀수입니다");
    return descriptor_parse(decoded, count, config, error);
}

void descriptor_print(const struct descriptor_config *config, FILE *stream) {
    fprintf(stream, "config length=%zu value=%u unique_interfaces=%zu alternate_settings=%zu attributes=0x%02x maxPower=%u\n",
            config->length, config->configuration_value, config->interface_count,
            config->alt_count, config->attributes, config->max_power);
    for (size_t i = 0; i < config->alt_count; i++) {
        const struct descriptor_alt *alt = &config->alts[i];
        fprintf(stream, "interface id=%u alt=%u endpoints=%zu class=0x%02x subclass=0x%02x protocol=0x%02x\n",
                alt->interface_id, alt->alternate, alt->endpoint_count,
                alt->class_code, alt->subclass, alt->protocol);
        for (size_t j = 0; j < alt->endpoint_count; j++) {
            const struct descriptor_endpoint *ep = &config->endpoints[alt->first_endpoint + j];
            fprintf(stream, "endpoint address=0x%02x direction=%s type=%u maxPacket=%u interval=%u\n",
                    ep->address, ep->direction_in ? "IN" : "OUT", ep->transfer_type,
                    ep->max_packet, ep->interval);
        }
    }
    for (size_t i = 0; i < config->record_count; i++) {
        const struct descriptor_record *record = &config->records[i];
        if (record->type != 2 && record->type != 4 && record->type != 5)
            fprintf(stream, "unknown offset=%u type=0x%02x length=%u\n",
                    record->offset, record->type, record->length);
    }
    fputs("프로토콜 미확정: 설명자만으로 ACM/AT/QMI 또는 칩셋을 식별하지 않습니다.\n", stream);
}
