#define _DARWIN_C_SOURCE
#include "../src/ppp_usb.h"
#include "../src/ppp_session.h"
#include "../src/ppp_runner.h"
#include "../src/discovery.h"
#include "../src/at_probe.h"
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL usb line=%d: %s\n", __LINE__, #c); failures++; } } while (0)

/* SDK vtables have their actual interface182 signatures. Registry identifiers
 * below exist only in this test; no IOKit registry/discovery function is called. */
static struct {
    unsigned refs[256], device_refs, interface_refs, plugin_refs;
    int adb_child; /* 1: "adb", 2: "adbx", 3: two adb clients, 4: "a d b" */
    int adb_first; /* the foreign client is listed before ours */
    unsigned child_scans; /* full registry validations (child scans) */
    unsigned enumeration, children, opens, closes, releases, adds, removes, writes;
    int plugin_created, pending, dispatch, queued, abort_inside, dispose_reentry;
    unsigned submits, callbacks, aborts, reentries;
    IOReturn submit_error, completion_error, abort_error, source_error, write_error, open_error, close_error;
    int mismatch, busy, invalid_iterator, retain_error, addref_error, query_error, iterator_error;
    uintptr_t count;
    unsigned char input[4096], output[32768];
    size_t input_length, output_length;
    void *buffer, *refcon;
    IOAsyncCallback1 callback;
    /* multi: several outstanding IN requests, completed oldest first. */
    int multi; unsigned depth;
    void *buffers[8], *refcons[8]; IOAsyncCallback1 callbacks_queue[8]; UInt32 sizes[8];
    UInt32 requested;
    /* Expected IN timeout (0 means the adapter default 1000) and the last
     * no-data timeouts the fake SDK saw for IN and OUT. */
    UInt32 read_timeout, last_read_timeout, last_write_timeout;
} sdk;
static struct descriptor_config fixture;
static IOUSBDeviceInterface device_table, *device_vtable = &device_table;
static IOUSBInterfaceInterface182 interface_table, *interface_vtable = &interface_table;
static IOCFPlugInInterface plugin_table, *plugin_vtable = &plugin_table;
static struct ppp_usb *live;
static void assert_dispatch_retained(void);
static void assert_dispose_rejected(void);
static void assert_session_rejected(void);
static int session_reentry;
static struct ppp_runner *active_runner;
static int runner_reentry, runner_sdk_signal;
static unsigned runner_pumps, runner_steps, runner_stops;
static void assert_runner_rejected(void);
static void runner_boundary_signal(void);

static kern_return_t fake_registry_id(io_registry_entry_t entry, uint64_t *id) {
    *id = entry == 1 ? 42 : entry == 50 ? 999 : 1000 + entry;
    if (sdk.mismatch == 1 && entry == 1) (*id)++;
    if (sdk.mismatch == 2 && entry == 12 && sdk.opens) (*id)++;
    return KERN_SUCCESS;
}
static CFTypeRef fake_property(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, IOOptionBits options) {
    (void)allocator; (void)options;
    int32_t n = 0;
    /* A foreign user client's creator: only the name may reach the label. */
    if (CFEqual(key, CFSTR("IOUserClientCreator")))
        return entry == 50 ? CFRetain(CFSTR("pid 4321, Foreign App.helper-2")) :
            entry == 52 ? CFRetain(CFSTR("pid 4321")) : entry == 53 ? CFRetain(CFSTR("4321,app")) :
            entry == 54 ? CFRetain(sdk.adb_child == 2 ? CFSTR("pid 77, adbx") : sdk.adb_child == 4 ?
                                   CFSTR("pid 77, a d b") : CFSTR("pid 77, adb")) :
            entry == 55 ? CFRetain(CFSTR("pid 78, adb")) : NULL;
    if (CFEqual(key, CFSTR(kUSBInterfaceNumber))) n = (int32_t)entry - 10;
    else if (CFEqual(key, CFSTR(kUSBConfigurationValue))) n = 1;
    else if (CFEqual(key, CFSTR(kUSBAlternateSetting))) n = sdk.mismatch == 4 ? 1 : 0;
    else return NULL;
    if (sdk.mismatch == 5) return CFRetain(CFSTR("not numeric"));
    return CFNumberCreate(NULL, kCFNumberSInt32Type, &n);
}
static kern_return_t child_iterator(io_registry_entry_t entry, const io_name_t plane, io_iterator_t *iterator) {
    (void)entry; (void)plane;
    sdk.children = 0; sdk.child_scans++; *iterator = 200; sdk.refs[200]++;
    return KERN_SUCCESS;
}
static kern_return_t parent_entry(io_registry_entry_t entry, const io_name_t plane, io_registry_entry_t *parent) {
    (void)entry; (void)plane;
    *parent = sdk.mismatch == 3 ? 2 : 1; sdk.refs[*parent]++;
    return KERN_SUCCESS;
}
static io_object_t iterator_next(io_iterator_t iterator) {
    io_object_t entry = 0;
    if (iterator == 100 && sdk.enumeration < 5) entry = 10 + sdk.enumeration++;
    if (iterator == 200) {
        unsigned k = sdk.children++;
        unsigned ours = sdk.adb_first && sdk.adb_child ? 1 : 0, foreign = 1 - ours;
        if (k == ours && (sdk.plugin_created || sdk.busy)) entry = 50;
        else if (k == foreign && sdk.adb_child) entry = 54; /* a second, foreign user client */
        else if (k == 2 && sdk.adb_child == 3) entry = 55; /* and a third, also adb */
    }
    if (entry) sdk.refs[entry]++;
    return entry;
}
static boolean_t iterator_valid(io_iterator_t iterator) { (void)iterator; return !sdk.invalid_iterator; }
static boolean_t conforms(io_object_t object, const io_name_t name) {
    (void)object; (void)name; return !sdk.busy;
}
/* A foreign driver class with characters the diagnostic must drop. */
static kern_return_t object_class(io_object_t object, io_name_t name) {
    (void)object; snprintf(name, sizeof(io_name_t), "%s", "Foreign-Driver.v2 /x"); return KERN_SUCCESS;
}
static kern_return_t retain_object(io_object_t object) {
    if (sdk.retain_error) return kIOReturnError;
    sdk.refs[object]++; return KERN_SUCCESS;
}
static kern_return_t release_object(io_object_t object) {
    CHECK(object < 256 && sdk.refs[object] > 0);
    sdk.refs[object]--; return KERN_SUCCESS;
}
static ULONG device_addref(void *self) { (void)self; if (sdk.addref_error) return 0; return ++sdk.device_refs; }
static ULONG device_release(void *self) { (void)self; CHECK(sdk.device_refs > 0); return --sdk.device_refs; }
static ULONG interface_release(void *self) {
    (void)self; CHECK(!sdk.pending && !sdk.dispatch && sdk.interface_refs > 0);
    sdk.releases++; sdk.plugin_created = 0; return --sdk.interface_refs;
}
static ULONG plugin_release(void *self) { (void)self; CHECK(sdk.plugin_refs > 0); return --sdk.plugin_refs; }
static HRESULT query_interface(void *self, REFIID id, LPVOID *value) {
    (void)self; (void)id;
    if (sdk.query_error) return E_NOINTERFACE;
    sdk.interface_refs++; *value = &interface_vtable; return S_OK;
}
static kern_return_t create_plugin(io_service_t service, CFUUIDRef type, CFUUIDRef interface_id,
                                   IOCFPlugInInterface ***plugin, SInt32 *score) {
    (void)type; (void)interface_id; CHECK(service == 12);
    sdk.plugin_created = 1; sdk.plugin_refs++; *plugin = &plugin_vtable; *score = 0;
    return KERN_SUCCESS;
}
static IOReturn vendor(void *self, UInt16 *v) { (void)self; *v = 0x2ca3; return KERN_SUCCESS; }
static IOReturn product(void *self, UInt16 *v) { (void)self; *v = 0x4009; return KERN_SUCCESS; }
static IOReturn revision(void *self, UInt16 *v) { (void)self; *v = 0x0318; return KERN_SUCCESS; }
static IOReturn configuration(void *self, UInt8 *v) { (void)self; *v = 1; return KERN_SUCCESS; }
static IOReturn number(void *self, UInt8 *v) { (void)self; *v = 2; return KERN_SUCCESS; }
static IOReturn alternate(void *self, UInt8 *v) { (void)self; *v = 0; return KERN_SUCCESS; }
static IOReturn endpoints(void *self, UInt8 *v) { (void)self; *v = 3; return KERN_SUCCESS; }
static IOReturn interface_iterator(void *self, IOUSBFindInterfaceRequest *request, io_iterator_t *iterator) {
    (void)self; CHECK(request->bAlternateSetting == 0);
    if (sdk.iterator_error) return kIOReturnError;
    sdk.enumeration = 0; *iterator = 100; sdk.refs[100]++; return KERN_SUCCESS;
}
static IOReturn interface_open(void *self) { (void)self; sdk.opens++; return sdk.open_error; }
static IOReturn interface_close(void *self) {
    (void)self; CHECK(!sdk.pending && !sdk.dispatch); sdk.closes++; return sdk.close_error;
}
static IOReturn pipes(void *self, UInt8 pipe, UInt8 *direction, UInt8 *endpoint,
                      UInt8 *type, UInt16 *packet, UInt8 *interval) {
    (void)self; CHECK(pipe >= 1 && pipe <= 3);
    const struct descriptor_endpoint *ep = &fixture.endpoints[fixture.alts[2].first_endpoint + pipe - 1];
    *direction = ep->direction_in ? kUSBIn : kUSBOut; *endpoint = ep->address & 15;
    *type = ep->transfer_type; *packet = ep->max_packet; *interval = ep->interval;
    if (sdk.mismatch == 6 && pipe == 1) (*interval)++;
    if (sdk.mismatch == 7 && pipe == 3) *endpoint = 4;
    if (sdk.mismatch == 8 && pipe == 2) *packet = 64;
    return KERN_SUCCESS;
}
static IOReturn write_pipe(void *self, UInt8 pipe, void *buffer, UInt32 size, UInt32 no_data, UInt32 completion) {
    (void)self; CHECK(pipe == 3 && size && size <= 4096 && no_data == completion && no_data > 0);
    sdk.writes++; sdk.last_write_timeout = no_data;
    if (session_reentry) assert_session_rejected();
    if (runner_reentry) assert_runner_rejected();
    runner_boundary_signal();
    if (sdk.write_error) return sdk.write_error;
    CHECK(size <= sizeof(sdk.output) - sdk.output_length);
    memcpy(sdk.output + sdk.output_length, buffer, size); sdk.output_length += size;
    return KERN_SUCCESS;
}
static IOReturn async_read(void *self, UInt8 pipe, void *buffer, UInt32 size,
                           UInt32 no_data, UInt32 completion, IOAsyncCallback1 callback, void *context) {
    (void)self; CHECK(pipe == 2 && size == 4096 && (sdk.multi || !sdk.pending) && !sdk.dispatch && no_data == completion);
    CHECK(no_data == (sdk.read_timeout ? sdk.read_timeout : 1000u));
    sdk.submits++; sdk.last_read_timeout = no_data;
    if (sdk.submit_error) return sdk.submit_error;
    if (sdk.multi) {
        CHECK(sdk.depth < 8);
        sdk.buffers[sdk.depth] = buffer; sdk.refcons[sdk.depth] = context;
        sdk.callbacks_queue[sdk.depth] = callback; sdk.sizes[sdk.depth] = size; sdk.depth++;
        sdk.buffer = sdk.buffers[0]; sdk.requested = sdk.sizes[0];
        sdk.callback = sdk.callbacks_queue[0]; sdk.refcon = sdk.refcons[0];
        sdk.pending = 1; return KERN_SUCCESS;
    }
    sdk.buffer = buffer; sdk.requested = size; sdk.callback = callback; sdk.refcon = context;
    sdk.pending = 1; return KERN_SUCCESS;
}
static void dispatch_callback(void) {
    CHECK(sdk.pending && sdk.queued && !sdk.dispatch);
    sdk.dispatch = 1;
    unsigned closes = sdk.closes, releases = sdk.releases, removes = sdk.removes;
    if (!sdk.completion_error && sdk.count <= sdk.requested)
        memcpy(sdk.buffer, sdk.input, sdk.input_length);
    if (sdk.dispose_reentry) assert_dispose_rejected();
    if (session_reentry) assert_session_rejected();
    if (runner_reentry) assert_runner_rejected();
    runner_boundary_signal();
    sdk.callback(sdk.refcon, sdk.completion_error, (void *)sdk.count);
    /* Callback returned but outer SDK dispatcher still owns its invocation. */
    CHECK(sdk.closes == closes && sdk.releases == releases && sdk.removes == removes);
    assert_dispatch_retained();
    sdk.dispatch = 0; sdk.pending = 0; sdk.queued = 0; sdk.callbacks++;
    if (sdk.multi) {
        for (unsigned i = 1; i < sdk.depth; i++) {
            sdk.buffers[i - 1] = sdk.buffers[i]; sdk.refcons[i - 1] = sdk.refcons[i];
            sdk.callbacks_queue[i - 1] = sdk.callbacks_queue[i]; sdk.sizes[i - 1] = sdk.sizes[i];
        }
        sdk.depth--;
        sdk.pending = sdk.depth > 0;
        if (sdk.depth) {
            sdk.buffer = sdk.buffers[0]; sdk.requested = sdk.sizes[0];
            sdk.callback = sdk.callbacks_queue[0]; sdk.refcon = sdk.refcons[0];
        }
    }
}
static IOReturn abort_pipe(void *self, UInt8 pipe) {
    (void)self; CHECK(pipe == 2 && sdk.pending); sdk.aborts++;
    if (sdk.dispose_reentry) assert_dispose_rejected();
    if (session_reentry) assert_session_rejected();
    if (runner_reentry) assert_runner_rejected();
    runner_boundary_signal();
    if (sdk.abort_inside && sdk.queued) dispatch_callback();
    return sdk.abort_error;
}
static IOReturn async_source(void *self, CFRunLoopSourceRef *source) {
    (void)self;
    if (sdk.source_error) return sdk.source_error;
    CFRunLoopSourceContext context = {0};
    *source = CFRunLoopSourceCreate(NULL, 0, &context);
    return *source ? KERN_SUCCESS : kIOReturnNoMemory;
}
static SInt32 run_loop(CFStringRef mode, CFTimeInterval seconds, Boolean once) {
    (void)mode; CHECK(seconds == 0 && once); runner_pumps++;
    if (runner_reentry) assert_runner_rejected();
    if (sdk.pending && sdk.queued) { dispatch_callback(); if (sdk.multi) return kCFRunLoopRunHandledSource; }
    return kCFRunLoopRunTimedOut;
}
static void add_source(CFRunLoopRef loop, CFRunLoopSourceRef source, CFStringRef mode) {
    (void)loop; (void)source; (void)mode; sdk.adds++;
}
static void remove_source(CFRunLoopRef loop, CFRunLoopSourceRef source, CFStringRef mode) {
    (void)loop; (void)source; (void)mode; CHECK(!sdk.pending && !sdk.dispatch); sdk.removes++;
}

#define IORegistryEntryGetRegistryEntryID fake_registry_id
#define IORegistryEntryCreateCFProperty fake_property
#define IORegistryEntryGetChildIterator child_iterator
#define IORegistryEntryGetParentEntry parent_entry
#define IOIteratorNext iterator_next
#define IOIteratorIsValid iterator_valid
#define IOObjectConformsTo conforms
#define IOObjectGetClass object_class
#define IOObjectRetain retain_object
#define IOObjectRelease release_object
#define IOCreatePlugInInterfaceForService create_plugin
#include "../src/usb_observer.c"
#define CFRunLoopRunInMode run_loop
#define CFRunLoopAddSource add_source
#define CFRunLoopRemoveSource remove_source
/* Existing checks keep the former validate-every-transfer behaviour; the
 * cached window is exercised separately in validate_interval_test. */
#define PPP_USB_VALIDATE_INTERVAL_US 0u
/* Existing checks keep the former single outstanding read; read-ahead slots
 * are exercised separately in read_ahead_test. */
#define PPP_USB_IN_SLOTS 1u
#include "../src/ppp_usb.c"

