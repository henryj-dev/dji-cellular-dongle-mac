#include "../src/reconnect.h"
#include "../src/discovery.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL line=%d condition=%s\n", __LINE__, #condition); failures++; } } while (0)

struct snapshot { int status; uint64_t id; };
struct fake {
    uint64_t now, snapshot_delay;
    const struct snapshot *sequence;
    unsigned length, snapshots, sleeps;
    unsigned cancel_at_snapshot, cancel_at_sleep;
    int cancel_initial, sleep_status, frozen_clock;
};
static uint64_t fake_now(void *context) { return ((struct fake *)context)->now; }
static int fake_cancelled(void *context) {
    struct fake *f = context;
    return f->cancel_initial || (f->cancel_at_snapshot && f->snapshots >= f->cancel_at_snapshot) ||
           (f->cancel_at_sleep && f->sleeps >= f->cancel_at_sleep);
}
static int fake_snapshot(void *context, uint64_t *id) {
    struct fake *f = context;
    unsigned index = f->snapshots < f->length ? f->snapshots : f->length - 1;
    f->snapshots++;
    f->now += f->snapshot_delay;
    *id = f->sequence[index].id;
    return f->sequence[index].status;
}
static int fake_sleep(void *context, uint32_t milliseconds) {
    struct fake *f = context;
    CHECK(milliseconds > 0 && milliseconds <= RECONNECT_POLL_MS);
    f->sleeps++;
    if (!f->frozen_clock) f->now += milliseconds;
    return f->sleep_status;
}
static int run(struct fake *f, uint64_t *id) {
    const struct reconnect_backend backend = {f, fake_now, fake_cancelled, fake_snapshot, fake_sleep};
    return reconnect_wait(&backend, id);
}

int main(void) {
    uint64_t id = 999;
    const struct snapshot reconnect[] = {{DJI_OK, 41}, {DJI_OK, 41}, {DJI_OK, 0},
                                         {DJI_OK, 41}, {DJI_OK, 42}};
    struct fake f = {.now = 5000, .sequence = reconnect, .length = 5};
    CHECK(run(&f, &id) == DJI_OK && id == 42);
    CHECK(f.snapshots == 5 && f.sleeps == 4 && f.now == 5400);

    const struct snapshot attach[] = {{DJI_OK, 0}, {DJI_OK, 0}, {DJI_OK, 42}};
    f = (struct fake){.sequence = attach, .length = 3};
    CHECK(run(&f, &id) == DJI_OK && id == 42 && f.sleeps == 2);

    const struct snapshot unchanged[] = {{DJI_OK, 41}};
    f = (struct fake){.sequence = unchanged, .length = 1};
    CHECK(run(&f, &id) == DJI_RECONNECT_TIMEOUT && id == 0);
    CHECK(f.now == RECONNECT_WAIT_MS && f.sleeps == 900 && f.snapshots == 900);
    const struct snapshot absent[] = {{DJI_OK, 0}};
    f = (struct fake){.sequence = absent, .length = 1};
    CHECK(run(&f, &id) == DJI_RECONNECT_TIMEOUT && id == 0);

    const struct snapshot duplicate_baseline[] = {{DJI_MULTIPLE, 0}};
    f = (struct fake){.sequence = duplicate_baseline, .length = 1};
    CHECK(run(&f, &id) == DJI_MULTIPLE && id == 0 && f.sleeps == 0);
    const struct snapshot duplicate_wait[] = {{DJI_OK, 41}, {DJI_MULTIPLE, 42}};
    f = (struct fake){.sequence = duplicate_wait, .length = 2};
    CHECK(run(&f, &id) == DJI_MULTIPLE && id == 0 && f.snapshots == 2);

    f = (struct fake){.sequence = attach, .length = 3, .cancel_initial = 1};
    CHECK(run(&f, &id) == DJI_CANCELLED && id == 0 && f.snapshots == 0);
    f = (struct fake){.sequence = attach, .length = 3, .cancel_at_snapshot = 1};
    CHECK(run(&f, &id) == DJI_CANCELLED && id == 0 && f.sleeps == 0);
    f = (struct fake){.sequence = attach, .length = 3, .cancel_at_sleep = 1};
    CHECK(run(&f, &id) == DJI_CANCELLED && id == 0 && f.snapshots == 1);
    f = (struct fake){.sequence = attach, .length = 3, .cancel_at_snapshot = 3};
    CHECK(run(&f, &id) == DJI_CANCELLED && id == 0);

    const struct snapshot error[] = {{DJI_OK, 0}, {DJI_UNSUPPORTED, 0}, {DJI_OK, 42}};
    f = (struct fake){.sequence = error, .length = 3};
    CHECK(run(&f, &id) == DJI_UNSUPPORTED && id == 0 && f.snapshots == 2);
    f = (struct fake){.sequence = attach, .length = 3, .sleep_status = DJI_SYSTEM_ERROR};
    CHECK(run(&f, &id) == DJI_SYSTEM_ERROR && id == 0 && f.snapshots == 1);

    /* A synchronous snapshot that returns after the deadline cannot authorize I/O. */
    const struct snapshot late[] = {{DJI_OK, 0}, {DJI_OK, 42}};
    f = (struct fake){.sequence = late, .length = 2, .snapshot_delay = 45000};
    CHECK(run(&f, &id) == DJI_RECONNECT_TIMEOUT && id == 0 && f.snapshots == 2);
    f = (struct fake){.sequence = attach, .length = 3, .snapshot_delay = 89950};
    CHECK(run(&f, &id) == DJI_RECONNECT_TIMEOUT && id == 0 && f.now == 90000 && f.snapshots == 1);
    f = (struct fake){.sequence = unchanged, .length = 1, .frozen_clock = 1};
    CHECK(run(&f, &id) == DJI_RECONNECT_TIMEOUT && id == 0 && f.sleeps == 900);
    f = (struct fake){.now = UINT64_MAX, .sequence = attach, .length = 3};
    CHECK(run(&f, &id) == DJI_SYSTEM_ERROR && id == 0 && f.snapshots == 0);

    /* Same guard used by discovery before plugin/open; a race never opens a double. */
    f = (struct fake){.sequence = attach, .length = 3};
    CHECK(run(&f, &id) == DJI_OK && id == 42);
    unsigned opens = 0;
    int status = discovery_generation_matches(id, 43);
    if (status == DJI_OK) opens++;
    CHECK(status == DJI_GONE && opens == 0);
    CHECK(discovery_generation_matches(id, 41) == DJI_GONE);
    CHECK(discovery_generation_matches(id, 0) == DJI_GONE);
    CHECK(discovery_generation_matches(0, 0) == DJI_GONE);
    status = discovery_generation_matches(id, 42);
    if (status == DJI_OK) opens++;
    CHECK(status == DJI_OK && opens == 1);

    printf("reconnect unit checks=%u failures=%u (fake registry/clock, no device access)\n", checks, failures);
    return failures ? 1 : 0;
}
