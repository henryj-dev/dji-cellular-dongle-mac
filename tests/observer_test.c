#include "../src/observer_policy.h"
#include "../src/discovery.h"
#include "../src/payload_analysis.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

struct fake {
    uint64_t now;
    unsigned prepared, opens, reads, closes, releases, validates;
    unsigned current, reads_on_interface, max_reads, last_timeout;
    int open_status, read_status, close_status, prepare_status;
    int fail_validation_at, validation_status, cancel_at_read, cancel_at_prepare;
    uint8_t pipe_override;
    uint32_t response_size;
    uint64_t prepare_delay, read_delay, validate_delay;
    size_t requested_bytes;
    int active;
    const uint8_t *response;
};
static uint64_t fake_now(void *p) { return ((struct fake *)p)->now; }
static int fake_cancelled(void *p) {
    struct fake *f = p;
    return (f->cancel_at_read && f->reads >= (unsigned)f->cancel_at_read) ||
           (f->cancel_at_prepare && f->prepared >= (unsigned)f->cancel_at_prepare);
}
static int fake_prepare(void *p, unsigned interface_id) {
    struct fake *f = p;
    CHECK(!f->active);
    CHECK(interface_id < OBSERVER_INTERFACES);
    f->prepared++; f->current = interface_id; f->reads_on_interface = 0;
    f->now += f->prepare_delay;
    return f->prepare_status;
}
static int fake_validate(void *p, unsigned interface_id, int opened, uint8_t *pipe) {
    struct fake *f = p;
    CHECK(interface_id == f->current);
    CHECK(opened == f->active);
    f->validates++; f->now += f->validate_delay;
    *pipe = f->pipe_override ? f->pipe_override : 2;
    if (f->fail_validation_at && f->validates >= (unsigned)f->fail_validation_at)
        return f->validation_status;
    return DJI_OK;
}
static int fake_open(void *p) {
    struct fake *f = p;
    CHECK(!f->active);
    f->opens++;
    if (f->open_status == DJI_OK) f->active = 1;
    return f->open_status;
}
static int fake_read(void *p, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct fake *f = p;
    CHECK(f->active);
    CHECK(pipe == 2); /* No pipe 0, OUT or interrupt request exists in this double. */
    CHECK(timeout > 0 && timeout <= OBSERVER_INTERFACE_MS);
    CHECK(*size > 0 && *size <= OBSERVER_BUFFER_SIZE);
    f->requested_bytes += *size;
    f->reads++; f->reads_on_interface++;
    if (f->reads_on_interface > f->max_reads) f->max_reads = f->reads_on_interface;
    f->last_timeout = timeout;
    f->now += f->read_status == OBSERVER_TIMEOUT ? timeout : f->read_delay;
    memset(buffer, 0xa5, *size);
    if (f->response && f->response_size <= *size) memcpy(buffer, f->response, f->response_size);
    /* Error returns deliberately lie about size: production must ignore it. */
    *size = f->read_status == DJI_OK ? f->response_size : UINT32_MAX;
    return f->read_status;
}
static int fake_close(void *p) {
    struct fake *f = p;
    CHECK(f->active);
    f->closes++; f->active = 0;
    return f->close_status;
}
static void fake_release(void *p) {
    struct fake *f = p;
    CHECK(!f->active);
    f->releases++;
}
static int run(struct fake *f, struct observer_result *result) {
    const struct observer_backend backend = {f, fake_now, fake_cancelled, fake_prepare,
        fake_validate, fake_open, fake_read, fake_close, fake_release};
    int status = observer_run(&backend, result, NULL, NULL);
    CHECK(!f->active);
    CHECK(f->closes <= f->opens);
    CHECK(f->max_reads <= OBSERVER_READ_LIMIT);
    CHECK(result->bytes <= OBSERVER_BYTE_LIMIT);
    return status;
}

static unsigned analyzed_reads;
static size_t analyzed_bytes;
static void analyze_read(const uint8_t *bytes, size_t size, FILE *stream) {
    CHECK(size > 0 && size <= PAYLOAD_ANALYSIS_LIMIT);
    analyzed_reads++;
    analyzed_bytes += size;
    payload_analyze_read(bytes, size, stream);
}