/* Only the new implementation's fixed-start call is redirected. Production
 * policy and the real observed supervisor/USB cleanup remain unchanged. */
static const char *session_fixture_path, *session_fixture_mode;
static unsigned session_starts, session_up_starts, session_loop_retains, session_loop_releases;
static int session_bind_failure, session_alloc_failure, session_start_failure;
static int session_clock_mode;
static uint64_t session_clock_value;
static struct ppp_session *active_session;
static uint64_t session_clock(void *context) {
    if (session_clock_mode == 1) return UINT64_MAX;
    if (session_clock_mode == 2) return UINT64_MAX - 1;
    if (session_clock_mode == 3) return session_clock_value;
    return usb_now(context);
}
static int session_bind(struct ppp_usb *adapter, struct ppp_supervisor *s, struct ppp_peer *peer) {
    memset(s, 0xa5, sizeof(*s)); /* Early failures must never inspect this object. */
    live = adapter; /* Existing fake SDK dispatch assertions borrow this alias. */
    if (session_bind_failure) return -1;
    int result = ppp_usb_peer(adapter, s, peer);
    if (!result) peer->now_ms = session_clock;
    return result;
}
static int session_fixture_start(struct ppp_supervisor *s, const struct ppp_peer *peer,
                                  const struct ppp_budgets *budgets, size_t capacity) {
    session_starts++;
    char *args[] = {(char *)session_fixture_path, (char *)session_fixture_mode, NULL};
    char *env[] = {NULL};
    return ppp_supervisor_start_observed(s, peer, budgets, capacity,
        session_start_failure ? "relative-fixture" : session_fixture_path, args, env, "/");
}
/* The connection-keeping start goes to the same own fixture; only the policy
 * selection is observed, never a production pppd launch. */
static int session_fixture_start_up(struct ppp_supervisor *s, const struct ppp_peer *peer,
                                    const struct ppp_budgets *budgets, size_t capacity) {
    session_up_starts++;
    return session_fixture_start(s, peer, budgets, capacity);
}
static void *session_calloc(size_t count, size_t size) {
    return session_alloc_failure ? NULL : calloc(count, size);
}
static CFTypeRef session_retain(CFTypeRef object) {
    session_loop_retains++;
    return CFRetain(object);
}
static void session_release_loop(CFTypeRef object) {
    session_loop_releases++;
    CFRelease(object);
}
#define ppp_supervisor_start_fixed session_fixture_start
#define ppp_supervisor_start_up session_fixture_start_up
#define ppp_usb_peer session_bind
#define calloc session_calloc
#define CFRetain session_retain
#define CFRelease session_release_loop
#include "../src/ppp_session.c"
#undef CFRelease
#undef CFRetain
#undef calloc
#undef ppp_usb_peer
#undef ppp_supervisor_start_up
#undef ppp_supervisor_start_fixed

/* Runner seams are local to this include; no production injection ABI. */
static struct {
    struct sigaction actions[2], original[2];
    unsigned calls, sleeps;
    int order[16], install_signal, sleep_signal, sleep_error, real_sleep, reentry;
    uint64_t fail_calls;
    uint32_t last_wait;
    volatile sig_atomic_t *external_stop;
} runner_fake;
static void runner_assert_restore_latched(void);
static void runner_old_int(int number) { (void)number; }
static void runner_old_term(int number) { (void)number; }
static void runner_emit(int number) {
    unsigned i = number == SIGINT ? 0 : 1;
    struct sigaction *action = &runner_fake.actions[i];
    CHECK(action->sa_handler != SIG_DFL && action->sa_handler != SIG_IGN);
    if (action->sa_handler != SIG_DFL && action->sa_handler != SIG_IGN)
        action->sa_handler(number);
}
static int runner_sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    unsigned i = number == SIGINT ? 0 : 1;
    CHECK(number == SIGINT || number == SIGTERM);
    unsigned call = ++runner_fake.calls;
    if (call <= 16) runner_fake.order[call - 1] = number;
    if (runner_fake.reentry) assert_runner_rejected();
    if (call <= 64 && (runner_fake.fail_calls & (UINT64_C(1) << (call - 1)))) {
        errno = EACCES; return -1;
    }
    if (old) *old = runner_fake.actions[i];
    runner_fake.actions[i] = *action;
    if (old) {
        CHECK(action->sa_flags == 0 && sigismember(&action->sa_mask, SIGINT) == 1 &&
              sigismember(&action->sa_mask, SIGTERM) == 1);
        if (runner_fake.install_signal) runner_emit(runner_fake.install_signal);
    } else runner_assert_restore_latched();
    return 0;
}
static int runner_nanosleep(const struct timespec *wait, struct timespec *remaining) {
    CHECK(wait->tv_sec == 0 && wait->tv_nsec >= 1000000 && wait->tv_nsec <= 50000000 && !remaining);
    runner_fake.sleeps++; runner_fake.last_wait = (uint32_t)(wait->tv_nsec / 1000000);
    if (runner_fake.reentry) assert_runner_rejected();
    if (runner_fake.sleep_signal) runner_emit(runner_fake.sleep_signal);
    if (runner_fake.external_stop) *runner_fake.external_stop = 1;
    if (runner_fake.sleep_error) { errno = runner_fake.sleep_error; return -1; }
    return runner_fake.real_sleep ? nanosleep(wait, remaining) : 0;
}
static int runner_session_step(struct ppp_session *session) {
    runner_steps++; return ppp_session_step(session);
}
static int runner_session_stop(struct ppp_session *session) {
    runner_stops++; return ppp_session_stop(session);
}
#define ppp_session_step runner_session_step
#define ppp_session_stop runner_session_stop
#define sigaction(...) runner_sigaction(__VA_ARGS__)
#define nanosleep runner_nanosleep
#define calloc session_calloc
#define CFRetain session_retain
#define CFRelease session_release_loop
#include "../src/ppp_runner.c"
#undef CFRelease
#undef CFRetain
#undef calloc
#undef nanosleep
#undef sigaction
#undef ppp_session_stop
#undef ppp_session_step

static void runner_assert_restore_latched(void) {
    struct ppp_runner *runner = active_runner ? active_runner : runner_lease;
    if (runner && !runner->status.session.local_done) CHECK(runner->session->stop_requested);
}
static void runner_boundary_signal(void) {
    if (runner_sdk_signal) { int number = runner_sdk_signal; runner_sdk_signal = 0; runner_emit(number); }
}
static void assert_dispatch_retained(void) {
    CHECK(live && live->pending && live->received);
    /* The slot actually being dispatched, wherever the ring head is. */
    CHECK(((struct ppp_usb_slot *)sdk.refcon)->count == (sdk.completion_error ? 0 : sdk.count));
    if (sdk.dispose_reentry) assert_dispose_rejected();
    if (session_reentry) assert_session_rejected();
    if (runner_reentry) assert_runner_rejected();
    runner_boundary_signal();
    CHECK(usb_close(live) == -1);
    usb_release(live);
    CHECK(!sdk.closes && !sdk.releases && !sdk.removes && sdk.interface_refs == 1);
}
static void assert_dispose_rejected(void) {
    struct ppp_usb *before_handle = live;
    struct ppp_usb before; memcpy(&before, live, sizeof(before));
    struct usb_session session; memcpy(&session, live->session, sizeof(session));
    unsigned char before_sdk[sizeof(sdk)]; memcpy(before_sdk, &sdk, sizeof(sdk));
    struct ppp_usb_dispose_result result = ppp_usb_dispose_step(&live);
    CHECK(!result.released && result.error && live == before_handle);
    CHECK(!memcmp(&before, live, sizeof(before)));
    CHECK(!memcmp(&session, live->session, sizeof(session)));
    CHECK(!memcmp(before_sdk, &sdk, sizeof(sdk)));
    sdk.reentries++;
}
static void reset(void) {
    if (active_runner) { fprintf(stderr, "unreleased runner before reset\n"); exit(EXIT_FAILURE); }
    memset(&sdk, 0, sizeof(sdk)); live = NULL;
    device_table = (IOUSBDeviceInterface){.AddRef = device_addref, .Release = device_release,
        .GetDeviceVendor = vendor, .GetDeviceProduct = product, .GetDeviceReleaseNumber = revision,
        .GetConfiguration = configuration, .CreateInterfaceIterator = interface_iterator};
    interface_table = (IOUSBInterfaceInterface182){.Release = interface_release,
        .GetDeviceVendor = vendor, .GetDeviceProduct = product, .GetDeviceReleaseNumber = revision,
        .GetConfigurationValue = configuration, .GetInterfaceNumber = number,
        .GetAlternateSetting = alternate, .GetNumEndpoints = endpoints,
        .USBInterfaceOpen = interface_open, .USBInterfaceClose = interface_close,
        .GetPipeProperties = pipes, .WritePipeTO = write_pipe, .ReadPipeAsyncTO = async_read,
        .AbortPipe = abort_pipe, .CreateInterfaceAsyncEventSource = async_source};
    plugin_table = (IOCFPlugInInterface){.QueryInterface = query_interface, .Release = plugin_release};
}
static void balanced(void) {
    CHECK(!sdk.pending && !sdk.dispatch && !sdk.device_refs && !sdk.interface_refs && !sdk.plugin_refs);
    for (unsigned i = 0; i < 256; i++) CHECK(sdk.refs[i] == 0);
}
static struct ppp_peer acquire(struct ppp_supervisor *supervisor) {
    struct ppp_peer peer = {0};
    CHECK(ppp_usb_acquire(1, &device_vtable, 42, &fixture, NULL, NULL, &live) == DJI_OK);
    /* NULL IN timeout keeps the former single-timeout behavior. */
    CHECK(live && live->timeout == 1000 && live->read_timeout == 1000 && sdk.opens == 1 && sdk.adds == 1);
    CHECK(ppp_usb_peer(live, supervisor, &peer) == 0);
    return peer;
}
static void queue_result(const unsigned char *data, size_t length, IOReturn error, uintptr_t count) {
    CHECK(length <= sizeof(sdk.input));
    if (data && length) memcpy(sdk.input, data, length);
    sdk.input_length = length; sdk.count = count; sdk.completion_error = error; sdk.queued = 1;
}
static void dispose(struct ppp_peer *peer) {
    (void)peer->quiesce(peer->context);
    if (sdk.pending) queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
    struct ppp_quiet quiet = peer->poll_quiet(peer->context);
    CHECK(quiet.quiet && !sdk.pending);
    CHECK(peer->close(peer->context) == 0 && peer->close(peer->context) == 0);
    CHECK(sdk.closes == 1 && sdk.removes == 1);
    peer->release(peer->context); live = NULL;
    CHECK(sdk.releases == 1); balanced();
}
/* Children diagnostic: fixed reason, count, class reduced to [A-Za-z0-9_]. */
static void children_label_test(void) {
    char out[160];
    children_label(out, sizeof(out), "extra-child", 2, 1, "Foreign-Driver.v2 /x", NULL);
    CHECK(!strcmp(out, "children-check reason=extra-child children=2 captured=1 class=ForeignDriverv2x creator=none"));
    children_label(out, sizeof(out), "iterator-invalid", 0, 0, "", NULL);
    CHECK(!strcmp(out, "children-check reason=iterator-invalid children=0 captured=0 class=none creator=none"));
    children_label(out, sizeof(out), "non-user-client", 1, 1, "-./ ", NULL);
    CHECK(!strcmp(out, "children-check reason=non-user-client children=1 captured=1 class=none creator=none"));
    char longer[200]; memset(longer, 'A', sizeof(longer) - 1); longer[sizeof(longer) - 1] = 0;
    children_label(out, sizeof(out), "id-mismatch", 1, 0, longer, NULL);
    const char *class_text = strstr(out, "class=");
    CHECK(class_text && strcspn(class_text + 6, " ") == 63);
    children_label(out, 8, "extra-child", 2, 1, "X", NULL);
    CHECK(strlen(out) == 7);
    /* Creator names keep [A-Za-z0-9_.-] only (the space is dropped). */
    char name[32];
    creator_name(50, name);
    CHECK(!strcmp(name, "ForeignApp.helper-2"));
    char empty[32];
    creator_name(51, empty); CHECK(!strcmp(empty, ""));
    /* Any other form is dropped: a pid never reaches the label. */
    creator_name(52, empty); CHECK(!strcmp(empty, ""));
    creator_name(53, empty); CHECK(!strcmp(empty, ""));
    children_label(out, sizeof(out), "extra-child", 2, 1, "AppleUSBHostInterfaceUserClient", name);
    CHECK(!strcmp(out, "children-check reason=extra-child children=2 captured=1"
                       " class=AppleUSBHostInterfaceUserClient creator=ForeignApp.helper-2"));
}
/* adb's leftover user client is ignored only after our own was captured and
 * only in the per-write check; any other creator or the open-time check stops. */
static void adb_child_test(void) {
    reset(); sdk.plugin_created = 1;
    IOUSBInterfaceInterface182 *table = NULL;
    struct usb_context c = {.current = 12, .interface = (IOUSBInterfaceInterface182 **)&table,
        .user_client_id = 999, .opened = 1};
    sdk.adb_child = 0; CHECK(check_children(&c, 0) == DJI_OK);
    sdk.adb_child = 1; CHECK(check_children(&c, 0) == DJI_OK && c.adb_logged);
    sdk.adb_first = 1; CHECK(check_children(&c, 0) == DJI_OK); sdk.adb_first = 0;
    sdk.adb_child = 2; CHECK(check_children(&c, 0) == DJI_BUSY);
    sdk.adb_child = 3; CHECK(check_children(&c, 0) == DJI_BUSY); /* only one is tolerated */
    sdk.adb_child = 4; CHECK(check_children(&c, 0) == DJI_BUSY); /* exact name before reduction */
    sdk.adb_child = 1; CHECK(check_children(&c, 1) == DJI_BUSY);
    c.opened = 0; CHECK(check_children(&c, 0) == DJI_BUSY); c.opened = 1;
    c.user_client_id = 0; CHECK(check_children(&c, 0) == DJI_BUSY);
    c.user_client_id = 999; c.interface = NULL; CHECK(check_children(&c, 0) == DJI_BUSY);
    for (unsigned i = 0; i < 256; i++) CHECK(sdk.refs[i] == 0);
    sdk.adb_child = 0; sdk.adb_first = 0; sdk.plugin_created = 0;
}
/* A successful validation is reused for the adapter's window; zero means
 * every transfer validates again; a failed validation never caches. */
static void validate_interval_test(void) {
    reset(); struct ppp_supervisor supervisor = {0};
    struct ppp_peer peer = acquire(&supervisor);
    unsigned char data[64];
    for (unsigned pass = 0; pass < 2; pass++) {
        live->validate_interval_us = pass ? 0 : 250000; live->validated_us = 0;
        unsigned before = sdk.child_scans;
        for (unsigned i = 0; i < 5; i++) {
            CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
            queue_result((const unsigned char *)"x", 1, 0, 1);
            struct offline_io got = peer.read(peer.context, data, sizeof(data));
            CHECK(got.code == OFFLINE_IO_OK && got.count == 1);
        }
        CHECK(sdk.child_scans - before == (pass ? 5u : 1u));
    }
    /* Elapsed window: the next submit validates again. */
    live->validate_interval_us = 250000; live->validated_us = 1;
    unsigned before = sdk.child_scans;
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN && sdk.child_scans == before + 1);
    queue_result((const unsigned char *)"x", 1, 0, 1);
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_OK);
    /* Accepted window risk: a foreign client inside the window is not seen
     * (the exclusive open still guards transfers); once expired it is. */
    live->validate_interval_us = 250000; live->validated_us = 0;
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN && live->validated_us);
    queue_result((const unsigned char *)"x", 1, 0, 1);
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_OK);
    sdk.adb_child = 2;
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    queue_result((const unsigned char *)"x", 1, 0, 1);
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_OK);
    /* Expired and failing: the read fails closed and the cache is cleared. */
    live->validated_us = 1;
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_ERROR && !live->validated_us);
    sdk.adb_child = 0;
    dispose(&peer);
}
/* PPP writes share the read's cached validation and send on the proved pipe;
 * the unchecked writer refuses any other pipe or a closed session. */
