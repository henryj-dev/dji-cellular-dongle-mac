#include "../src/usb_observer.h"
#include "../src/observer_policy.h"
#include "../src/discovery.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures, metadata_calls, signal_calls;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

static volatile sig_atomic_t wait_flag;
static int inject_at_signal;
static struct sigaction handlers[2];
static void prior_signal(int number) { wait_flag = number; }
static int fake_sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    unsigned index = number == SIGINT ? 0 : 1;
    CHECK(number == SIGINT || number == SIGTERM);
    signal_calls++;
    if (old) *old = handlers[index];
    /* Deliver while the prior wait handler still owns this signal. */
    if (inject_at_signal && signal_calls == (unsigned)inject_at_signal)
        handlers[index].sa_handler(number);
    if (action) handlers[index] = *action;
    return 0;
}
static IOReturn fake_registry_id(io_registry_entry_t service, uint64_t *id) {
    (void)service; (void)id;
    metadata_calls++;
    return kIOReturnNoDevice;
}
#define sigaction(...) fake_sigaction(__VA_ARGS__)
#define IORegistryEntryGetRegistryEntryID fake_registry_id
#include "../src/usb_observer.c"
#undef IORegistryEntryGetRegistryEntryID
#undef sigaction

static void reset(int injection) {
    wait_flag = 0;
    metadata_calls = signal_calls = 0;
    inject_at_signal = injection;
    memset(handlers, 0, sizeof(handlers));
    handlers[0].sa_handler = handlers[1].sa_handler = prior_signal;
}

static unsigned prepares, opens, reads, closes, releases;
static int cancel_prepare, cancel_read;
static uint64_t fake_now(void *context) { (void)context; return 0; }
static int fake_prepare(void *context, unsigned interface_id) {
    (void)context; (void)interface_id;
    prepares++;
    if (cancel_prepare) wait_flag = SIGTERM;
    return DJI_OK;
}
static int fake_validate(void *context, unsigned interface_id, int opened, uint8_t *pipe) {
    (void)context; (void)interface_id; (void)opened;
    *pipe = 2;
    return DJI_OK;
}
static int fake_open(void *context) { (void)context; opens++; return DJI_OK; }
static int fake_read(void *context, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    (void)context; (void)pipe; (void)buffer; (void)timeout;
    reads++; *size = 1;
    if (cancel_read) wait_flag = SIGINT;
    return DJI_OK;
}
static int fake_close(void *context) { (void)context; closes++; return DJI_OK; }
static void fake_release(void *context) { (void)context; releases++; }

int main(void) {
    /* Arrival after discovery's final check, before observer entry. */
    reset(0);
    wait_flag = SIGINT;
    CHECK(usb_observer_observe(0, NULL, 42, NULL, 1, &wait_flag) == DJI_CANCELLED);
    CHECK(metadata_calls == 0 && signal_calls == 4);
    CHECK(handlers[0].sa_handler == prior_signal && handlers[1].sa_handler == prior_signal);

    /* Arrival during either handler replacement is also retained. */
    for (int injection = 1; injection <= 2; injection++) {
        reset(injection);
        CHECK(usb_observer_observe(0, NULL, 42, NULL, 1, &wait_flag) == DJI_CANCELLED);
        CHECK(metadata_calls == 0 && wait_flag != 0 && signal_calls == 4);
        CHECK(handlers[0].sa_handler == prior_signal && handlers[1].sa_handler == prior_signal);
    }
    /* Normal observe has no prior flag and still restores both handlers. */
    reset(0);
    wait_flag = SIGINT;
    CHECK(usb_observer_observe(0, NULL, 42, NULL, 0, NULL) == DJI_GONE);
    CHECK(metadata_calls == 1 && signal_calls == 4);
    CHECK(handlers[0].sa_handler == prior_signal && handlers[1].sa_handler == prior_signal);

    reset(0);
    interrupted = 0;
    struct usb_context context = {.prior_cancelled = &wait_flag};
    const struct observer_backend backend = {&context, fake_now, cancelled, fake_prepare,
        fake_validate, fake_open, fake_read, fake_close, fake_release};
    struct observer_result result;
    wait_flag = SIGINT;
    CHECK(observer_run(&backend, &result, NULL, NULL) == DJI_CANCELLED);
    CHECK(prepares == 0 && opens == 0 && reads == 0);
    wait_flag = 0; cancel_prepare = 1;
    CHECK(observer_run(&backend, &result, NULL, NULL) == DJI_CANCELLED);
    CHECK(prepares == 1 && opens == 0 && reads == 0 && releases == 1);
    wait_flag = 0; cancel_prepare = 0; cancel_read = 1;
    CHECK(observer_run(&backend, &result, NULL, NULL) == DJI_CANCELLED);
    CHECK(prepares == 2 && opens == 1 && reads == 1 && closes == 1 && releases == 2);
    CHECK(result.bytes == 0);
    /* Adapter guards must refuse before dereferencing a missing interface. */
    uint32_t size = 1;
    CHECK(open_interface(&context) == DJI_CANCELLED);
    CHECK(read_bulk(&context, 2, NULL, &size, 100) == DJI_CANCELLED);
    wait_flag = 0;
    CHECK(!cancelled(&context));
    on_signal(SIGTERM);
    CHECK(cancelled(&context));

    printf("observer signal checks=%u failures=%u (fake handlers/registry/transport, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