static unsigned cached_calls, observe_calls, wait_calls;
int discovery_wait_reconnect(void) { wait_calls++; return DJI_RECONNECT_TIMEOUT; }
int discovery_inspect(void) { cached_calls++; return DJI_NOT_FOUND; }
static unsigned analyze_calls;
int discovery_observe(int analyze) { observe_calls++; analyze_calls += analyze != 0; return DJI_BUSY; }
static unsigned probe_calls;
int discovery_probe_at(void) { probe_calls++; return DJI_GONE; }
static unsigned probe_ati_calls;
int discovery_probe_ati(void) { probe_ati_calls++; return DJI_BUSY; }
static unsigned probe_status_calls;
int discovery_probe_status(void) { probe_status_calls++; return DJI_ACCESS; }
static unsigned probe_pdp_calls;
int discovery_probe_pdp(void) { probe_pdp_calls++; return DJI_UNSUPPORTED; }
static unsigned probe_data_support_calls;
int discovery_probe_data_support(void) { probe_data_support_calls++; return DJI_ACCESS; }
#define main tested_cli_main
#include "../src/main.c"
#undef main

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    static struct descriptor_config config;
    const char *error = NULL;
    CHECK(descriptor_load_fixture(argv[1], &config, &error) == 0);
    CHECK(observer_config_allowed(&config));
    for (unsigned i = 0; i < 5; i++) {
        const struct descriptor_alt *alt = &config.alts[i];
        unsigned bulk_in = 0;
        for (unsigned e = 0; e < alt->endpoint_count; e++) {
            const struct descriptor_endpoint *ep = &config.endpoints[alt->first_endpoint + e];
            CHECK(observer_pipe_matches(&config, i, e, ep->address, ep->transfer_type, ep->max_packet, ep->interval));
            CHECK(!observer_pipe_matches(&config, i, e, ep->address ^ 0x80, ep->transfer_type, ep->max_packet, ep->interval));
            CHECK(!observer_pipe_matches(&config, i, e, ep->address, ep->transfer_type ^ 1, ep->max_packet, ep->interval));
            CHECK(!observer_pipe_matches(&config, i, e, ep->address, ep->transfer_type, ep->max_packet + 1, ep->interval));
            CHECK(!observer_pipe_matches(&config, i, e, ep->address, ep->transfer_type, ep->max_packet, ep->interval ^ 1));
            if (ep->direction_in && ep->transfer_type == 2) bulk_in++;
        }
        CHECK(bulk_in == 1);
    }
    CHECK(!observer_pipe_matches(&config, 5, 0, 0x81, 2, 512, 0));
    CHECK(!observer_pipe_matches(&config, 0, 8, 0x81, 2, 512, 0));
    config.configuration_value = 2; CHECK(!observer_config_allowed(&config)); config.configuration_value = 1;
    config.alts[0].alternate = 1; CHECK(!observer_config_allowed(&config)); config.alts[0].alternate = 0;
    config.raw[208] ^= 1; CHECK(!observer_config_allowed(&config)); config.raw[208] ^= 1;
    for (size_t i = 0; i < config.length; i++) {
        config.raw[i] ^= 1; CHECK(!observer_config_allowed(&config)); config.raw[i] ^= 1;
    }
    CHECK(!observer_config_allowed(NULL));

    struct observer_result result;
    struct fake f = {.response_size = 1};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(f.prepared == 5 && f.opens == 5 && f.closes == 5 && f.releases == 5);
    CHECK(result.reads == 40 && result.bytes == 40 && f.max_reads == 8);

    f = (struct fake){.response_size = OBSERVER_BUFFER_SIZE};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(result.bytes == OBSERVER_BYTE_LIMIT && result.reads == 8);
    CHECK(f.prepared == 1 && f.closes == 1 && f.requested_bytes == OBSERVER_BYTE_LIMIT);

    f = (struct fake){.read_status = OBSERVER_TIMEOUT};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(result.bytes == 0 && result.timeouts == 5 && result.reads == 5);
    CHECK(f.closes == 5 && result.duration_ms == 10000);

    f = (struct fake){.response_size = 0};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(result.reads == 5 && result.bytes == 0 && f.closes == 5);

    f = (struct fake){.response_size = 1, .read_delay = 1500};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(result.reads == 10 && f.last_timeout == 500 && f.closes == 5);
    /* A double overruns its timeout: the next I/O must still never start late. */
    CHECK(result.duration_ms == 15000);

    f = (struct fake){.prepare_delay = 2000};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(f.opens == 0 && f.reads == 0 && f.releases == 5);
    f = (struct fake){.prepare_delay = 15000};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(f.prepared == 1 && f.opens == 0 && result.duration_ms == 15000);

    f = (struct fake){.response_size = 1, .validate_delay = 999};
    CHECK(run(&f, &result) == DJI_OK);
    CHECK(f.reads == 0 && f.closes == f.opens);

    const int errors[] = {DJI_GONE, DJI_ACCESS, DJI_BUSY, DJI_UNSUPPORTED, DJI_SYSTEM_ERROR};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        f = (struct fake){.open_status = errors[i]};
        CHECK(run(&f, &result) == errors[i]);
        CHECK(f.opens == 1 && f.closes == 0 && f.releases == 1 && f.reads == 0);
        f = (struct fake){.read_status = errors[i]};
        CHECK(run(&f, &result) == errors[i]);
        CHECK(result.bytes == 0 && f.closes == 1 && f.reads == 1 && f.releases == 1);
        f = (struct fake){.prepare_status = errors[i]};
        CHECK(run(&f, &result) == errors[i]);
        CHECK(f.opens == 0 && f.releases == 1);
    }
    for (int validation = 1; validation <= 4; validation++) {
        f = (struct fake){.response_size = 1, .fail_validation_at = validation, .validation_status = DJI_GONE};
        CHECK(run(&f, &result) == DJI_GONE);
        CHECK(f.closes == (validation == 1 ? 0u : 1u));
        CHECK(f.reads == (validation == 4 ? 1u : 0u));
        CHECK(f.releases == 1);
    }
    f = (struct fake){.cancel_at_prepare = 1};
    CHECK(run(&f, &result) == DJI_CANCELLED);
    CHECK(f.opens == 0 && f.releases == 1);
    f = (struct fake){.response_size = 100, .cancel_at_read = 1};
    CHECK(run(&f, &result) == DJI_CANCELLED);
    CHECK(f.reads == 1 && f.closes == 1 && result.bytes == 0);
    f = (struct fake){.read_status = DJI_GONE, .close_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &result) == DJI_CLOSE_ERROR);
    CHECK(f.closes == 1 && f.releases == 1);
    f = (struct fake){.response_size = OBSERVER_BUFFER_SIZE + 1};
    CHECK(run(&f, &result) == DJI_SYSTEM_ERROR);
    CHECK(result.bytes == 0 && f.closes == 1);

    f = (struct fake){.pipe_override = 4};
    CHECK(run(&f, &result) == DJI_INVALID_DESCRIPTOR);
    CHECK(f.reads == 0 && f.closes == 1);
    f = (struct fake){.read_status = OBSERVER_TIMEOUT, .cancel_at_read = 5};
    CHECK(run(&f, &result) == DJI_CANCELLED);
    CHECK(f.closes == 5 && result.bytes == 0);
    f = (struct fake){.read_status = OBSERVER_TIMEOUT, .fail_validation_at = 4, .validation_status = DJI_GONE};
    CHECK(run(&f, &result) == DJI_GONE);
    CHECK(f.reads == 1 && f.closes == 1 && result.bytes == 0);

    FILE *output = tmpfile();
    CHECK(output != NULL);
    if (output) {
        f = (struct fake){.response_size = 1};
        const struct observer_backend backend = {&f, fake_now, fake_cancelled, fake_prepare,
            fake_validate, fake_open, fake_read, fake_close, fake_release};
        CHECK(observer_run(&backend, &result, output, NULL) == DJI_OK);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        char text[8192];
        size_t length = fread(text, 1, sizeof(text) - 1, output);
        text[length] = '\0';
        CHECK(strstr(text, "protocol=unknown") != NULL);
        CHECK(strstr(text, "bytes=40 reads=40") != NULL);
        CHECK(strstr(text, "a5") == NULL);
        CHECK(fclose(output) == 0);
    }

    char *default_args[] = {"dji-lte-mac", NULL};
    CHECK(tested_cli_main(1, default_args) == DJI_NOT_FOUND);
    CHECK(cached_calls == 1 && observe_calls == 0);
    char *observe_args[] = {"dji-lte-mac", "--observe", NULL};
    CHECK(tested_cli_main(2, observe_args) == DJI_BUSY);
    CHECK(cached_calls == 1 && observe_calls == 1); /* Double, never real IOKit. */
    CHECK(analyze_calls == 0 && analyzed_reads == 0); /* Defaults never analyze. */
    char *analyze_args[] = {"dji-lte-mac", "--observe", "--analyze", NULL};
    CHECK(tested_cli_main(3, analyze_args) == DJI_BUSY);
    CHECK(cached_calls == 1 && observe_calls == 2 && analyze_calls == 1);
    CHECK(wait_calls == 0);
    char *wait_args[] = {"dji-lte-mac", "--wait-reconnect", "--observe", "--analyze", NULL};
    CHECK(tested_cli_main(4, wait_args) == DJI_RECONNECT_TIMEOUT);
    CHECK(wait_calls == 1 && cached_calls == 1 && observe_calls == 2 && analyze_calls == 1);
    char *invalid[][6] = {
        {"dji-lte-mac", "--wait-reconnect", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--observe", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--analyze", NULL},
        {"dji-lte-mac", "--observe", "--wait-reconnect", "--analyze", NULL},
        {"dji-lte-mac", "--observe", "--analyze", "--wait-reconnect", NULL},
        {"dji-lte-mac", "--analyze", "--wait-reconnect", "--observe", NULL},
        {"dji-lte-mac", "--analyze", "--observe", "--wait-reconnect", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--analyze", "--observe", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--observe", "--analyze", "extra", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--wait-reconnect", "--observe", "--analyze", NULL},
        {"dji-lte-mac", "--fixture", "--wait-reconnect", NULL},
        {"dji-lte-mac", "--help", "--wait-reconnect", NULL},
        {"dji-lte-mac", "--analyze", NULL},
        {"dji-lte-mac", "--analyze", "--observe", NULL},
        {"dji-lte-mac", "--observe", "--analyze", "extra", NULL},
        {"dji-lte-mac", "--fixture", "--analyze", NULL},
        {"dji-lte-mac", "--fixture", "--analyze", argv[1], NULL},
        {"dji-lte-mac", "--fixture", argv[1], "--analyze", NULL},
        {"dji-lte-mac", "--help", "--analyze", NULL},
        {"dji-lte-mac", "--observe", "--analyze", "--analyze", NULL},
        {"dji-lte-mac", "--observe", "invalid", NULL},
        {"dji-lte-mac", "--probe-at", "extra", NULL},
        {"dji-lte-mac", "--observe", "--probe-at", NULL},
        {"dji-lte-mac", "--probe-at", "--observe", NULL},
        {"dji-lte-mac", "--probe-at", "--analyze", NULL},
        {"dji-lte-mac", "--probe-at", "--probe-at", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--probe-at", NULL},
        {"dji-lte-mac", "--fixture", "--probe-at", NULL},
        {"dji-lte-mac", "--help", "--probe-at", NULL},
        {"dji-lte-mac", "--probe-at=2", NULL},
        {"dji-lte-mac", "--probe", NULL},
        {"dji-lte-mac", "--probe-ati", "extra", NULL},
        {"dji-lte-mac", "--probe-at", "--probe-ati", NULL},
        {"dji-lte-mac", "--probe-ati", "--probe-at", NULL},
        {"dji-lte-mac", "--observe", "--probe-ati", NULL},
        {"dji-lte-mac", "--probe-ati", "--observe", NULL},
        {"dji-lte-mac", "--probe-ati", "--analyze", NULL},
        {"dji-lte-mac", "--probe-ati", "--probe-ati", NULL},
        {"dji-lte-mac", "--fixture", "--probe-ati", NULL},
        {"dji-lte-mac", "--help", "--probe-ati", NULL},
        {"dji-lte-mac", "--probe-ati=1", NULL},
        {"dji-lte-mac", "--probe-atI", NULL},
        {"dji-lte-mac", "--probe-status", "extra", NULL},
        {"dji-lte-mac", "--probe-status", "--probe-at", NULL},
        {"dji-lte-mac", "--probe-at", "--probe-status", NULL},
        {"dji-lte-mac", "--probe-ati", "--probe-status", NULL},
        {"dji-lte-mac", "--probe-status", "--probe-ati", NULL},
        {"dji-lte-mac", "--observe", "--probe-status", NULL},
        {"dji-lte-mac", "--probe-status", "--observe", NULL},
        {"dji-lte-mac", "--probe-status", "--probe-status", NULL},
        {"dji-lte-mac", "--fixture", "--probe-status", NULL},
        {"dji-lte-mac", "--help", "--probe-status", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--probe-status", NULL},
        {"dji-lte-mac", "--probe-status=cpin", NULL},
        {"dji-lte-mac", "--probe-pdp", "extra", NULL},
        {"dji-lte-mac", "--probe-pdp", "--probe-status", NULL},
        {"dji-lte-mac", "--probe-status", "--probe-pdp", NULL},
        {"dji-lte-mac", "--probe-at", "--probe-pdp", NULL},
        {"dji-lte-mac", "--probe-pdp", "--probe-ati", NULL},
        {"dji-lte-mac", "--observe", "--probe-pdp", NULL},
        {"dji-lte-mac", "--probe-pdp", "--probe-pdp", NULL},
        {"dji-lte-mac", "--fixture", "--probe-pdp", NULL},
        {"dji-lte-mac", "--help", "--probe-pdp", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--probe-pdp", NULL},
        {"dji-lte-mac", "--probe-pdp=1", NULL},
        {"dji-lte-mac", "--probe-data-support", "extra", NULL},
        {"dji-lte-mac", "--probe-data-support", "--probe-at", NULL},
        {"dji-lte-mac", "--probe-at", "--probe-data-support", NULL},
        {"dji-lte-mac", "--probe-data-support", "--probe-ati", NULL},
        {"dji-lte-mac", "--probe-ati", "--probe-data-support", NULL},
        {"dji-lte-mac", "--probe-data-support", "--probe-status", NULL},
        {"dji-lte-mac", "--probe-status", "--probe-data-support", NULL},
        {"dji-lte-mac", "--probe-data-support", "--probe-pdp", NULL},
        {"dji-lte-mac", "--probe-pdp", "--probe-data-support", NULL},
        {"dji-lte-mac", "--probe-data-support", "--observe", NULL},
        {"dji-lte-mac", "--observe", "--probe-data-support", NULL},
        {"dji-lte-mac", "--probe-data-support", "--analyze", NULL},
        {"dji-lte-mac", "--probe-data-support", "--probe-data-support", NULL},
        {"dji-lte-mac", "--fixture", "--probe-data-support", NULL},
        {"dji-lte-mac", "--help", "--probe-data-support", NULL},
        {"dji-lte-mac", "--wait-reconnect", "--probe-data-support", NULL},
        {"dji-lte-mac", "--probe-data-support=PPP", NULL},
        {"dji-lte-mac", "--probe-data-support", "PPP", "1", NULL}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        int count = 0;
        while (invalid[i][count]) count++;
        CHECK(tested_cli_main(count, invalid[i]) == DJI_USAGE);
        CHECK(cached_calls == 1 && observe_calls == 2 && analyze_calls == 1 && wait_calls == 1);
        CHECK(probe_calls == 0 && probe_ati_calls == 0 && probe_status_calls == 0 && probe_pdp_calls == 0 &&
              probe_data_support_calls == 0);
    }
    char *probe_args[] = {"dji-lte-mac", "--probe-at", NULL};
    CHECK(tested_cli_main(2, probe_args) == DJI_GONE);
    CHECK(probe_calls == 1 && cached_calls == 1 && observe_calls == 2 && wait_calls == 1);
    char *probe_ati_args[] = {"dji-lte-mac", "--probe-ati", NULL};
    CHECK(tested_cli_main(2, probe_ati_args) == DJI_BUSY);
    CHECK(probe_ati_calls == 1 && probe_calls == 1 && observe_calls == 2);
    char *probe_status_args[] = {"dji-lte-mac", "--probe-status", NULL};
    CHECK(tested_cli_main(2, probe_status_args) == DJI_ACCESS);
    CHECK(probe_status_calls == 1 && probe_ati_calls == 1 && probe_calls == 1 && observe_calls == 2);
    char *probe_pdp_args[] = {"dji-lte-mac", "--probe-pdp", NULL};
    CHECK(tested_cli_main(2, probe_pdp_args) == DJI_UNSUPPORTED);
    CHECK(probe_pdp_calls == 1 && probe_status_calls == 1 && observe_calls == 2);
    char *probe_data_support_args[] = {"dji-lte-mac", "--probe-data-support", NULL};
    CHECK(tested_cli_main(2, probe_data_support_args) == DJI_ACCESS);
    CHECK(probe_data_support_calls == 1 && probe_pdp_calls == 1 && probe_status_calls == 1 &&
          probe_ati_calls == 1 && probe_calls == 1 && observe_calls == 2 && cached_calls == 1 && wait_calls == 1);
    output = tmpfile();
    CHECK(output != NULL);
    if (output) {
        const struct observer_backend backend = {&f, fake_now, fake_cancelled, fake_prepare,
            fake_validate, fake_open, fake_read, fake_close, fake_release};
        const uint8_t token[] = "\r\n+CME ERROR: synthetic-private-value\r\n";
        f = (struct fake){.response_size = sizeof(token) - 1, .response = token};
        CHECK(observer_run(&backend, &result, output, analyze_read) == DJI_OK);
        CHECK(analyzed_reads == 40 && analyzed_bytes == result.bytes);
        CHECK(f.requested_bytes == (size_t)40 * PAYLOAD_ANALYSIS_LIMIT);
        analyzed_reads = 0; analyzed_bytes = 0;
        f = (struct fake){.response_size = PAYLOAD_ANALYSIS_LIMIT};
        CHECK(observer_run(&backend, &result, output, analyze_read) == DJI_OK);
        CHECK(analyzed_reads == 16 && analyzed_bytes == OBSERVER_BYTE_LIMIT && result.reads == 16);
        CHECK(f.prepared == 2 && f.max_reads == 8 && f.closes == 2);
        analyzed_reads = 0; analyzed_bytes = 0;
        for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
            f = (struct fake){.read_status = errors[i], .response = token, .response_size = sizeof(token) - 1};
            CHECK(observer_run(&backend, &result, output, analyze_read) == errors[i]);
            CHECK(analyzed_reads == 0 && analyzed_bytes == 0 && result.bytes == 0 && f.closes == 1);
        }
        f = (struct fake){.read_status = OBSERVER_TIMEOUT};
        CHECK(observer_run(&backend, &result, output, analyze_read) == DJI_OK);
        CHECK(analyzed_reads == 0 && result.bytes == 0 && f.reads == 5 && f.closes == 5);
        f = (struct fake){.response_size = 0};
        CHECK(observer_run(&backend, &result, output, analyze_read) == DJI_OK);
        CHECK(analyzed_reads == 0 && f.reads == 5 && f.closes == 5);
        f = (struct fake){.response_size = PAYLOAD_ANALYSIS_LIMIT + 1};
        CHECK(observer_run(&backend, &result, output, analyze_read) == DJI_SYSTEM_ERROR);
        CHECK(analyzed_reads == 0 && f.closes == 1);
        f = (struct fake){.response_size = 1, .cancel_at_read = 1};
        CHECK(observer_run(&backend, &result, output, analyze_read) == DJI_CANCELLED);
        CHECK(analyzed_reads == 0 && result.bytes == 0 && f.closes == 1);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        char text[32768];
        size_t length = fread(text, 1, sizeof(text) - 1, output);
        text[length] = '\0';
        CHECK(strstr(text, "candidate=at-fixed-token") != NULL);
        CHECK(strstr(text, "analysis status=no-data") != NULL);
        CHECK(strstr(text, "synthetic-private-value") == NULL && strstr(text, "CME") == NULL);
        CHECK(strstr(text, "a5") == NULL);
        CHECK(fclose(output) == 0);
    }
    printf("observer unit checks=%u failures=%u (fake transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