static void cached_write_test(void) {
    reset(); struct ppp_supervisor supervisor = {0};
    struct ppp_peer peer = acquire(&supervisor);
    supervisor.ready = supervisor.connected = 1; supervisor.stream.ready = 1;
    supervisor.phase = PPP_SESSION; supervisor.peer.context = live;
    live->validate_interval_us = 250000; live->validated_us = 0;
    unsigned scans = sdk.child_scans, writes = sdk.writes;
    for (unsigned i = 0; i < 4; i++) {
        struct offline_io wrote = peer.write(peer.context, (const unsigned char *)"abcd", 4);
        CHECK(wrote.code == OFFLINE_IO_OK && wrote.count == 4);
    }
    CHECK(sdk.child_scans == scans + 1 && sdk.writes == writes + 4 && live->valid_out == 3);
    CHECK(usb_session_write_validated(live->session, 2, (const uint8_t *)"x", 1, 100) == DJI_SYSTEM_ERROR);
    CHECK(usb_session_write_validated(live->session, 0, (const uint8_t *)"x", 1, 100) == DJI_SYSTEM_ERROR);
    CHECK(usb_session_write_validated(live->session, 3, (const uint8_t *)"x", 4097, 100) == DJI_SYSTEM_ERROR);
    CHECK(sdk.writes == writes + 4);
    supervisor.ready = supervisor.connected = 0; supervisor.stream.ready = 0;
    dispose(&peer);
}
/* Read-ahead: four requests stay queued, data comes out in submission order,
 * and a stop drops a completion that raced it instead of failing. */
static void read_ahead_test(void) {
    reset(); sdk.multi = 1;
    struct ppp_supervisor supervisor = {0};
    struct ppp_peer peer = acquire(&supervisor);
    live->in_slots = 4; live->validate_interval_us = 250000; live->validated_us = 0;
    unsigned char data[64];
    unsigned scans = sdk.child_scans;
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    CHECK(sdk.depth == 4 && live->pending == 4 && live->used == 4 && sdk.submits == 4);
    for (unsigned char i = 0; i < 10; i++) {
        unsigned char byte = (unsigned char)('a' + i);
        queue_result(&byte, 1, 0, 1);
        struct offline_io got = peer.read(peer.context, data, sizeof(data));
        CHECK(got.code == OFFLINE_IO_OK && got.count == 1 && data[0] == byte);
        CHECK(sdk.depth == 4 && live->pending == 4 && live->used == 4);
    }
    CHECK(sdk.submits == 14 && sdk.child_scans == scans + 1 && supervisor.usb_empty_polls == 0);
    /* Nothing arrived: an empty poll, nothing new submitted. */
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN && sdk.submits == 14);
    CHECK(supervisor.usb_empty_polls == 1);
    /* Stop: one success races the abort, the rest come back aborted. */
    CHECK(peer.quiesce(peer.context) == 0 && sdk.aborts == 1);
    unsigned char late = 'z';
    queue_result(&late, 1, 0, 1);
    struct ppp_quiet quiet = peer.poll_quiet(peer.context);
    CHECK(!quiet.quiet && !quiet.error && live->in_discarded == 1 && supervisor.usb_in_discarded == 1);
    while (sdk.pending) {
        queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        quiet = peer.poll_quiet(peer.context);
    }
    CHECK(quiet.quiet && !quiet.error && !live->pending && !live->error);
    CHECK(peer.close(peer.context) == 0 && sdk.closes == 1);
    peer.release(peer.context); live = NULL;
    sdk.multi = 0; balanced();
}
static void acquisition_test(void) {
    for (int i = 1; i <= 8; i++) {
        reset(); sdk.mismatch = i;
        CHECK(ppp_usb_acquire(1, &device_vtable, 42, &fixture, NULL, NULL, &live) != DJI_OK && !live);
        balanced();
    }
    for (unsigned i = 0; i < 10; i++) {
        reset();
        if (i == 0) sdk.busy = 1;
        if (i == 1) sdk.invalid_iterator = 1;
        if (i == 2) sdk.retain_error = 1;
        if (i == 3) sdk.addref_error = 1;
        if (i == 4) sdk.query_error = 1;
        if (i == 5) sdk.iterator_error = 1;
        if (i == 6) sdk.open_error = kIOReturnExclusiveAccess;
        if (i == 7) sdk.source_error = kIOReturnUnsupported;
        uint32_t timeout = i == 8 ? 0 : 1000, read_timeout = i == 9 ? 0 : 1000;
        CHECK(ppp_usb_acquire(1, &device_vtable, 42, &fixture, &timeout, &read_timeout, &live) != DJI_OK && !live);
        balanced();
    }
    /* Separate OUT and IN timeouts reach their own SDK calls. */
    reset(); sdk.read_timeout = 31000;
    {
        uint32_t timeout = 77, read_timeout = 31000;
        struct ppp_supervisor s = {0}; struct ppp_peer peer = {0};
        CHECK(ppp_usb_acquire(1, &device_vtable, 42, &fixture, &timeout, &read_timeout, &live) == DJI_OK);
        CHECK(live && live->timeout == 77 && live->read_timeout == 31000);
        CHECK(ppp_usb_peer(live, &s, &peer) == 0);
        unsigned char data[4096];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN && sdk.last_read_timeout == 31000);
        dispose(&peer);
    }
    reset();
    struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    CHECK(live->session->context.config != &fixture);
    CHECK(peer.quiesce(peer.context) == 0 && peer.poll_quiet(peer.context).quiet && !sdk.aborts);
    dispose(&peer);
}
static void completion_test(void) {
    for (unsigned i = 0; i < 7; i++) {
        reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        unsigned char data[4096]; memset(data, 0xa5, sizeof(data));
        if (i == 6) sdk.submit_error = kIOReturnNoDevice;
        struct offline_io result = peer.read(peer.context, data, sizeof(data));
        CHECK(result.code == (i == 6 ? OFFLINE_IO_ERROR : OFFLINE_IO_AGAIN));
        CHECK(sdk.submits == 1);
        if (i != 6) {
            const unsigned char bytes[] = {0, 0xff, 0x7e, 0x7d};
            uintptr_t count = i == 0 ? sizeof(bytes) : i == 1 ? 0 : i == 2 ? 4097 : UINTPTR_MAX;
            IOReturn error = i == 3 ? kIOReturnTimeout : i == 4 ? kIOReturnNoDevice : i == 5 ? kIOReturnError : 0;
            queue_result(bytes, sizeof(bytes), error, count);
            result = peer.read(peer.context, data, sizeof(data));
            CHECK(result.code == (i == 0 ? OFFLINE_IO_OK : OFFLINE_IO_ERROR));
            CHECK(result.count == (i == 0 ? sizeof(bytes) : 0));
            if (i == 0) CHECK(!memcmp(data, bytes, sizeof(bytes)));
            else CHECK(data[0] == 0xa5 && data[4095] == 0xa5);
            CHECK(sdk.submits == 1 && sdk.callbacks == 1 && !live->pending);
        }
        /* Fixed labels, mirrored into the bound supervisor. */
        static const enum ppp_usb_error_kind kinds[] = {PPP_USB_ERROR_NONE, PPP_USB_ERROR_ZERO_LENGTH,
            PPP_USB_ERROR_OVERSIZE, PPP_USB_ERROR_TIMEOUT, PPP_USB_ERROR_NO_DEVICE, PPP_USB_ERROR_OTHER,
            PPP_USB_ERROR_NO_DEVICE};
        enum ppp_usb_error_site site = i == 0 ? PPP_USB_SITE_NONE : i == 6 ? PPP_USB_SITE_SUBMIT :
            PPP_USB_SITE_COMPLETION;
        CHECK(live->error_kind == kinds[i] && live->error_site == site);
        CHECK(s.usb_error_kind == kinds[i] && s.usb_error_site == site);
        dispose(&peer);
    }
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    unsigned char data[4096];
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    queue_result((const unsigned char *)"large", 5, 0, 5);
    CHECK(peer.read(peer.context, data, 4).code == OFFLINE_IO_ERROR && sdk.submits == 1);
    CHECK(s.usb_error_kind == PPP_USB_ERROR_OVERSIZE && s.usb_error_site == PPP_USB_SITE_COMPLETION);
    dispose(&peer);
}
/* Every completion error stays fatal and is never resubmitted; only cancellation
 * we requested (stopping) ends quietly. The first label is sticky. */
static void usb_error_label_test(void) {
    static const struct { IOReturn error; enum ppp_usb_error_kind kind; } map[] = {
        {kIOReturnTimeout, PPP_USB_ERROR_TIMEOUT}, {kIOUSBTransactionTimeout, PPP_USB_ERROR_TIMEOUT},
        {kIOReturnAborted, PPP_USB_ERROR_ABORTED}, {kIOUSBTransactionReturned, PPP_USB_ERROR_ABORTED},
        {kIOReturnNoDevice, PPP_USB_ERROR_NO_DEVICE}, {kIOReturnNotAttached, PPP_USB_ERROR_NO_DEVICE},
        {kIOReturnOffline, PPP_USB_ERROR_NO_DEVICE}, {kIOReturnNotOpen, PPP_USB_ERROR_NOT_OPEN},
        {kIOReturnExclusiveAccess, PPP_USB_ERROR_NOT_OPEN}, {kIOUSBPipeStalled, PPP_USB_ERROR_STALL},
        {kIOReturnOverrun, PPP_USB_ERROR_OVERRUN}, {kIOReturnError, PPP_USB_ERROR_OTHER},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        unsigned char data[4096];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        queue_result(NULL, 0, map[i].error, UINTPTR_MAX);
        /* Not stopping: aborted is an error too. */
        for (unsigned n = 0; n < 3; n++)
            CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_ERROR);
        CHECK(sdk.submits == 1 && sdk.callbacks == 1 && !live->pending && live->error == map[i].error);
        CHECK(live->error_kind == map[i].kind && live->error_site == PPP_USB_SITE_COMPLETION);
        CHECK(s.usb_error_kind == map[i].kind && s.usb_error_site == PPP_USB_SITE_COMPLETION);
        dispose(&peer);
    }
    /* Stopping: our own cancellation in either spelling is a normal end. */
    for (unsigned i = 0; i < 2; i++) {
        reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        unsigned char data[4096];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        CHECK(peer.quiesce(peer.context) == 0 && sdk.aborts == 1);
        queue_result(NULL, 0, i ? kIOUSBTransactionReturned : kIOReturnAborted, UINTPTR_MAX);
        struct ppp_quiet quiet = peer.poll_quiet(peer.context);
        CHECK(quiet.quiet && !quiet.error && !live->error && sdk.submits == 1);
        CHECK(live->error_kind == PPP_USB_ERROR_NONE && live->error_site == PPP_USB_SITE_NONE);
        CHECK(s.usb_error_kind == PPP_USB_ERROR_NONE && s.usb_error_site == PPP_USB_SITE_NONE);
        dispose(&peer);
    }
    /* Stopping does not excuse a timeout. */
    reset();
    {
        struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        unsigned char data[4096];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        CHECK(peer.quiesce(peer.context) == 0);
        queue_result(NULL, 0, kIOUSBTransactionTimeout, UINTPTR_MAX);
        struct ppp_quiet quiet = peer.poll_quiet(peer.context);
        CHECK(quiet.quiet && quiet.error && sdk.submits == 1);
        CHECK(s.usb_error_kind == PPP_USB_ERROR_TIMEOUT && s.usb_error_site == PPP_USB_SITE_COMPLETION);
        dispose(&peer);
    }
    /* Sticky: a failed abort keeps its label over the later completion. */
    reset();
    {
        struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        unsigned char data[4096];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        sdk.abort_error = kIOReturnNotOpen;
        CHECK(peer.quiesce(peer.context) == -1);
        queue_result(NULL, 0, kIOReturnTimeout, UINTPTR_MAX);
        struct ppp_quiet quiet = peer.poll_quiet(peer.context);
        CHECK(quiet.quiet && quiet.error && live->error == kIOReturnNotOpen);
        CHECK(live->error_kind == PPP_USB_ERROR_NOT_OPEN && live->error_site == PPP_USB_SITE_ABORT);
        CHECK(s.usb_error_kind == PPP_USB_ERROR_NOT_OPEN && s.usb_error_site == PPP_USB_SITE_ABORT);
        dispose(&peer);
    }
}
static void cancellation_test(void) {
    for (unsigned i = 0; i < 4; i++) {
        reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        unsigned char data[4096];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        sdk.abort_error = i == 1 ? kIOReturnError : 0;
        sdk.abort_inside = i == 2;
        if (i == 2) queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        if (i == 3) { queue_result((const unsigned char *)"X", 1, 0, 1); pump(live); }
        CHECK(peer.quiesce(peer.context) == (i == 1 ? -1 : 0));
        CHECK(peer.quiesce(peer.context) == (i == 1 ? -1 : 0));
        CHECK(sdk.aborts == (i == 3 ? 0u : 1u));
        if (i < 2) {
            for (unsigned n = 0; n < 8; n++) {
                struct ppp_quiet quiet = peer.poll_quiet(peer.context);
                CHECK(!quiet.quiet && quiet.error == (i == 1));
                CHECK(peer.close(peer.context) == -1);
                peer.release(peer.context);
                CHECK(!sdk.closes && !sdk.releases && !sdk.removes && sdk.pending);
            }
            char diagnostic[256];
            CHECK(ppp_usb_diagnostic(live, diagnostic, sizeof(diagnostic)) > 0 && strstr(diagnostic, "phase=wait"));
            queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        }
        CHECK(peer.poll_quiet(peer.context).quiet);
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_ERROR);
        CHECK(sdk.submits == 1);
        dispose(&peer);
    }
}
static void *wrong_owner_calls(void *context) {
    struct ppp_peer *peer = context;
    unsigned char data[16];
    memset(data, 0xa5, sizeof(data));
    struct offline_io read = peer->read(peer->context, data, sizeof(data));
    struct offline_io write = peer->write(peer->context, data, sizeof(data));
    int begin = peer->begin_connect(peer->context);
    int quiesce = peer->quiesce(peer->context);
    struct ppp_quiet quiet = peer->poll_quiet(peer->context);
    int close = peer->close(peer->context);
    peer->release(peer->context);
    int rejected = read.code == OFFLINE_IO_ERROR && !read.count &&
        write.code == OFFLINE_IO_ERROR && !write.count && begin == -1 && quiesce == -1 &&
        !quiet.quiet && quiet.error == 1 && close == -1;
    for (size_t i = 0; i < sizeof(data); i++)
        if (data[i] != 0xa5) rejected = 0;
    return rejected ? context : NULL;
}
static void wrong_owner_test(void) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    s.peer = peer; s.ready = s.connected = 1; s.phase = PPP_SESSION;
    unsigned char rx[16], tx[16];
    struct offline_backend backend = {peer.context, peer.write, peer.write,
        peer.now_ms, peer.close, peer.release};
    CHECK(offline_stream_init(&s.stream, &backend, rx, sizeof(rx), tx, sizeof(tx)) == OFFLINE_OK);
    size_t consumed = 0;
    CHECK(offline_stream_input(&s.stream, OFFLINE_RX, OFFLINE_IO_OK,
        (const unsigned char *)"CONNECT\r\n", 9, 9, &consumed) == OFFLINE_OK && consumed == 9);
    CHECK(offline_stream_ready(&s.stream) == OFFLINE_OK);
    unsigned char data[16];
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    for (unsigned phase = 0; phase < 3; phase++) {
        unsigned submits = sdk.submits, writes = sdk.writes, aborts = sdk.aborts;
        unsigned closes = sdk.closes, releases = sdk.releases, removes = sdk.removes;
        pthread_t thread;
        void *result = NULL;
        int started = pthread_create(&thread, NULL, wrong_owner_calls, &peer);
        CHECK(started == 0);
        if (!started) {
            CHECK(pthread_join(thread, &result) == 0);
            CHECK(result == &peer);
        }
        CHECK(sdk.submits == submits && sdk.writes == writes && sdk.aborts == aborts);
        CHECK(sdk.closes == closes && sdk.releases == releases && sdk.removes == removes);
        if (phase == 0) {
            CHECK(live->pending && !live->stopping);
            CHECK(peer.quiesce(peer.context) == 0);
            queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
            struct ppp_quiet quiet = peer.poll_quiet(peer.context);
            CHECK(quiet.quiet && !quiet.error);
        } else if (phase == 1) CHECK(peer.close(peer.context) == 0);
    }
    CHECK(sdk.submits == 1 && !sdk.writes && sdk.aborts == 1 && sdk.closes == 1 &&
          !sdk.releases && sdk.removes == 1);
    peer.release(peer.context); live = NULL;
    CHECK(sdk.releases == 1); balanced();
}
static void disposed(int error) {
    struct ppp_usb_dispose_result result = ppp_usb_dispose_step(&live);
    CHECK(result.released && result.error == error && !live);
    CHECK(sdk.closes == 1 && sdk.releases == 1 && sdk.removes == 1);
    CHECK(sdk.adds == 1 && !sdk.writes);
    balanced();
    result = ppp_usb_dispose_step(&live);
    CHECK(result.released && !result.error && !live);
    CHECK(sdk.closes == 1 && sdk.releases == 1 && sdk.removes == 1);
    balanced();
}
static void dispose_acquisition_test(void) {
    reset();
    struct ppp_usb_dispose_result result = ppp_usb_dispose_step(NULL);
    CHECK(!result.released && result.error);
    result = ppp_usb_dispose_step(&live);
    CHECK(result.released && !result.error && !sdk.closes && !sdk.releases && !sdk.removes);
    balanced();
    for (unsigned i = 0; i < 4; i++) {
        reset(); struct ppp_supervisor s; struct ppp_peer peer = {0};
        memset(&s, 0xa5, sizeof(s));
        CHECK(ppp_usb_acquire(1, &device_vtable, 42, &fixture, NULL, NULL, &live) == DJI_OK);
        struct ppp_usb before; memcpy(&before, live, sizeof(before));
        if (i == 1) CHECK(ppp_usb_peer(live, NULL, &peer) == -1);
        if (i == 2) CHECK(ppp_usb_peer(live, &s, NULL) == -1);
        if (i == 3) {
            CHECK(ppp_usb_peer(live, &s, &peer) == 0);
            memcpy(&before, live, sizeof(before));
            CHECK(ppp_usb_peer(live, &s, &peer) == -1);
        }
        CHECK(!memcmp(&before, live, sizeof(before)));
        CHECK(!sdk.submits && !sdk.aborts && !sdk.writes && !sdk.closes && !sdk.releases && !sdk.removes);
        CHECK(sdk.device_refs == 1 && sdk.interface_refs == 1);
        disposed(0);
        CHECK(!sdk.submits && !sdk.aborts);
    }
}
static uint64_t failed_start_clock(void *context) { (void)context; return UINT64_MAX; }
static uint64_t overflow_start_clock(void *context) { (void)context; return UINT64_MAX - 1; }
static void dispose_start_failure_test(const char *path) {
    for (unsigned pending = 0; pending < 2; pending++) for (unsigned i = 0; i < 9; i++) {
        reset(); struct ppp_supervisor s;
        memset(&s, 0xa5, sizeof(s));
        struct ppp_peer peer = acquire(&s), test_peer = peer;
        struct ppp_budgets budgets = {10000, 10000, 10000, 10000};
        char *args[] = {(char *)path, "echo", NULL}, *env[] = {NULL};
        unsigned char data[16];
        /* Synthetic state: direct test-only peer.read BEFORE failed start.
         * Production start does not submit IN in this order. */
        if (pending) CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        if (i == 1) test_peer.read = NULL;
        if (i == 2) budgets.start_ms = 0;
        if (i == 4) test_peer.now_ms = failed_start_clock;
        if (i == 5) test_peer.now_ms = overflow_start_clock;
        unsigned char before[sizeof(s)]; memcpy(before, &s, sizeof(s));
        CHECK(ppp_supervisor_start(i == 3 ? NULL : &s, &test_peer, &budgets, i == 0 ? 0 : 8,
              i == 6 ? "relative-path" : path, i == 7 ? NULL : args, env,
              i == 8 ? "relative-cwd" : "/") == -1);
        if (i < 6) CHECK(!memcmp(before, &s, sizeof(s))); /* Not initialized on early failure. */
        else CHECK(!s.child.started && !s.child.pid && s.child.input_fd == -1 &&
                   s.child.output_fd == -1 && s.child.error_fd == -1 && s.child.exec_fd == -1);
        CHECK(sdk.submits == pending && !sdk.writes && !sdk.aborts && !sdk.closes && !sdk.releases && !sdk.removes);
        CHECK(sdk.device_refs == 1 && sdk.interface_refs == 1 && !live->stopping && !live->error);
        if (pending) {
            struct ppp_usb *handle = live;
            for (unsigned n = 0; n < 3; n++) {
                struct ppp_usb_dispose_result result = ppp_usb_dispose_step(&live);
                CHECK(!result.released && !result.error && live == handle && live->stopping && live->pending);
                CHECK(sdk.aborts == 1 && !sdk.closes && !sdk.releases && !sdk.removes);
                CHECK(sdk.device_refs == 1 && sdk.interface_refs == 1);
            }
            struct ppp_peer unused = {0};
            CHECK(ppp_usb_peer(live, &s, &unused) == -1);
            CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_ERROR && sdk.submits == 1);
            queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        }
        disposed(0);
        CHECK(sdk.submits == pending && sdk.aborts == pending && sdk.callbacks == pending);
    }
}
static void dispose_completion_test(void) {
    for (unsigned i = 0; i < 10; i++) {
        reset(); struct ppp_supervisor s; memset(&s, 0xa5, sizeof(s));
        struct ppp_peer peer = acquire(&s);
        unsigned char data[16];
        CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
        sdk.abort_error = i == 1 ? kIOReturnError : 0;
        sdk.abort_inside = i == 2;
        sdk.dispose_reentry = i == 2 || i == 8;
        if (i == 2) queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        if (i == 3) {
            queue_result((const unsigned char *)"X", 1, kIOReturnSuccess, 1);
            pump(live); /* Stored success is undelivered even though already quiet. */
        }
        if (i == 7) {
            remember_error(live, kIOReturnNoDevice, PPP_USB_ERROR_NONE, PPP_USB_SITE_VALIDATE);
            sdk.abort_error = kIOReturnError;
        }
        struct ppp_usb *handle = live;
        if (i != 2 && i != 3) {
            for (unsigned n = 0; n < 4; n++) {
                struct ppp_usb_dispose_result result = ppp_usb_dispose_step(&live);
                CHECK(!result.released && result.error == (i == 1 || i == 7) && live == handle);
                CHECK(live->pending && live->stopping && !live->dispose_active);
                CHECK(sdk.aborts == 1 && !sdk.closes && !sdk.releases && !sdk.removes);
                CHECK(sdk.device_refs == 1 && sdk.interface_refs == 1 && sdk.submits == 1 && !sdk.writes);
                if (i == 7) CHECK(live->error == kIOReturnNoDevice); /* First error survives Abort. */
            }
            IOReturn error = i == 4 ? kIOReturnTimeout : kIOReturnAborted;
            uintptr_t count = UINTPTR_MAX;
            if (i == 5 || i == 6) { error = kIOReturnSuccess; count = i == 5 ? 0 : 4097; }
            if (i == 9) { error = kIOReturnSuccess; count = 1; }
            queue_result(i == 9 ? (const unsigned char *)"X" : NULL, i == 9 ? 1 : 0, error, count);
        }
        disposed(i != 0 && i != 2 && i != 8);
        CHECK(sdk.submits == 1 && sdk.callbacks == 1 && sdk.aborts == (i == 3 ? 0u : 1u));
        if (i == 2 || i == 8) CHECK(sdk.reentries == 3); /* Abort and callback entry/return. */
    }
}
static void *wrong_dispose_owner(void *context) {
    assert_dispose_rejected();
    return context;
}
static void *retained_other_loop(void *context) {
    CFRunLoopRef *loop = context;
    *loop = CFRunLoopGetCurrent(); CFRetain(*loop);
    return context;
}
static void dispose_rejection_test(void) {
    CFRunLoopRef other_loop = NULL;
    pthread_t loop_thread;
    int loop_started = pthread_create(&loop_thread, NULL, retained_other_loop, &other_loop);
    CHECK(loop_started == 0);
    if (!loop_started) CHECK(pthread_join(loop_thread, NULL) == 0);
    reset(); struct ppp_supervisor s; memset(&s, 0xa5, sizeof(s));
    struct ppp_peer peer = acquire(&s);
    unsigned char data[16];
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    for (unsigned phase = 0; phase < 3; phase++) {
        live->pumping = 1; assert_dispose_rejected(); live->pumping = 0;
        live->dispose_active = 1; assert_dispose_rejected(); live->dispose_active = 0;
        if (other_loop) {
            CFRunLoopRef owner_loop = live->loop;
            CHECK(other_loop != owner_loop);
            live->loop = other_loop; assert_dispose_rejected(); live->loop = owner_loop;
        }
        pthread_t thread; void *result = NULL;
        int started = pthread_create(&thread, NULL, wrong_dispose_owner, &peer);
        CHECK(started == 0);
        if (!started) { CHECK(pthread_join(thread, &result) == 0); CHECK(result == &peer); }
        if (phase == 0) {
            struct ppp_usb_dispose_result result = ppp_usb_dispose_step(&live);
            CHECK(!result.released && !result.error && live->pending && sdk.aborts == 1);
        } else if (phase == 1) {
            queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
            CHECK(peer.poll_quiet(peer.context).quiet);
        }
    }
    disposed(0);
    CHECK(sdk.submits == 1 && sdk.callbacks == 1 && sdk.aborts == 1);
    if (other_loop) CFRelease(other_loop);
    /* Reentry during ordinary read pumping, without a disposal guard. */
    reset(); peer = acquire(&s);
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    sdk.dispose_reentry = 1;
    queue_result((const unsigned char *)"X", 1, kIOReturnSuccess, 1);
    struct offline_io read = peer.read(peer.context, data, sizeof(data));
    CHECK(read.code == OFFLINE_IO_OK && read.count == 1 && data[0] == 'X');
    CHECK(sdk.reentries == 2 && !live->stopping && !live->error);
    disposed(0);
    CHECK(sdk.submits == 1 && sdk.callbacks == 1 && !sdk.aborts);
}
static void dispose_close_test(void) {
    for (unsigned failure = 0; failure < 2; failure++) for (unsigned closed = 0; closed < 2; closed++) {
        reset(); struct ppp_supervisor s; memset(&s, 0xa5, sizeof(s));
        struct ppp_peer peer = acquire(&s);
        sdk.close_error = failure ? kIOReturnError : 0;
        if (closed) {
            CHECK(peer.quiesce(peer.context) == 0 && peer.poll_quiet(peer.context).quiet);
            CHECK(peer.close(peer.context) == (int)failure && live->close_error == (int)failure && !live->error);
            sdk.close_error = 0; /* The first close error remains sticky. */
        }
        disposed((int)failure);
        CHECK(!sdk.submits && !sdk.aborts);
    }
}
static uint64_t wall(void) {
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void drive(struct ppp_supervisor *s) {
    CHECK(ppp_supervisor_step(s) >= 0);
    struct pollfd fd = {s->child.output_fd, POLLIN, 0}; (void)poll(&fd, 1, 1);
}
static void start_child(struct ppp_supervisor *s, struct ppp_peer *peer, const char *path, size_t capacity) {
    struct ppp_budgets budgets = {10000, 10000, 10000, 10000};
    char *args[] = {(char *)path, "echo", NULL}, *env[] = {"PPP_FIXTURE=explicit", NULL};
    struct ppp_peer manual = *peer;
    manual.begin_connect = NULL; /* Preserve the injected/manual fixture contract. */
    CHECK(ppp_supervisor_start(s, &manual, &budgets, capacity, path, args, env, "/") == 0);
    uint64_t deadline = wall() + 3000;
    while (s->pending_tx.length < 3 && !s->error && wall() < deadline) drive(s);
    CHECK(s->pending_tx.length == 3 && !sdk.submits && !sdk.writes);
    CHECK(ppp_supervisor_ready(s) == 0);
}
static void cleanup_child(struct ppp_supervisor *s) {
    uint64_t deadline = wall() + 3000;
    while (s->phase != PPP_CLOSED && wall() < deadline) drive(s);
    CHECK(s->phase == PPP_CLOSED && s->child.reaped && s->peer_quiet && s->stream.cleanup_done);
    CHECK(sdk.closes == 1 && sdk.releases == 1 && sdk.removes == 1);
    CHECK(ppp_supervisor_step(s) == 1);
    live = NULL; balanced();
}
static void asynchronous_exec_failure_test(void) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    struct ppp_budgets budgets = {10000, 10000, 10000, 10000};
    const char *path = "/dji-ppp-usb-missing-fixture";
    char *args[] = {(char *)path, NULL}, *env[] = {NULL};
    /* fork/start succeeds: ENOENT arrives asynchronously. Supervisor owns peer;
     * caller dispose is forbidden, even though exec will fail. */
    CHECK(ppp_supervisor_start(&s, &peer, &budgets, 8, path, args, env, "/") == 0);
    CHECK(s.child.started && !sdk.submits && !sdk.writes && !sdk.closes && !sdk.releases);
    cleanup_child(&s);
    CHECK(s.error == PPP_ERROR_CHILD && s.child.exec_error == ENOENT && s.child.reap_count == 1);
    CHECK(!sdk.submits && !sdk.aborts && !sdk.writes);
}
/* control is the modem's text before PPP data: plain CONNECT, or the hook echo
 * followed by a rate-suffixed CONNECT. */
static void integrated_test(const char *path, size_t capacity, size_t split, const char *control) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    unsigned char forbidden = 'Q';
    CHECK(peer.write(peer.context, &forbidden, 1).code == OFFLINE_IO_ERROR && !sdk.writes);
    start_child(&s, &peer, path, capacity);
    const size_t head = strlen(control), total = head + 3072;
    unsigned char bytes[64 + 3072]; CHECK(head <= 64); memcpy(bytes, control, head);
    for (size_t n = head; n < total; n++) bytes[n] = (unsigned char)n;
    size_t offset = 0; uint64_t deadline = wall() + 10000;
    unsigned old_submits = 0, completions = 0; int order = 1, retained = 0;
    while (sdk.output_length < total - head + 3 && !s.error && wall() < deadline) {
        if (sdk.pending && !sdk.queued && offset < total) {
            size_t length = total - offset;
            if (length > split) length = split;
            queue_result(bytes + offset, length, 0, length); offset += length; completions++;
        }
        size_t suffix = s.usb_length - s.usb_offset; old_submits = sdk.submits;
        drive(&s);
        if (suffix && sdk.submits != old_submits) order = 0;
        retained |= s.usb_length > s.usb_offset;
    }
    CHECK(!s.error && s.connected && s.stream.ready && order);
    CHECK(split <= capacity || retained);
    CHECK(sdk.output_length == total - head + 3 && !memcmp(sdk.output, "RDY", 3));
    CHECK(!memcmp(sdk.output + 3, bytes + head, total - head));
    /* Diagnostic counters: every accepted USB and child byte, never contents. */
    CHECK(s.usb_input_bytes == offset && s.child_output_bytes >= sdk.output_length);
    CHECK(s.usb_input_reads == completions && completions > 0);
    CHECK(s.stream.control_bytes == head && s.stream.echo_seen == (control[0] == 'A'));
    CHECK(s.stream.modem_result == OFFLINE_MODEM_NONE);
    /* The negotiation copy sees only post-CONNECT bytes, never the control text. */
    CHECK(s.negotiation.summary.bytes[PPP_NEGO_PEER] == total - head &&
          s.negotiation.summary.bytes[PPP_NEGO_PEER] == s.stream.rx.accepted);
    CHECK(s.negotiation.summary.bytes[PPP_NEGO_LOCAL] == s.stream.tx.accepted);
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    /* An already submitted read must still complete after peer_done. */
    deadline = wall() + 3000;
    while (s.phase != PPP_CLEANUP && s.phase != PPP_CLOSED && wall() < deadline) drive(&s);
    if (sdk.pending) queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
    cleanup_child(&s);
    CHECK(s.error == PPP_ERROR_NONE);
}
static void undelivered_completion_test(const char *path, int stored) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    start_child(&s, &peer, path, 8); drive(&s);
    CHECK(sdk.pending);
    queue_result((const unsigned char *)"CONNECT\r\n", 9, kIOReturnSuccess, 9);
    drive(&s);
    CHECK(s.connected && !s.error && !s.stream.rx.accepted);
    drive(&s); /* Leave a second IN outstanding before peer_done. */
    CHECK(sdk.pending && sdk.submits == 2);
    unsigned submits = sdk.submits, writes = sdk.writes;
    const unsigned char byte = 0x80;
    if (stored) {
        queue_result(&byte, 1, kIOReturnSuccess, 1);
        pump(live); /* Completion returned, but no peer read delivered it. */
        CHECK(live->slots[live->head].completed && live->slots[live->head].count == 1 && !live->pending && !live->error);
    }
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    uint64_t deadline = wall() + 3000;
    if (!stored) {
        while (s.phase != PPP_CLEANUP && s.phase != PPP_CLOSED && wall() < deadline) drive(&s);
        CHECK(s.phase == PPP_CLEANUP && !s.error && !s.peer_quiet && !s.stream.drain_done);
        CHECK(sdk.aborts == 1 && sdk.pending && !sdk.closes && !sdk.releases);
        /* Successful positive completion wins the race with Abort. */
        queue_result(&byte, 1, kIOReturnSuccess, 1);
    }
    while (!s.error && s.phase != PPP_CLOSED && wall() < deadline) drive(&s);
    CHECK(s.error == PPP_ERROR_USB && s.stream.recovery_unknown && !s.stream.drain_done);
    CHECK(!s.stream.rx.accepted && !s.stream.rx.delivered);
    CHECK(sdk.submits == submits && sdk.writes == writes && sdk.aborts == (stored ? 0u : 1u));
    cleanup_child(&s);
    CHECK(s.error == PPP_ERROR_USB && s.peer_quiet && !s.stream.drain_done);
}
static void supervisor_wait_test(const char *path, int abort_failure, int successful_in) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    start_child(&s, &peer, path, 8); drive(&s);
    CHECK(sdk.pending && sdk.submits == 1);
    sdk.abort_error = abort_failure ? kIOReturnError : 0;
    CHECK(ppp_supervisor_stop(&s) == 0 && !s.error && s.phase == PPP_CANCEL);
    uint64_t deadline = wall() + 3000;
    while (!s.child.reaped && wall() < deadline) drive(&s);
    CHECK(s.child.reaped && s.phase == (abort_failure ? PPP_CLEANUP : PPP_CANCEL) && !s.peer_quiet);
    CHECK(s.error == (abort_failure ? PPP_ERROR_USB : PPP_ERROR_NONE) &&
          sdk.aborts == 1 && !sdk.closes && !sdk.releases);
    for (unsigned n = 0; n < 10; n++) drive(&s);
    CHECK(sdk.aborts == 1 && sdk.pending && !sdk.removes && !s.stream.drain_done);
    const unsigned char partial[] = "CON";
    queue_result(successful_in ? partial : NULL, successful_in ? 3 : 0,
                 successful_in ? kIOReturnSuccess : kIOReturnAborted,
                 successful_in ? 3 : UINTPTR_MAX);
    cleanup_child(&s);
    CHECK(s.error == (abort_failure || successful_in ? PPP_ERROR_USB : PPP_ERROR_NONE));
    CHECK(!s.stream.drain_done && !s.stream.rx.accepted && sdk.submits == 1 && !sdk.writes);
}
static void write_failure_test(const char *path) {
    static const struct { IOReturn error; enum ppp_usb_error_kind kind; } cases[] = {
        {kIOReturnTimeout, PPP_USB_ERROR_TIMEOUT}, {kIOReturnNoDevice, PPP_USB_ERROR_NO_DEVICE},
        {kIOReturnError, PPP_USB_ERROR_OTHER}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    start_child(&s, &peer, path, 8); drive(&s);
    queue_result((const unsigned char *)"CONNECT\r\n", 9, 0, 9); drive(&s);
    CHECK(s.connected && !s.error);
    /* Keep another IN outstanding across the synchronous OUT failure. */
    unsigned char data[4096]; CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_AGAIN);
    sdk.write_error = cases[i].error;
    const unsigned char byte = 'X';
    CHECK(peer.write(peer.context, &byte, 1).code == OFFLINE_IO_ERROR);
    CHECK(sdk.last_write_timeout == 1000);
    unsigned submits = sdk.submits, writes = sdk.writes;
    CHECK(peer.read(peer.context, data, sizeof(data)).code == OFFLINE_IO_ERROR);
    CHECK(peer.write(peer.context, &byte, 1).code == OFFLINE_IO_ERROR);
    CHECK(sdk.submits == submits && sdk.writes == writes && sdk.pending);
    drive(&s); CHECK(s.error == PPP_ERROR_USB && s.phase == PPP_CLEANUP);
    queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
    cleanup_child(&s); CHECK(s.error == PPP_ERROR_USB);
    /* The adapter is freed; its label survives in the supervisor, and the
     * stopping abort did not replace it. */
    CHECK(s.usb_error_kind == cases[i].kind && s.usb_error_site == PPP_USB_SITE_WRITE);
    }
}
static void probe_pointer_test(void) {
    reset(); struct usb_session *session = NULL;
    CHECK(usb_session_acquire(1, &device_vtable, 42, &fixture, &session) == DJI_OK);
    const uint8_t copy[] = {'A', 'T', '\r'};
    CHECK(write_bulk(&session->context, 3, copy, sizeof(copy), 100) == DJI_INVALID_DESCRIPTOR && !sdk.writes);
    const uint8_t execute[] = "AT+CGDATA=\"PPP\",1\r";
    CHECK(!at_probe_is_fixed_command(execute, sizeof(execute) - 1));
    CHECK(write_bulk(&session->context, 3, execute, sizeof(execute) - 1, 100) == DJI_INVALID_DESCRIPTOR && !sdk.writes);
    uint32_t size = 0;
    const uint8_t *fixed = at_probe_command_bytes(AT_PROBE_COMMAND_AT, &size);
    CHECK(write_bulk(&session->context, 3, fixed, size, 100) == DJI_OK && sdk.writes == 1);
    CHECK(usb_session_close(session) == DJI_OK); usb_session_release(session); balanced();
}
static void connect_hook_test(void) {
    for (unsigned i = 0; i < 15; i++) {
        reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        s.peer = peer; s.phase = PPP_CONNECT; s.ready = 1;
        s.child.exec_done = 1; s.connect_attempted = 1;
        if (i == 0) s.phase = PPP_START;
        if (i == 1) s.ready = 0;
        if (i == 2) s.child.exec_done = 0;
        if (i == 3) s.child.exec_error = 1;
        if (i == 4) s.error = PPP_ERROR_CHILD;
        if (i == 5) s.connected = 1;
        if (i == 6) s.peer.context = NULL;
        if (i == 7) s.stopped = 1;
        if (i == 8) s.child.reaped = 1;
        if (i == 9) s.stdout_eof = 1;
        if (i == 10) s.connect_attempted = 0;
        if (i == 11) { CHECK(peer.quiesce(peer.context) == 0); }
        if (i == 12) live->pumping = 1;
        if (i == 13) s.child.ownership_lost = 1;
        if (i == 14) s.child.transport_abandoned = 1;
        CHECK(peer.begin_connect(peer.context) == -1 && !sdk.writes && !sdk.submits && !live->connect_attempted);
        live->pumping = 0; dispose(&peer);
    }
    for (unsigned failure = 0; failure < 2; failure++) {
        reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
        s.peer = peer; s.phase = PPP_CONNECT; s.ready = 1;
        s.child.exec_done = 1; s.connect_attempted = 1;
        sdk.write_error = failure ? kIOReturnTimeout : 0;
        CHECK(peer.begin_connect(peer.context) == (failure ? -1 : 0));
        const unsigned char command[] = "AT+CGDATA=\"PPP\",1\r";
        CHECK(sdk.writes == 1 && live->connect_attempted && !sdk.submits);
        if (!failure) CHECK(sdk.output_length == sizeof(command) - 1 && !memcmp(sdk.output, command, sizeof(command) - 1));
        /* The stream accepts exactly this hook's echo; the bytes sent must match it. */
        CHECK(sizeof(command) == sizeof(OFFLINE_CGDATA_COMMAND "\r") &&
              !memcmp(command, OFFLINE_CGDATA_COMMAND "\r", sizeof(command)));
        CHECK(peer.begin_connect(peer.context) == -1 && sdk.writes == 1);
        unsigned char byte = 0;
        CHECK(peer.write(peer.context, &byte, 1).code == OFFLINE_IO_ERROR && sdk.writes == 1);
        if (failure) CHECK(peer.read(peer.context, &byte, 1).code == OFFLINE_IO_ERROR && !sdk.submits);
        CHECK(s.usb_error_kind == (failure ? PPP_USB_ERROR_TIMEOUT : PPP_USB_ERROR_NONE));
        CHECK(s.usb_error_site == (failure ? PPP_USB_SITE_CONNECT : PPP_USB_SITE_NONE));
        dispose(&peer);
    }
    /* A preexisting callback is still pumped across a failed control request. */
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    s.peer = peer; s.phase = PPP_CONNECT; s.ready = s.child.exec_done = s.connect_attempted = 1;
    unsigned char bytes[4096];
    CHECK(peer.read(peer.context, bytes, sizeof(bytes)).code == OFFLINE_IO_AGAIN);
    sdk.write_error = kIOReturnError;
    CHECK(peer.begin_connect(peer.context) == -1 && sdk.writes == 1 && sdk.pending);
    CHECK(peer.read(peer.context, bytes, sizeof(bytes)).code == OFFLINE_IO_ERROR);
    CHECK(peer.begin_connect(peer.context) == -1 && sdk.writes == 1 && sdk.submits == 1);
    CHECK(peer.quiesce(peer.context) == -1 && sdk.aborts == 1);
    CHECK(!peer.poll_quiet(peer.context).quiet && !sdk.closes && !sdk.releases);
    queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
    CHECK(peer.poll_quiet(peer.context).quiet && sdk.callbacks == 1);
    CHECK(s.usb_error_kind == PPP_USB_ERROR_OTHER && s.usb_error_site == PPP_USB_SITE_CONNECT);
    dispose(&peer);
}
static void observed_usb_test(const char *path, int failure) {
    reset(); struct ppp_supervisor s = {0}; struct ppp_peer peer = acquire(&s);
    struct ppp_budgets budgets = {10000, 10000, 10000, 10000};
    char *args[] = {(char *)path, "binary", NULL}, *env[] = {NULL};
    CHECK(ppp_supervisor_start_observed(&s, &peer, &budgets, 3, path, args, env, "/") == 0);
    CHECK(ppp_supervisor_ready(&s) == -1);
    sdk.write_error = failure ? kIOReturnTimeout : 0;
    uint64_t deadline = wall() + 3000;
    while (!s.connect_attempted && !s.error && wall() < deadline) drive(&s);
    const unsigned char command[] = "AT+CGDATA=\"PPP\",1\r";
    CHECK(s.connect_attempted && s.output_observed && sdk.writes == 1);
    if (failure) {
        CHECK(s.error == PPP_ERROR_USB && !sdk.submits && !sdk.output_length);
        cleanup_child(&s); CHECK(sdk.writes == 1 && !sdk.submits);
        return;
    }
    CHECK(!s.error && sdk.pending && sdk.submits == 1 && sdk.output_length == sizeof(command) - 1);
    CHECK(!memcmp(sdk.output, command, sizeof(command) - 1));
    const unsigned char incoming[] = {'C','O','N','N','E','C','T','\r','\n',0x80,0x7d,0};
    queue_result(incoming, sizeof(incoming), 0, sizeof(incoming));
    const unsigned char first[] = {0, 0xff, 0x7e, 0x7d, 3, 1};
    while (sdk.output_length < sizeof(command) - 1 + sizeof(first) + 3 && !s.error && wall() < deadline) drive(&s);
    CHECK(!s.error && s.connected && s.stream.rx.delivered == 3);
    CHECK(sdk.output_length == sizeof(command) - 1 + sizeof(first) + 3);
    CHECK(!memcmp(sdk.output + sizeof(command) - 1, first, sizeof(first)));
    CHECK(!memcmp(sdk.output + sizeof(command) - 1 + sizeof(first), incoming + 9, 3));
    CHECK(peer.begin_connect(peer.context) == -1);
    CHECK(ppp_supervisor_stop(&s) == 0 && ppp_supervisor_peer_done(&s) == 0);
    deadline = wall() + 3000;
    while (s.phase != PPP_CLEANUP && s.phase != PPP_CLOSED && wall() < deadline) drive(&s);
    if (sdk.pending) queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
    cleanup_child(&s); CHECK(!s.error);
}
/* Session regressions share the real adapter and fixture, never host metadata. */
static unsigned session_metadata_calls;
static int session_metadata_mode;
static uid_t session_uid(void *context) { (void)context; return 501; }
static int session_home(void *context, uid_t uid, const char **path) {
    (void)context; CHECK(uid == 501); *path = "/fake-session-home";
    return session_metadata_mode == 3 ? -1 : 0;
}
static int session_metadata(void *context, const char *path, struct stat *st) {
    (void)context; session_metadata_calls++;
    if (session_metadata_mode == 2) return EACCES;
    if (!strcmp(path, "/usr/sbin/pppd")) { st->st_mode = S_IFREG | 0755; return 0; }
    if (!strcmp(path, "/usr") || !strcmp(path, "/usr/sbin") ||
        !strcmp(path, "/private") || !strcmp(path, "/private/etc") ||
        !strcmp(path, "/private/etc/ppp") || !strcmp(path, "/fake-session-home")) {
        st->st_mode = S_IFDIR | 0755; return 0;
    }
    if (session_metadata_mode == 1 && !strcmp(path, "/private/etc/ppp/options")) {
        st->st_mode = S_IFREG | 0644; return 0;
    }
    return ENOENT;
}
static struct ppp_metadata_backend session_metadata_backend = {NULL, session_uid, session_home, session_metadata};
static struct ppp_session_request session_request(void) {
    return (struct ppp_session_request){.service = 1, .device = &device_vtable,
        .registry_id = 42, .config = &fixture, .budgets = {10000, 10000, 10000, 10000},
        .capacity = 3, .timeout_ms = NULL};
}
static void session_reset(const char *path, const char *mode) {
    /* Never reset away an unresolved live object, even after a failed CHECK. */
    if (active_session) { fprintf(stderr, "unreleased session before reset\n"); exit(EXIT_FAILURE); }
    reset(); session_fixture_path = path; session_fixture_mode = mode;
    session_starts = session_up_starts = session_metadata_calls = session_loop_retains = session_loop_releases = 0;
    session_bind_failure = session_alloc_failure = session_start_failure = session_clock_mode = 0;
    session_metadata_mode = session_reentry = 0; session_clock_value = 100;
    /* session_request budgets: IN = 10000 + 10000 + 10000 + 1000 ms. */
    sdk.read_timeout = 31000;
}
static struct ppp_session_status session_status(void) {
    struct ppp_session_status status;
    if (ppp_session_snapshot(active_session, &status)) {
        fprintf(stderr, "session snapshot rejected during test\n"); exit(EXIT_FAILURE);
    }
    return status;
}
static void session_accept(struct ppp_session_request *r) {
    if (ppp_session_start(r, &session_metadata_backend, &active_session) || !active_session) {
        fprintf(stderr, "session not accepted\n"); exit(EXIT_FAILURE);
    }
    CHECK(session_loop_retains == 1 && !session_loop_releases);
}
static void session_drive(void) {
    int result = ppp_session_step(active_session);
    if (result < 0) { fprintf(stderr, "valid session step rejected\n"); exit(EXIT_FAILURE); }
    (void)poll(NULL, 0, 1);
}
static void session_finish(void) {
    uint64_t deadline = wall() + 3000;
    while (!session_status().local_done && wall() < deadline) {
        if (sdk.pending && !sdk.queued && live->stopping)
            queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        session_drive();
    }
    if (!session_status().local_done) {
        fprintf(stderr, "session cleanup still owns context after test bound\n"); exit(EXIT_FAILURE);
    }
    CHECK(ppp_session_step(active_session) == 1 && ppp_session_step(active_session) == 1);
    if (session_status().supervisor_valid) {
        CHECK(active_session->supervisor.child.reaped && active_session->supervisor.child.reap_count == 1 &&
              active_session->supervisor.child.term_signals <= 1 && active_session->supervisor.peer_quiet);
        CHECK(!active_session->adapter && !active_session->peer.context);
    }
    CHECK(ppp_session_release(&active_session) == 0 && !active_session);
    CHECK(ppp_session_release(&active_session) == 0 && !active_session);
    CHECK(session_loop_retains == 1 && session_loop_releases == 1);
    live = NULL; balanced();
}
static void assert_session_rejected(void) {
    struct ppp_session *handle = active_session;
    unsigned char before[sizeof(*handle)], before_sdk[sizeof(sdk)];
    memcpy(before, handle, sizeof(before)); memcpy(before_sdk, &sdk, sizeof(sdk));
    struct ppp_session_status status, original;
    memset(&status, 0xa5, sizeof(status)); memcpy(&original, &status, sizeof(status));
    CHECK(ppp_session_step(handle) == -1 && ppp_session_stop(handle) == -1);
    CHECK(ppp_session_peer_done(handle) == -1 && ppp_session_snapshot(handle, &status) == -1);
    CHECK(ppp_session_release(&active_session) == -1 && active_session == handle);
    CHECK(!memcmp(&status, &original, sizeof(status)) && !memcmp(before, handle, sizeof(before)));
    CHECK(!memcmp(before_sdk, &sdk, sizeof(sdk)) && !session_loop_releases);
}
static void session_inputs_test(const char *path) {
    for (unsigned i = 0; i < 16; i++) {
        session_reset(path, "binary");
        struct ppp_session_request r = session_request();
        struct ppp_metadata_backend metadata = session_metadata_backend;
        uint32_t zero = 0;
        if (i == 0) r.service = 0;
        if (i == 1) r.device = NULL;
        if (i == 2) r.config = NULL;
        if (i == 3) r.capacity = 0;
        if (i == 4) r.capacity = PPP_SUPERVISOR_BUFFER + 1;
        if (i == 5) r.budgets.start_ms = 0;
        if (i == 6) r.budgets.connect_ms = 0;
        if (i == 7) r.budgets.session_ms = 0;
        if (i == 8) r.budgets.drain_ms = 0;
        if (i == 9) r.timeout_ms = &zero;
        if (i == 10) metadata.metadata = NULL;
        if (i == 11) session_alloc_failure = 1;
        if (i == 13) r.registry_id = 0;
        /* Derived IN timeout beyond uint32_t, either as a sum or as one term. */
        if (i == 14) r.budgets.session_ms = UINT32_MAX;
        if (i == 15) r.budgets.connect_ms = UINT64_MAX;
        CHECK(ppp_session_start(i == 12 ? NULL : &r, &metadata, &active_session) == -1 && !active_session);
        CHECK(!session_metadata_calls && !sdk.opens && !session_starts && !session_loop_retains);
        balanced();
    }
    session_reset(path, "binary");
    struct ppp_session_request r = session_request();
    CHECK(ppp_session_start(&r, &session_metadata_backend, NULL) == -1);
    CHECK(ppp_session_step(NULL) == -1 && ppp_session_stop(NULL) == -1 && ppp_session_peer_done(NULL) == -1);
    struct ppp_session_status status;
    CHECK(ppp_session_snapshot(NULL, &status) == -1 && ppp_session_release(NULL) == -1);
    CHECK(ppp_session_release(&active_session) == 0 && !session_metadata_calls);
    session_accept(&r);
    unsigned char before[sizeof(*active_session)]; memcpy(before, active_session, sizeof(before));
    unsigned metadata_calls = session_metadata_calls, opens = sdk.opens;
    CHECK(ppp_session_start(&r, &session_metadata_backend, &active_session) == -1);
    CHECK(!memcmp(before, active_session, sizeof(before)) && session_metadata_calls == metadata_calls && sdk.opens == opens);
    CHECK(ppp_session_release(&active_session) == -1 && !memcmp(before, active_session, sizeof(before)));
    CHECK(ppp_session_stop(active_session) == 0); session_finish();
    /* The largest derived IN timeout that still fits is accepted as is. */
    session_reset(path, "binary");
    r = session_request();
    r.budgets.connect_ms = UINT32_MAX - 1002; r.budgets.session_ms = 1; r.budgets.drain_ms = 1;
    session_accept(&r);
    CHECK(live->timeout == 1000 && live->read_timeout == UINT32_MAX);
    CHECK(ppp_session_stop(active_session) == 0); session_finish();
    CHECK(!session_starts && !sdk.submits);
}
/* Launch mode selection and the explicit IN timeout of the connection-keeping mode. */
static void session_launch_mode_test(const char *path) {
    uint32_t zero = 0, explicit_in = 75000;
    for (unsigned i = 0; i < 5; i++) {
        session_reset(path, "binary");
        struct ppp_session_request r = session_request();
        if (i == 0) r.launch = (enum ppp_launch_mode)2;
        if (i == 1) r.launch = (enum ppp_launch_mode)-1;
        if (i == 2) { r.launch = PPP_LAUNCH_MODE_UP; r.read_timeout_ms = &zero; }
        /* UP without an explicit IN timeout, even with budgets whose derived sum fits. */
        if (i == 3) r.launch = PPP_LAUNCH_MODE_UP;
        if (i == 4) { r.launch = PPP_LAUNCH_MODE_UP;
            r.budgets = (struct ppp_budgets){5000, 10000, UINT64_C(30) * 24 * 60 * 60 * 1000, 10000}; }
        /* Control: the same over-wide budgets without an explicit IN timeout. */
        if (i == 4) { struct ppp_session_request once = r; once.launch = PPP_LAUNCH_MODE_ONCE;
            once.budgets.session_ms = UINT32_MAX;
            CHECK(ppp_session_start(&once, &session_metadata_backend, &active_session) == -1 && !active_session); }
        CHECK(ppp_session_start(&r, &session_metadata_backend, &active_session) == -1 && !active_session);
        CHECK(!session_metadata_calls && !sdk.opens && !session_starts && !session_loop_retains);
        balanced();
    }
    /* A 30-day session with an explicit 75 s IN timeout is accepted as is and
     * selects only the connection-keeping start. */
    session_reset(path, "binary"); sdk.read_timeout = 75000;
    struct ppp_session_request r = session_request();
    r.launch = PPP_LAUNCH_MODE_UP; r.read_timeout_ms = &explicit_in;
    r.budgets = (struct ppp_budgets){5000, 10000, UINT64_C(30) * 24 * 60 * 60 * 1000, 10000};
    session_accept(&r);
    explicit_in = 1; /* Consumed before return. */
    CHECK(live->timeout == 1000 && live->read_timeout == 75000);
    CHECK(active_session->launch == PPP_LAUNCH_MODE_UP);
    CHECK(ppp_session_step(active_session) == 0 && session_starts == 1 && session_up_starts == 1);
    CHECK(session_status().state == PPP_SESSION_RUNNING && session_status().supervisor_valid);
    CHECK(active_session->supervisor.budgets.session_ms == UINT64_C(2592000000));
    CHECK(ppp_session_stop(active_session) == 0); session_finish();
    /* A budget sum beyond uint32_t is fine when the IN timeout is explicit. */
    session_reset(path, "binary"); sdk.read_timeout = 75000; explicit_in = 75000;
    r = session_request();
    r.launch = PPP_LAUNCH_MODE_UP; r.read_timeout_ms = &explicit_in;
    r.budgets.session_ms = UINT32_MAX;
    session_accept(&r);
    CHECK(live->read_timeout == 75000);
    CHECK(ppp_session_stop(active_session) == 0); session_finish();
    CHECK(!session_starts && !session_up_starts);
    /* The zero-initialized default keeps the single-shot start and derived IN. */
    session_reset(path, "binary");
    r = session_request();
    session_accept(&r);
    CHECK(active_session->launch == PPP_LAUNCH_MODE_ONCE && live->read_timeout == 31000);
    CHECK(ppp_session_step(active_session) == 0 && session_starts == 1 && !session_up_starts);
    CHECK(ppp_session_stop(active_session) == 0); session_finish();
}
static void session_preparation_test(const char *path) {
    for (int mode = 1; mode <= 3; mode++) {
        session_reset(path, "binary"); session_metadata_mode = mode;
        struct ppp_session_request r = session_request(); session_accept(&r);
        struct ppp_session_status status = session_status();
        CHECK(status.state == PPP_SESSION_CLOSED && status.local_done && status.error == PPP_SESSION_ERROR_METADATA);
        CHECK(!status.supervisor_valid && !sdk.opens && !sdk.submits && !sdk.writes && !session_starts);
        session_finish();
    }
    for (unsigned i = 0; i < 5; i++) {
        session_reset(path, "binary"); struct ppp_session_request r = session_request();
        if (i == 0) sdk.open_error = kIOReturnExclusiveAccess;
        if (i == 1) session_bind_failure = 1;
        if (i == 2) session_start_failure = 1;
        if (i == 3) session_clock_mode = 1;
        if (i == 4) session_clock_mode = 2;
        session_accept(&r);
        unsigned char poisoned[sizeof(active_session->supervisor)];
        memcpy(poisoned, &active_session->supervisor, sizeof(poisoned));
        if (i >= 2) {
            CHECK(session_status().state == PPP_SESSION_PREPARED && !session_starts);
            CHECK(ppp_session_step(active_session) == 0 && session_starts == 1);
        }
        struct ppp_session_status status = session_status();
        CHECK(status.error == (i == 0 ? PPP_SESSION_ERROR_ACQUIRE : i == 1 ? PPP_SESSION_ERROR_BIND : PPP_SESSION_ERROR_START));
        CHECK(!status.supervisor_valid && !sdk.writes && !sdk.submits);
        if (i == 1 || i >= 3) CHECK(!memcmp(poisoned, &active_session->supervisor, sizeof(poisoned)));
        if (i) CHECK(status.state == PPP_SESSION_DISPOSING && sdk.device_refs == 1 && !sdk.closes);
        session_finish();
        CHECK(session_starts == (i >= 2 ? 1u : 0u));
        if (i) CHECK(sdk.closes == 1 && sdk.releases == 1 && sdk.removes == 1);
    }
    session_reset(path, "binary");
    struct descriptor_config copied = fixture;
    uint32_t timeout = 77;
    struct ppp_session_request r = session_request(); r.config = &copied; r.timeout_ms = &timeout;
    session_accept(&r);
    /* timeout_ms is OUT only; IN still derives from the budgets. */
    CHECK(live->timeout == 77 && live->read_timeout == 31000 && live->session->context.config != &copied);
    CHECK(active_session->budgets.start_ms == 10000 && active_session->capacity == 3);
    memset(&r, 0, sizeof(r)); memset(&copied, 0, sizeof(copied)); timeout = 0;
    CHECK(ppp_session_peer_done(active_session) == -1);
    CHECK(ppp_session_stop(active_session) == 0 && ppp_session_stop(active_session) == 0);
    session_finish(); CHECK(!session_starts && !sdk.submits && !sdk.writes);
}
static void session_until(enum ppp_phase phase) {
    uint64_t deadline = wall() + 3000;
    while (session_status().supervisor_phase != phase && !session_status().error && wall() < deadline) session_drive();
    if (session_status().supervisor_phase != phase || session_status().error) {
        CHECK(0); (void)ppp_session_stop(active_session); session_finish();
        fprintf(stderr, "session did not reach requested fixture phase\n"); exit(EXIT_FAILURE);
    }
}
static void session_start_fixture(const char *path, const char *mode) {
    session_reset(path, mode); struct ppp_session_request r = session_request(); session_accept(&r);
    CHECK(live->timeout == 1000 && live->read_timeout == 31000 && !session_starts && !sdk.submits && !sdk.writes);
    CHECK(ppp_session_step(active_session) == 0 && session_starts == 1);
    CHECK(session_status().state == PPP_SESSION_RUNNING && session_status().supervisor_valid);
    CHECK(!active_session->adapter && !active_session->peer.context && !sdk.submits && !sdk.writes);
    CHECK(!active_session->supervisor.output_observed && !active_session->supervisor.ready);
}
static void session_running_test(const char *path) {
    for (unsigned phase = 0; phase < 3; phase++) {
        session_start_fixture(path, phase == 0 ? "silent" : "binary");
        if (!phase) {
            for (unsigned n = 0; n < 3; n++) session_drive();
            CHECK(!sdk.writes && !sdk.submits && !active_session->supervisor.output_observed &&
                  !active_session->supervisor.ready);
        }
        if (phase) {
            session_until(PPP_CONNECT);
            CHECK(sdk.writes == 1 && sdk.pending && active_session->supervisor.output_observed &&
                  active_session->supervisor.child.exec_done);
        }
        if (phase == 2) {
            const unsigned char incoming[] = {'C','O','N','N','E','C','T','\r','\n',0x80,0x7d,0};
            queue_result(incoming, sizeof(incoming), 0, sizeof(incoming));
            session_until(PPP_SESSION);
            uint64_t deadline = wall() + 3000;
            while (sdk.output_length < 27 && !session_status().error && wall() < deadline) session_drive();
            const unsigned char first[] = {0, 0xff, 0x7e, 0x7d, 3, 1};
            /* Fixed request (18 bytes), first stdout and CONNECT tail, all ordered. */
            CHECK(sdk.output_length == 27 && !memcmp(sdk.output + 18, first, sizeof(first)) &&
                  !memcmp(sdk.output + 24, incoming + 9, 3));
        }
        unsigned submits = sdk.submits, writes = sdk.writes;
        CHECK(ppp_session_stop(active_session) == 0 && ppp_session_stop(active_session) == 0);
        CHECK(!active_session->supervisor.stopped && !sdk.aborts);
        CHECK(ppp_session_step(active_session) == 0 && active_session->supervisor.stopped);
        CHECK(sdk.submits == submits && sdk.writes == writes && !sdk.aborts);
        CHECK(active_session->stop_delivered && active_session->supervisor.phase == (phase == 2 ? PPP_DRAIN : PPP_CANCEL));
        uint64_t deadline_ms = active_session->supervisor.deadline_ms;
        unsigned signals = active_session->supervisor.child.term_signals;
        CHECK(ppp_session_stop(active_session) == 0 && active_session->supervisor.deadline_ms == deadline_ms);
        if (phase == 2) {
            CHECK(!active_session->supervisor.peer_done && !active_session->supervisor.stream.drain_done);
            CHECK(ppp_session_peer_done(active_session) == 0 && ppp_session_peer_done(active_session) == -1);
        } else CHECK(ppp_session_peer_done(active_session) == -1);
        session_finish();
        CHECK(session_starts == 1 && signals <= 1 && sdk.closes == 1 && sdk.releases == 1);
    }
    session_start_fixture("/dji-ppp-session-missing-fixture", "binary");
    uint64_t deadline = wall() + 3000;
    while (!session_status().local_done && wall() < deadline) session_drive();
    CHECK(session_status().error == PPP_SESSION_ERROR_SUPERVISOR &&
          session_status().supervisor_error == PPP_ERROR_CHILD &&
          active_session->supervisor.child.exec_error == ENOENT &&
          active_session->supervisor.child.reap_count == 1);
    session_finish();
    CHECK(!sdk.writes && !sdk.submits && session_starts == 1);
}
static void session_error_wait_test(const char *path) {
    for (unsigned i = 0; i < 5; i++) {
        session_start_fixture(path, "binary"); session_until(PPP_CONNECT);
        if (i == 4) {
            queue_result((const unsigned char *)"CONNECT\r\n", 9, 0, 9); session_until(PPP_SESSION);
            session_drive(); CHECK(sdk.pending);
        }
        /* SESSION consumed CONNECT's callback, then submitted a second IN.
         * Other cases still own their first IN without a completion. */
        unsigned completed_before_stop = i == 4 ? 1u : 0u;
        CHECK(sdk.callbacks == completed_before_stop && sdk.submits == completed_before_stop + 1 &&
              sdk.pending && !sdk.aborts);
        if (i == 0) sdk.abort_error = kIOReturnError;
        if (i == 1) sdk.close_error = kIOReturnError;
        CHECK(ppp_session_stop(active_session) == 0); session_drive();
        if (i == 2 || i == 4) {
            session_clock_mode = 3; session_clock_value = active_session->supervisor.deadline_ms;
        }
        if (i == 3) session_clock_mode = 1;
        for (unsigned n = 0; n < 10; n++) session_drive();
        CHECK(sdk.pending && !sdk.closes && !sdk.releases && !sdk.removes && sdk.aborts == 1);
        CHECK(session_status().state == PPP_SESSION_RUNNING && !session_status().local_done);
        CHECK(ppp_session_release(&active_session) == -1 && active_session);
        if (i == 4) CHECK(!active_session->supervisor.peer_done && !active_session->supervisor.stream.drain_done);
        if (i == 0) CHECK(session_status().supervisor_error == PPP_ERROR_USB);
        if (i == 2 || i == 4) CHECK(session_status().supervisor_error == PPP_ERROR_TIMEOUT);
        if (i == 3) CHECK(session_status().supervisor_error == PPP_ERROR_CLOCK);
        queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        uint64_t deadline = wall() + 3000;
        while (!session_status().local_done && wall() < deadline) session_drive();
        struct ppp_session_status status = session_status();
        CHECK(status.error == PPP_SESSION_ERROR_SUPERVISOR && status.supervisor_valid);
        if (i == 1) CHECK(status.cleanup_error && status.supervisor_error == PPP_ERROR_CLOSE);
        struct ppp_session_observation seen;
        CHECK(ppp_session_observe(active_session, &seen) == 0);
        /* A failed AbortPipe is labeled; the later aborted completion is not. */
        CHECK(seen.usb_error_kind == (i == 0 ? PPP_USB_ERROR_OTHER : PPP_USB_ERROR_NONE));
        CHECK(seen.usb_error_site == (i == 0 ? PPP_USB_SITE_ABORT : PPP_USB_SITE_NONE));
        session_finish();
        CHECK(sdk.callbacks == completed_before_stop + 1 && sdk.callbacks == sdk.submits &&
              sdk.aborts == 1 && sdk.closes == 1 && sdk.releases == 1);
    }
}
/* A real session: IN uses the derived timeout, OUT keeps 1000 ms, and the
 * first USB label is observable after the adapter is released. */
static void session_usb_label_test(const char *path) {
    for (unsigned i = 0; i < 3; i++) {
        session_start_fixture(path, "binary"); session_until(PPP_CONNECT);
        CHECK(sdk.last_write_timeout == 1000 && sdk.last_read_timeout == 31000 && sdk.submits == 1);
        if (i == 0) queue_result(NULL, 0, kIOReturnTimeout, UINTPTR_MAX);
        if (i == 1) {
            queue_result((const unsigned char *)"CONNECT\r\n", 9, 0, 9); session_until(PPP_SESSION);
            session_drive(); CHECK(sdk.pending && sdk.submits == 2 && sdk.last_read_timeout == 31000);
            queue_result(NULL, 0, kIOUSBTransactionTimeout, UINTPTR_MAX);
        }
        if (i == 2) {
            sdk.write_error = kIOReturnTimeout;
            queue_result((const unsigned char *)"CONNECT\r\n", 9, 0, 9);
        }
        unsigned submits = i == 1 ? 2u : 1u;
        uint64_t deadline = wall() + 3000;
        while (!session_status().local_done && wall() < deadline) {
            /* Our own cancellation, in the USB transaction spelling. */
            if (sdk.pending && !sdk.queued && live->stopping)
                queue_result(NULL, 0, kIOUSBTransactionReturned, UINTPTR_MAX);
            session_drive();
        }
        struct ppp_session_status status = session_status();
        CHECK(status.local_done && status.supervisor_valid && status.supervisor_error == PPP_ERROR_USB);
        CHECK(sdk.releases == 1 && sdk.closes == 1);
        /* No resubmission after a fatal IN completion. */
        if (i < 2) CHECK(sdk.submits == submits && sdk.callbacks == submits && !sdk.aborts);
        else CHECK(sdk.writes >= 2 && sdk.last_write_timeout == 1000);
        struct ppp_session_observation seen;
        CHECK(ppp_session_observe(active_session, &seen) == 0);
        CHECK(seen.usb_error_kind == PPP_USB_ERROR_TIMEOUT);
        CHECK(seen.usb_error_site == (i < 2 ? PPP_USB_SITE_COMPLETION : PPP_USB_SITE_WRITE));
        live = NULL; /* Released with the session cleanup. */
        session_finish();
    }
}
static void *session_wrong_owner(void *context) { assert_session_rejected(); return context; }
static void session_owner_test(const char *path) {
    CFRunLoopRef other = NULL; pthread_t thread;
    int started = pthread_create(&thread, NULL, retained_other_loop, &other); CHECK(!started);
    if (!started) CHECK(!pthread_join(thread, NULL));
    session_reset(path, "binary"); struct ppp_session_request r = session_request(); session_accept(&r);
    for (unsigned state = 0; state < 3; state++) {
        active_session->busy = 1; assert_session_rejected(); active_session->busy = 0;
        if (other) {
            CFRunLoopRef original = active_session->loop;
            active_session->loop = other; assert_session_rejected(); active_session->loop = original;
        }
        started = pthread_create(&thread, NULL, session_wrong_owner, active_session); CHECK(!started);
        if (!started) CHECK(!pthread_join(thread, NULL));
        if (state == 0) {
            CHECK(ppp_session_step(active_session) == 0);
            session_reentry = 1; session_until(PPP_CONNECT);
            /* Dispatch reentry before callback, after callback and inline Abort. */
            queue_result((const unsigned char *)"CON", 3, 0, 3); session_drive();
            session_drive(); CHECK(sdk.pending);
        } else if (state == 1) {
            sdk.abort_inside = 1; queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
            CHECK(ppp_session_stop(active_session) == 0); session_drive();
            uint64_t deadline = wall() + 3000;
            while (!session_status().local_done && wall() < deadline) session_drive();
            if (!session_status().local_done) { fprintf(stderr, "owner test cleanup stalled\n"); exit(EXIT_FAILURE); }
            session_reentry = 0;
        }
    }
    session_finish(); if (other) CFRelease(other);
}
/* Runner regressions keep the same suite/fixture and never synthesize peer_done. */
static void runner_reset(const char *path, const char *mode) {
    session_reset(path, mode);
    memset(&runner_fake, 0, sizeof(runner_fake));
    runner_fake.actions[0].sa_handler = runner_old_int;
    runner_fake.actions[1].sa_handler = runner_old_term;
    for (unsigned i = 0; i < 2; i++) {
        sigemptyset(&runner_fake.actions[i].sa_mask);
        sigaddset(&runner_fake.actions[i].sa_mask, SIGUSR1);
        runner_fake.actions[i].sa_flags = SA_RESTART;
        runner_fake.original[i] = runner_fake.actions[i];
    }
    runner_reentry = runner_sdk_signal = 0;
    runner_steps = runner_stops = runner_pumps = 0;
}
static void runner_accept(void) {
    CHECK(ppp_runner_begin(&active_session, &active_runner) == 0);
    if (!active_runner || active_session) {
        fprintf(stderr, "runner not accepted\n"); exit(EXIT_FAILURE);
    }
    CHECK(session_loop_retains == 2 && !session_loop_releases);
}
static struct ppp_runner_status runner_slice(const volatile sig_atomic_t *stop, const uint32_t *wait) {
    struct ppp_runner_status status;
    unsigned steps = runner_steps, sleeps = runner_fake.sleeps;
    int result = ppp_runner_slice(active_runner, stop, wait, &status);
    if (result < 0) { fprintf(stderr, "valid runner slice rejected\n"); exit(EXIT_FAILURE); }
    CHECK(result == status.session.local_done && runner_steps - steps <= 1 && runner_fake.sleeps - sleeps <= 1);
    return status;
}
static void runner_restored(void) {
    for (unsigned i = 0; i < 2; i++) {
        CHECK(runner_fake.actions[i].sa_handler == runner_fake.original[i].sa_handler &&
              runner_fake.actions[i].sa_flags == runner_fake.original[i].sa_flags);
        CHECK(!memcmp(&runner_fake.actions[i].sa_mask, &runner_fake.original[i].sa_mask,
                      sizeof(runner_fake.actions[i].sa_mask)));
    }
}
static void runner_close(void) {
    uint64_t deadline = wall() + 3000;
    while (!active_runner->status.session.local_done && wall() < deadline) {
        if (sdk.pending && !sdk.queued && live->stopping)
            queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        (void)runner_slice(NULL, NULL);
        (void)poll(NULL, 0, 1);
    }
    if (!active_runner->status.session.local_done) {
        fprintf(stderr, "runner cleanup still owns context after test bound\n"); exit(EXIT_FAILURE);
    }
    unsigned steps = runner_steps, sleeps = runner_fake.sleeps, pumps = runner_pumps;
    CHECK(runner_slice(NULL, NULL).session.local_done);
    CHECK(runner_steps == steps && runner_fake.sleeps == sleeps && runner_pumps == pumps);
    if (active_runner->session->supervisor_valid) {
        struct ppp_supervisor *s = &active_runner->session->supervisor;
        CHECK(s->child.reaped && s->child.reap_count == 1 && s->child.term_signals <= 1 && s->peer_quiet);
        CHECK(!s->peer_done && !s->stream.drain_done);
    }
}
static void runner_finish(void) {
    runner_close();
    struct ppp_runner_status status;
    CHECK(ppp_runner_restore(active_runner, &status) == 0 && !status.unrestored_actions);
    runner_restored();
    CHECK(ppp_runner_release(&active_runner) == 0 && !active_runner && !runner_lease);
    CHECK(ppp_runner_release(&active_runner) == 0 && session_loop_releases == 2);
    live = NULL; balanced();
}
static void assert_runner_rejected(void) {
    struct ppp_runner *handle = active_runner ? active_runner : runner_lease;
    struct ppp_session *session = handle ? handle->session : active_session;
    struct ppp_runner_status status, original;
    memset(&status, 0xa5, sizeof(status)); memcpy(&original, &status, sizeof(status));
    unsigned char before_sdk[sizeof(sdk)], before_session[sizeof(*session)];
    memcpy(before_sdk, &sdk, sizeof(sdk)); memcpy(before_session, session, sizeof(*session));
    struct ppp_runner before;
    if (handle) memcpy(&before, handle, sizeof(before));
    unsigned calls = runner_fake.calls, sleeps = runner_fake.sleeps;
    unsigned retains = session_loop_retains, releases = session_loop_releases;
    struct ppp_runner *other = NULL;
    CHECK(ppp_runner_begin(&session, &other) == -1 && !other);
    CHECK(ppp_runner_slice(handle, NULL, NULL, &status) == -1);
    CHECK(ppp_runner_restore(handle, &status) == -1);
    if (handle) {
        struct ppp_runner *owning = handle;
        CHECK(ppp_runner_release(&owning) == -1 && owning == handle);
    }
    CHECK(!memcmp(&status, &original, sizeof(status)) &&
          !memcmp(before_sdk, &sdk, sizeof(sdk)) && !memcmp(before_session, session, sizeof(*session)));
    if (handle) CHECK(!memcmp(&before, handle, sizeof(before)));
    CHECK(runner_fake.calls == calls && runner_fake.sleeps == sleeps &&
          retains == session_loop_retains && releases == session_loop_releases);
}
static void *runner_wrong_owner(void *context) { assert_runner_rejected(); return context; }
static void runner_owner_checks(void) {
    active_runner->busy = 1; assert_runner_rejected(); active_runner->busy = 0;
    active_runner->session->busy = 1; assert_runner_rejected(); active_runner->session->busy = 0;
    pthread_t thread; CFRunLoopRef other = NULL;
    int started = pthread_create(&thread, NULL, retained_other_loop, &other); CHECK(!started);
    if (!started) CHECK(!pthread_join(thread, NULL));
    if (other) {
        CFRunLoopRef original = active_runner->loop;
        active_runner->loop = other; assert_runner_rejected(); active_runner->loop = original;
        CFRelease(other);
    }
    started = pthread_create(&thread, NULL, runner_wrong_owner, NULL); CHECK(!started);
    if (!started) CHECK(!pthread_join(thread, NULL));
}
static void runner_inputs_test(const char *path) {
    runner_reset(path, "binary");
    struct ppp_session_request request = session_request(); session_accept(&request);
    struct ppp_session *saved = active_session;
    CHECK(ppp_runner_begin(NULL, &active_runner) == -1 && ppp_runner_begin(&active_session, NULL) == -1);
    struct ppp_session *empty = NULL;
    CHECK(ppp_runner_begin(&empty, &active_runner) == -1 && ppp_runner_release(NULL) == -1);
    session_alloc_failure = 1;
    CHECK(ppp_runner_begin(&active_session, &active_runner) == -1 && active_session == saved && !active_runner);
    session_alloc_failure = 0;
    active_session->busy = 1; assert_runner_rejected(); active_session->busy = 0;
    pthread_t thread; int started = pthread_create(&thread, NULL, runner_wrong_owner, NULL); CHECK(!started);
    if (!started) CHECK(!pthread_join(thread, NULL));
    CFRunLoopRef other = NULL;
    started = pthread_create(&thread, NULL, retained_other_loop, &other); CHECK(!started);
    if (!started) CHECK(!pthread_join(thread, NULL));
    if (other) {
        CFRunLoopRef original = active_session->loop;
        active_session->loop = other; assert_runner_rejected(); active_session->loop = original;
    }
    CHECK(!runner_fake.calls && !session_starts && !sdk.writes && session_loop_retains == 1);
    runner_fake.reentry = 1; runner_accept(); runner_fake.reentry = 0;
    struct ppp_session *borrowed = active_runner->session;
    unsigned char before_runner[sizeof(*active_runner)], before_session[sizeof(*borrowed)];
    memcpy(before_runner, active_runner, sizeof(before_runner)); memcpy(before_session, borrowed, sizeof(before_session));
    CHECK(ppp_runner_begin(&borrowed, &active_runner) == -1 && borrowed == active_runner->session &&
          !memcmp(before_runner, active_runner, sizeof(before_runner)) && !memcmp(before_session, borrowed, sizeof(before_session)));
    struct ppp_runner_status status, original;
    memset(&status, 0xa5, sizeof(status)); memcpy(&original, &status, sizeof(status));
    for (unsigned i = 0; i < 2; i++) {
        uint32_t invalid = i ? 51 : 0;
        CHECK(ppp_runner_slice(active_runner, NULL, &invalid, &status) == -1 && !memcmp(&status, &original, sizeof(status)));
    }
    CHECK(ppp_runner_slice(active_runner, NULL, NULL, NULL) == -1 && ppp_runner_restore(active_runner, NULL) == -1);
    CHECK(ppp_runner_release(&active_runner) == -1 && !session_starts && !runner_fake.sleeps);
    active_runner->busy = 1; assert_runner_rejected(); active_runner->busy = 0;
    if (other) {
        CFRunLoopRef original_loop = active_runner->loop;
        active_runner->loop = other; assert_runner_rejected(); active_runner->loop = original_loop;
    }
    started = pthread_create(&thread, NULL, runner_wrong_owner, NULL); CHECK(!started);
    if (!started) CHECK(!pthread_join(thread, NULL));
    /* A different valid session cannot take the lease, even after restoration. */
    session_metadata_mode = 1; struct ppp_session *second = NULL;
    CHECK(ppp_session_start(&request, &session_metadata_backend, &second) == 0 && second);
    struct ppp_runner *second_runner = NULL;
    CHECK(ppp_runner_begin(&second, &second_runner) == -1 && second && !second_runner);
    CHECK(ppp_runner_restore(active_runner, &status) == 0 && status.session.stop_requested);
    CHECK(ppp_runner_begin(&second, &second_runner) == -1 && second && !second_runner);
    runner_close(); CHECK(!session_starts);
    CHECK(ppp_runner_release(&active_runner) == 0 && !active_runner);
    runner_signal = SIGTERM; /* A preceding lease's signal must not leak into CLOSED input. */
    unsigned calls = runner_fake.calls, sleeps = runner_fake.sleeps;
    CHECK(ppp_runner_begin(&second, &active_runner) == 0 && !second && active_runner);
    status = runner_slice(NULL, NULL);
    CHECK(status.session.local_done && !status.error && !status.signal_number && !status.installed_actions &&
          !status.unrestored_actions && runner_fake.calls == calls && runner_fake.sleeps == sleeps);
    CHECK(ppp_runner_release(&active_runner) == 0 && !active_runner && session_loop_retains == session_loop_releases);
    runner_restored(); live = NULL; balanced(); if (other) CFRelease(other);
}
static void runner_install_restore_test(const char *path) {
    for (unsigned mode = 0; mode < 3; mode++) {
        runner_reset(path, "binary"); struct ppp_session_request request = session_request(); session_accept(&request);
        runner_fake.fail_calls = mode == 0 ? 1 : mode == 1 ? 2 : 6;
        runner_accept();
        struct ppp_runner_status status = runner_slice(NULL, NULL);
        CHECK(status.error == PPP_RUNNER_ERROR_INSTALL && status.install_errno[mode ? 1 : 0] == EACCES &&
              status.session.stop_requested && !session_starts && !sdk.submits && !sdk.writes);
        CHECK(status.unrestored_actions == (mode == 2 ? PPP_RUNNER_SIGINT : 0u));
        if (mode == 2) {
            CHECK(status.rollback_errno[0] == EACCES && runner_fake.calls == 3);
            CHECK(ppp_runner_release(&active_runner) == -1);
            CHECK(ppp_runner_restore(active_runner, &status) == 0 && status.rollback_errno[0] == EACCES &&
                  status.install_errno[1] == EACCES && status.error == PPP_RUNNER_ERROR_INSTALL);
            CHECK(runner_fake.order[3] == SIGINT);
        }
        runner_fake.fail_calls = 0; runner_finish();
    }
    for (unsigned failure = 0; failure < 3; failure++) {
        runner_reset(path, "binary"); struct ppp_session_request request = session_request(); session_accept(&request);
        runner_accept();
        /* Restore calls 3=TERM, 4=INT; both are attempted despite failure. */
        runner_fake.fail_calls = failure == 0 ? 4 : failure == 1 ? 8 : 12;
        struct ppp_runner_status status;
        runner_fake.reentry = 1;
        CHECK(ppp_runner_restore(active_runner, &status) == 1);
        runner_fake.reentry = 0;
        unsigned remaining = failure == 0 ? PPP_RUNNER_SIGTERM : failure == 1 ? PPP_RUNNER_SIGINT : 3;
        CHECK(status.error == PPP_RUNNER_ERROR_RESTORE && status.unrestored_actions == remaining &&
              status.session.stop_requested && runner_fake.calls == 4 &&
              runner_fake.order[2] == SIGTERM && runner_fake.order[3] == SIGINT);
        runner_close(); runner_owner_checks(); CHECK(ppp_runner_release(&active_runner) == -1);
        unsigned calls = runner_fake.calls;
        CHECK(ppp_runner_restore(active_runner, &status) == 0 && !status.unrestored_actions &&
              status.error == PPP_RUNNER_ERROR_RESTORE && runner_fake.calls == calls + (failure == 2 ? 2 : 1));
        CHECK(status.restore_errno[0] == (failure ? EACCES : 0) && status.restore_errno[1] == (failure == 1 ? 0 : EACCES));
        runner_finish();
    }
}
static void runner_wait_signal_test(const char *path) {
    for (unsigned mode = 0; mode < 6; mode++) {
        runner_reset(path, "silent"); struct ppp_session_request request = session_request(); session_accept(&request);
        if (mode == 0) runner_fake.install_signal = SIGINT;
        runner_accept();
        volatile sig_atomic_t stop = mode == 1;
        if (mode == 2) runner_emit(SIGTERM);
        struct ppp_runner_status status;
        if (mode <= 2) {
            unsigned char before[sizeof(*active_runner->session)], before_sdk[sizeof(sdk)];
            memcpy(before, active_runner->session, sizeof(before)); memcpy(before_sdk, &sdk, sizeof(sdk));
            unsigned retains = session_loop_retains, releases = session_loop_releases, calls = runner_fake.calls;
            unsigned sleeps = runner_fake.sleeps, steps = runner_steps;
            CHECK(!memcmp(before, active_runner->session, sizeof(before)) && !memcmp(before_sdk, &sdk, sizeof(sdk)) && !runner_stops);
            CHECK(retains == session_loop_retains && releases == session_loop_releases && calls == runner_fake.calls &&
                  sleeps == runner_fake.sleeps && steps == runner_steps);
            status = runner_slice(&stop, NULL);
            CHECK(status.session.local_done && status.session.stop_requested && !session_starts && !runner_fake.sleeps);
            CHECK(status.signal_number == (mode == 0 ? SIGINT : mode == 2 ? SIGTERM : 0));
        } else {
            status = runner_slice(NULL, NULL);
            CHECK(session_starts == 1 && runner_steps == 1 && runner_fake.sleeps == 1 && runner_fake.last_wait == 10 && !runner_pumps);
            struct ppp_session_observation copy;
            unsigned pumps_before = runner_pumps, steps_before = runner_steps;
            CHECK(ppp_runner_observe(active_runner, &status, &copy) == 0);
            CHECK(copy.started && copy.pid > 0 && !copy.reaped && !copy.ownership_lost);
            CHECK(!copy.exited && !copy.signaled && copy.stream_state == OFFLINE_CONTROL_WAIT);
            {   /* Diagnostic fields are copied from the owned supervisor, values only. */
                struct ppp_supervisor *sv = &active_runner->session->supervisor, saved = *sv;
                sv->usb_input_bytes = 7; sv->usb_input_reads = 2; sv->child_output_bytes = 5; sv->stream.control_bytes = 3;
                sv->stream.echo_seen = 1; sv->stream.modem_result = OFFLINE_MODEM_CME;
                sv->stream.modem_code = 30; sv->stream.modem_code_valid = 1;
                sv->stream.error = OFFLINE_ERROR_RESULT;
                struct ppp_session_observation seeded;
                CHECK(ppp_runner_observe(active_runner, &status, &seeded) == 0);
                CHECK(seeded.usb_input_bytes == 7 && seeded.usb_input_reads == 2 && seeded.child_output_bytes == 5 &&
                      seeded.control_bytes == 3 && seeded.echo_seen && seeded.modem_result == OFFLINE_MODEM_CME &&
                      seeded.modem_code == 30 && seeded.modem_code_valid &&
                      seeded.stream_error == OFFLINE_ERROR_RESULT && seeded.pid == copy.pid);
                *sv = saved;
            }
            CHECK(runner_pumps == pumps_before && runner_steps == steps_before);
            pid_t captured_pid = copy.pid;
            copy.pid = 0; copy.reaped = 1;
            CHECK(ppp_runner_observe(active_runner, &status, &copy) == 0 &&
                  copy.pid == captured_pid && !copy.reaped);
            struct ppp_session_observation unchanged = copy;
            active_runner->busy = 1;
            CHECK(ppp_runner_observe(active_runner, &status, &copy) == -1 &&
                  !memcmp(&copy, &unchanged, sizeof(copy)));
            active_runner->busy = 0;
            runner_fake.reentry = 1;
            uint32_t wait = mode == 3 ? 1 : 50;
            if (mode == 3) { runner_fake.sleep_signal = SIGINT; runner_fake.sleep_error = EINTR; }
            if (mode == 4) runner_fake.external_stop = &stop;
            if (mode == 5) runner_fake.sleep_error = EIO;
            status = runner_slice(mode == 4 ? &stop : NULL, &wait);
            CHECK(runner_fake.sleeps == 2 && runner_fake.last_wait == wait && status.session.stop_requested && runner_stops == 1);
            CHECK(status.wait_errno == (mode == 5 ? EIO : 0) && status.error == (mode == 5 ? PPP_RUNNER_ERROR_WAIT : PPP_RUNNER_ERROR_NONE));
            runner_fake.reentry = 0; runner_fake.sleep_error = 0; runner_fake.sleep_signal = 0; runner_fake.external_stop = NULL;
            (void)runner_slice(&stop, NULL);
            uint64_t deadline = active_runner->session->supervisor.deadline_ms;
            unsigned terms = active_runner->session->supervisor.child.term_signals;
            runner_emit(SIGTERM); stop = 1; (void)runner_slice(&stop, NULL);
            CHECK(runner_stops == 1 && active_runner->session->supervisor.deadline_ms == deadline &&
                  active_runner->session->supervisor.child.term_signals == terms);
            if (mode == 5) CHECK(active_runner->status.wait_errno == EIO && active_runner->status.error == PPP_RUNNER_ERROR_WAIT);
        }
        runner_finish(); CHECK(!sdk.writes && !sdk.submits);
    }
    /* Real short cadence with the own fixture; not a wall-clock upper bound. */
    runner_reset(path, "silent"); struct ppp_session_request request = session_request(); session_accept(&request); runner_accept();
    runner_fake.real_sleep = 1; uint32_t wait = 1;
    (void)runner_slice(NULL, &wait); (void)runner_slice(NULL, &wait);
    CHECK(runner_fake.sleeps == 2 && runner_fake.last_wait == 1);
    struct ppp_runner_status status; CHECK(ppp_runner_restore(active_runner, &status) == 0);
    runner_finish();
}
static void runner_until(enum ppp_phase phase) {
    uint64_t deadline = wall() + 3000;
    while (active_runner->status.session.supervisor_phase != phase && !active_runner->status.session.error && wall() < deadline) {
        (void)runner_slice(NULL, NULL); (void)poll(NULL, 0, 1);
    }
    if (active_runner->status.session.supervisor_phase != phase || active_runner->status.session.error) {
        fprintf(stderr, "runner did not reach requested fixture phase\n"); exit(EXIT_FAILURE);
    }
}
static void runner_running_wait_test(const char *path) {
    for (unsigned mode = 0; mode < 6; mode++) {
        runner_reset(path, "binary"); struct ppp_session_request request = session_request(); session_accept(&request); runner_accept();
        runner_reentry = 1; (void)runner_slice(NULL, NULL); runner_until(PPP_CONNECT);
        CHECK(session_starts == 1 && sdk.writes == 1 && sdk.pending);
        if (mode == 4) {
            queue_result((const unsigned char *)"CONNECT\r\n", 9, 0, 9); runner_until(PPP_SESSION);
            (void)runner_slice(NULL, NULL); CHECK(sdk.pending);
        }
        if (mode == 5) {
            /* Signal in SDK dispatch is only observed after this synchronous step. */
            runner_sdk_signal = SIGINT;
            queue_result((const unsigned char *)"CONNECT\r\n", 9, 0, 9);
            struct ppp_runner_status status = runner_slice(NULL, NULL);
            CHECK(status.signal_number == SIGINT && status.session.stop_requested && !active_runner->session->supervisor.stopped);
            (void)runner_slice(NULL, NULL);
        } else {
            if (mode == 0) sdk.abort_error = kIOReturnError;
            if (mode == 1) sdk.close_error = kIOReturnError;
            runner_emit(SIGTERM); (void)runner_slice(NULL, NULL);
        }
        struct ppp_session *session = active_runner->session;
        CHECK(session->stop_delivered && session->supervisor.stopped && !session->supervisor.peer_done);
        if (mode == 2 || mode == 4) {
            session_clock_mode = 3; session_clock_value = session->supervisor.deadline_ms;
        }
        if (mode == 3) session_clock_mode = 1;
        if (mode == 5) {
            (void)runner_slice(NULL, NULL); CHECK(sdk.pending);
            session_clock_mode = 3; session_clock_value = session->supervisor.deadline_ms;
        }
        struct ppp_runner_status status;
        CHECK(ppp_runner_restore(active_runner, &status) == 0 && status.session.stop_requested);
        unsigned calls = runner_fake.calls;
        volatile sig_atomic_t stop = 1;
        uint64_t stop_deadline = session->supervisor.deadline_ms;
        unsigned term_signals = session->supervisor.child.term_signals;
        for (unsigned n = 0; n < 10; n++) {
            (void)runner_slice(&stop, NULL); (void)poll(NULL, 0, 1);
        }
        CHECK(session->supervisor.deadline_ms == stop_deadline && session->supervisor.child.term_signals == term_signals);
        CHECK(sdk.pending && !sdk.closes && !sdk.releases && !sdk.removes && sdk.aborts == 1 &&
              !active_runner->status.session.local_done && sdk.interface_refs == 1 && sdk.device_refs == 1);
        runner_owner_checks();
        CHECK(ppp_runner_release(&active_runner) == -1 && active_runner && runner_lease == active_runner && runner_fake.calls == calls);
        if (mode == 0) CHECK(session->supervisor.error == PPP_ERROR_USB);
        if (mode == 2 || mode == 4) CHECK(session->supervisor.error == PPP_ERROR_TIMEOUT);
        if (mode == 3) CHECK(session->supervisor.error == PPP_ERROR_CLOCK);
        CHECK(runner_stops == 1 && !session->supervisor.peer_done && !session->supervisor.stream.drain_done);
        /* The delayed completion still exercises runner guards before and after callback return. */
        queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
        runner_close(); runner_reentry = 0;
        if (mode == 1) CHECK(active_runner->status.session.cleanup_error && active_runner->status.session.supervisor_error == PPP_ERROR_CLOSE);
        runner_finish();
        CHECK(sdk.callbacks == sdk.submits && sdk.closes == 1 && sdk.releases == 1 && sdk.removes == 1);
    }
    /* Stop delivered inside the fixed OUT seam cannot cancel that existing OUT. */
    runner_reset(path, "binary"); struct ppp_session_request request = session_request(); session_accept(&request); runner_accept();
    (void)runner_slice(NULL, NULL); runner_sdk_signal = SIGTERM; runner_until(PPP_CONNECT);
    CHECK(sdk.writes == 1 && active_runner->status.signal_number == SIGTERM && active_runner->status.session.stop_requested);
    (void)runner_slice(NULL, NULL); runner_finish(); CHECK(sdk.writes == 1);
    /* Inline cancellation callback, before/after outer dispatch return. */
    runner_reset(path, "binary"); request = session_request(); session_accept(&request); runner_accept();
    (void)runner_slice(NULL, NULL); runner_until(PPP_CONNECT); runner_reentry = 1;
    sdk.abort_inside = 1; queue_result(NULL, 0, kIOReturnAborted, UINTPTR_MAX);
    runner_emit(SIGINT); (void)runner_slice(NULL, NULL); runner_close(); runner_reentry = 0;
    runner_finish(); CHECK(sdk.aborts == 1);
}
int main(int argc, char **argv) {
    if (argc != 3 || argv[1][0] != '/') return 2;
    const char *error = NULL;
    if (descriptor_load_fixture(argv[2], &fixture, &error)) return 2;
    session_inputs_test(argv[1]); session_launch_mode_test(argv[1]); session_preparation_test(argv[1]); session_running_test(argv[1]);
    session_error_wait_test(argv[1]); session_usb_label_test(argv[1]); session_owner_test(argv[1]);
    runner_inputs_test(argv[1]); runner_install_restore_test(argv[1]);
    runner_wait_signal_test(argv[1]); runner_running_wait_test(argv[1]);
    children_label_test(); adb_child_test(); validate_interval_test(); cached_write_test(); read_ahead_test(); acquisition_test(); completion_test(); usb_error_label_test(); cancellation_test(); wrong_owner_test(); probe_pointer_test();
    dispose_acquisition_test(); dispose_start_failure_test(argv[1]); dispose_completion_test();
    dispose_rejection_test(); dispose_close_test(); asynchronous_exec_failure_test();
    connect_hook_test(); observed_usb_test(argv[1], 0); observed_usb_test(argv[1], 1);
    integrated_test(argv[1], 3, 4096, "CONNECT\r\n"); integrated_test(argv[1], 17, 31, "CONNECT\r\n");
    integrated_test(argv[1], 127, 1, "CONNECT\r\n");
    integrated_test(argv[1], 17, 31, OFFLINE_CGDATA_COMMAND "\r\r\nCONNECT 150000000\r\n");
    supervisor_wait_test(argv[1], 0, 0); supervisor_wait_test(argv[1], 1, 0);
    supervisor_wait_test(argv[1], 0, 1); write_failure_test(argv[1]);
    undelivered_completion_test(argv[1], 0); undelivered_completion_test(argv[1], 1);
    printf("ppp_usb same implementation/fake SDK checks=%u failures=%u (own child fixture, no device/network/pppd)\n", checks, failures);
    return failures ? 1 : 0;
}
