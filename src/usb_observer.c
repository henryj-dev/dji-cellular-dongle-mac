#include "usb_observer.h"
#include "observer_policy.h"
#include "discovery.h"
#include "payload_analysis.h"
#include "at_probe.h"
#include "status_probe.h"
#include "usb_session.h"
#include <stdlib.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <signal.h>
#include <string.h>
#include <time.h>

struct usb_context {
    io_service_t device_service;
    IOUSBDeviceInterface **device;
    uint64_t device_id;
    const struct descriptor_config *config;
    const volatile sig_atomic_t *prior_cancelled;
    io_service_t services[OBSERVER_INTERFACES];
    uint64_t interface_ids[OBSERVER_INTERFACES];
    io_service_t current;
    uint64_t user_client_id;
    IOUSBInterfaceInterface182 **interface;
    int opened;      /* our exclusive USBInterfaceOpen is held */
    int adb_logged;  /* the tolerated adb client was reported once */
};

static volatile sig_atomic_t interrupted;
static void on_signal(int signal_number) { interrupted = signal_number; }

static uint64_t now_ms(void *context) {
    (void)context;
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return AT_PROBE_CLOCK_ERROR;
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static int cancelled(void *context) {
    struct usb_context *c = context;
    return interrupted != 0 || (c->prior_cancelled && *c->prior_cancelled != 0);
}

static int io_status(const char *operation, IOReturn code) {
    if (code == kIOReturnSuccess) return DJI_OK;
    int status = DJI_SYSTEM_ERROR;
    if (code == kIOReturnNoDevice || code == kIOReturnNotAttached) status = DJI_GONE;
    else if (code == kIOReturnNotPermitted || code == kIOReturnNotPrivileged) status = DJI_ACCESS;
    else if (code == kIOReturnExclusiveAccess || code == kIOReturnBusy) status = DJI_BUSY;
    else if (code == kIOReturnUnsupported) status = DJI_UNSUPPORTED;
    fprintf(stderr, "%s 실패: status=%d IOReturn=0x%08x\n", operation, status, (unsigned)code);
    return status;
}

/* Registry metadata only. Missing/non-numeric properties are not 'unowned'. */
static int registry_number(io_service_t service, CFStringRef key, int32_t *value) {
    CFTypeRef property = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    int valid = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
                CFNumberGetValue(property, kCFNumberSInt32Type, value);
    if (property) CFRelease(property);
    return valid;
}

/* Before plugin creation any child means refuse. After creation, allow only the
 * single identified USB user client created in that interval, not driver children.
 * This is a registry snapshot, not an atomic proof against driver attachment races.
 * Normal exclusive open still arbitrates ownership; we never seize or detach. */
/* IOUserClientCreator reads "pid N, name": keep the name only, reduced to
 * [A-Za-z0-9_.-] and 31 bytes; never the pid. */
static void creator_name(io_service_t child, char out[32]) {
    out[0] = 0;
    CFTypeRef value = IORegistryEntryCreateCFProperty(child, CFSTR("IOUserClientCreator"), kCFAllocatorDefault, 0);
    if (!value) return;
    char text[128];
    if (CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8)) {
        /* Only the documented "pid N, name" form; anything else is dropped so a
         * pid can never reach the label. */
        const char *name = strncmp(text, "pid ", 4) ? NULL : strstr(text, ", ");
        name = name ? name + 2 : "";
        size_t used = 0;
        for (const char *p = name; *p && used < 31; p++)
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                *p == '_' || *p == '.' || *p == '-') out[used++] = *p;
        out[used] = 0;
    }
    CFRelease(value);
}
/* Diagnostic text only: a fixed reason label, the child count and the first
 * unexpected child's IOKit class, reduced to [A-Za-z0-9_] and 63 bytes. Class
 * names are driver type names, never device identifiers or user data. The creator is a process
 * name only ([A-Za-z0-9_.-], 31 bytes), never its pid. */
/* Exactly "pid <digits>, adb" as the kernel records it, before any reduction. */
static int creator_is_adb(io_service_t child) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(child, CFSTR("IOUserClientCreator"), kCFAllocatorDefault, 0);
    if (!value) return 0;
    char text[64];
    int match = 0;
    if (CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8) && !strncmp(text, "pid ", 4)) {
        const char *p = text + 4;
        if (*p >= '0' && *p <= '9') {
            while (*p >= '0' && *p <= '9') p++;
            match = !strcmp(p, ", adb");
        }
    }
    CFRelease(value);
    return match;
}
static void children_label(char *out, size_t size, const char *reason, unsigned count, int captured,
                           const char *class_name, const char *creator) {
    char safe[64];
    size_t used = 0;
    for (const char *p = class_name; p && *p && used < sizeof(safe) - 1; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')
            safe[used++] = *p;
    safe[used] = 0;
    (void)snprintf(out, size, "children-check reason=%s children=%u captured=%d class=%s creator=%s",
                   reason, count, captured, used ? safe : "none", creator && creator[0] ? creator : "none");
}
static int check_children(struct usb_context *c, int capture) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    IOReturn kr = IORegistryEntryGetChildIterator(c->current, kIOServicePlane, &iterator);
    if (kr != KERN_SUCCESS) return io_status("interface children", kr);
    int status = DJI_OK;
    unsigned count = 0;
    const char *reason = "none";
    io_name_t class_name = "";
    char creator[32] = "";
    unsigned tolerated = 0;
    io_service_t child;
    while ((child = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        uint64_t id = 0;
        int user_client = IOObjectConformsTo(child, "IOUSBInterfaceUserClientV2") ||
                          IOObjectConformsTo(child, "IOUSBInterfaceUserClientV3") ||
                          IOObjectConformsTo(child, "IOUSBHostInterfaceUserClient");
        /* The Android Debug Bridge server probes Android-like interfaces and
         * leaves a user client. Only after our own client was captured and
         * while our exclusive open is held (so it cannot do pipe I/O), one
         * user client created by exactly "adb" is ignored. A second one, any
         * other creator, or any child before the open still stops us. */
        if (!capture && c->opened && c->interface && c->user_client_id && user_client && !tolerated &&
            IORegistryEntryGetRegistryEntryID(child, &id) == KERN_SUCCESS && id != c->user_client_id &&
            creator_is_adb(child)) {
            tolerated = 1;
            if (!c->adb_logged) {
                c->adb_logged = 1;
                fputs("children-check tolerated creator=adb\n", stderr);
            }
            IOObjectRelease(child);
            continue;
        }
        id = 0;
        count++;
        const char *why = !c->interface ? "child-before-open" : !user_client ? "non-user-client" :
            count > 1 ? "extra-child" : IORegistryEntryGetRegistryEntryID(child, &id) != KERN_SUCCESS ?
            "registry-id-error" : !capture && id != c->user_client_id ? "id-mismatch" : NULL;
        if (why) {
            if (status == DJI_OK) {
                reason = why;
                if (IOObjectGetClass(child, class_name) != KERN_SUCCESS) class_name[0] = 0;
                creator_name(child, creator);
            }
            status = DJI_BUSY;
        } else if (capture) c->user_client_id = id;
        IOObjectRelease(child);
    }
    if (!IOIteratorIsValid(iterator)) { status = DJI_GONE; reason = "iterator-invalid"; }
    IOObjectRelease(iterator);
    if (status != DJI_OK) {
        char label[192];
        /* captured=0: no own user client id was recorded at open time. */
        children_label(label, sizeof(label), reason, count, c->user_client_id != 0, class_name, creator);
        fprintf(stderr, "다른 owner/알 수 없는 child 또는 registry 변경: 열기/읽기 중단 %s\n", label);
    }
    return status;
}

static int device_matches(struct usb_context *c) {
    uint64_t id = 0;
    UInt16 vid = 0, pid = 0, release = 0;
    UInt8 config = 0;
    IOReturn kr = IORegistryEntryGetRegistryEntryID(c->device_service, &id);
    if (kr != KERN_SUCCESS) return io_status("device registry ID", kr);
    if ((kr = (*c->device)->GetDeviceVendor(c->device, &vid)) != KERN_SUCCESS ||
        (kr = (*c->device)->GetDeviceProduct(c->device, &pid)) != KERN_SUCCESS ||
        (kr = (*c->device)->GetDeviceReleaseNumber(c->device, &release)) != KERN_SUCCESS ||
        (kr = (*c->device)->GetConfiguration(c->device, &config)) != KERN_SUCCESS)
        return io_status("device metadata", kr);
    return id == c->device_id && vid == 0x2ca3 && pid == 0x4009 &&
           release == 0x0318 && config == 1 ? DJI_OK : DJI_GONE;
}

static void release_interface(void *context) {
    struct usb_context *c = context;
    if (c->interface) (*c->interface)->Release(c->interface);
    c->interface = NULL;
    c->current = IO_OBJECT_NULL;
    c->user_client_id = 0;
    c->opened = 0;
}

static int prepare(void *context, unsigned interface_id) {
    struct usb_context *c = context;
    int status = device_matches(c);
    if (status != DJI_OK) return status;
    c->current = c->services[interface_id];
    status = check_children(c, 0);
    if (status != DJI_OK) return status;
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(c->current, kIOUSBInterfaceUserClientTypeID,
                                                   kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        if (plugin) (*plugin)->Release(plugin);
        return kr == KERN_SUCCESS ? DJI_UNSUPPORTED : io_status("interface plugin", kr);
    }
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID182),
                                           (LPVOID *)&c->interface);
    (*plugin)->Release(plugin);
    if (hr || !c->interface) return DJI_UNSUPPORTED;
    return check_children(c, 1);
}

static int validate(void *context, unsigned interface_id, int opened, uint8_t *bulk_pipe) {
    struct usb_context *c = context;
    int status = device_matches(c);
    if (status != DJI_OK) return status;
    uint64_t id = 0, parent_id = 0;
    io_service_t parent = IO_OBJECT_NULL;
    IOReturn kr = IORegistryEntryGetRegistryEntryID(c->current, &id);
    if (kr != KERN_SUCCESS) return io_status("interface registry ID", kr);
    kr = IORegistryEntryGetParentEntry(c->current, kIOServicePlane, &parent);
    if (kr != KERN_SUCCESS) return io_status("interface parent", kr);
    kr = IORegistryEntryGetRegistryEntryID(parent, &parent_id);
    IOObjectRelease(parent);
    if (kr != KERN_SUCCESS) return io_status("parent registry ID", kr);
    if (id != c->interface_ids[interface_id] || parent_id != c->device_id) return DJI_GONE;
    int32_t registry_config = 0, registry_alt = -1, registry_interface = -1;
    if (!registry_number(c->current, CFSTR(kUSBConfigurationValue), &registry_config) ||
        !registry_number(c->current, CFSTR(kUSBAlternateSetting), &registry_alt) ||
        !registry_number(c->current, CFSTR(kUSBInterfaceNumber), &registry_interface)) return DJI_UNSUPPORTED;
    UInt16 vid = 0, pid = 0, release = 0;
    UInt8 config = 0, number = 0, alt = 0, endpoints = 0;
    IOUSBInterfaceInterface182 **interface = c->interface;
    if ((kr = (*interface)->GetDeviceVendor(interface, &vid)) != KERN_SUCCESS ||
        (kr = (*interface)->GetDeviceProduct(interface, &pid)) != KERN_SUCCESS ||
        (kr = (*interface)->GetDeviceReleaseNumber(interface, &release)) != KERN_SUCCESS ||
        (kr = (*interface)->GetConfigurationValue(interface, &config)) != KERN_SUCCESS ||
        (kr = (*interface)->GetInterfaceNumber(interface, &number)) != KERN_SUCCESS ||
        (kr = (*interface)->GetAlternateSetting(interface, &alt)) != KERN_SUCCESS ||
        (kr = (*interface)->GetNumEndpoints(interface, &endpoints)) != KERN_SUCCESS)
        return io_status("interface cached metadata", kr);
    if (vid != 0x2ca3 || pid != 0x4009 || release != 0x0318 || config != 1 || alt != 0 ||
        number != interface_id || registry_config != 1 || registry_alt != 0 ||
        registry_interface != (int32_t)interface_id ||
        endpoints != c->config->alts[interface_id].endpoint_count) return DJI_GONE;
    status = check_children(c, 0);
    if (status != DJI_OK || !opened) return status;
    /* Pipe properties are queried only after open, when kernel pipes exist. */
    *bulk_pipe = 0;
    for (UInt8 pipe = 1; pipe <= endpoints; pipe++) {
        UInt8 direction = 0, endpoint = 0, type = 0, interval = 0;
        UInt16 packet = 0;
        kr = (*interface)->GetPipeProperties(interface, pipe, &direction, &endpoint,
                                             &type, &packet, &interval);
        if (kr != KERN_SUCCESS) return io_status("pipe properties", kr);
        if ((direction != kUSBIn && direction != kUSBOut) || endpoint == 0 || endpoint > 15)
            return DJI_GONE;
        uint8_t address = endpoint | (direction == kUSBIn ? 0x80 : 0);
        if (!observer_pipe_matches(c->config, interface_id, pipe - 1, address, type, packet, interval))
            return DJI_GONE;
        if (direction == kUSBIn && type == kUSBBulk) {
            if (*bulk_pipe != 0) return DJI_INVALID_DESCRIPTOR;
            *bulk_pipe = pipe;
        }
    }
    return *bulk_pipe != 0 ? DJI_OK : DJI_INVALID_DESCRIPTOR;
}

static int open_interface(void *context) {
    struct usb_context *c = context;
    if (cancelled(c)) return DJI_CANCELLED;
    int status = io_status("USBInterfaceOpen", (*c->interface)->USBInterfaceOpen(c->interface));
    c->opened = status == DJI_OK;
    return status;
}

static int read_bulk(void *context, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    struct usb_context *c = context;
    if (cancelled(c)) return DJI_CANCELLED;
    IOReturn kr = (*c->interface)->ReadPipeTO(c->interface, pipe, buffer, size, timeout, timeout);
    if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) return OBSERVER_TIMEOUT;
    return io_status("ReadPipeTO", kr);
}

static int close_interface(void *context) {
    struct usb_context *c = context;
    c->opened = 0;
    return io_status("USBInterfaceClose", (*c->interface)->USBInterfaceClose(c->interface));
}

/* Probe-only: reuse the full observer validation, then require exactly one
 * BULK OUT 0x03 and BULK IN 0x84, both maxPacket 512, on interface 2. */
static int probe_validate(void *context, unsigned interface_id, int opened,
                          uint8_t *out_pipe, uint8_t *in_pipe) {
    struct usb_context *c = context;
    if (interface_id != AT_PROBE_INTERFACE) return DJI_INVALID_DESCRIPTOR;
    uint8_t bulk_in = 0;
    int status = validate(context, interface_id, opened, &bulk_in);
    if (status != DJI_OK || !opened) return status;
    IOUSBInterfaceInterface182 **interface = c->interface;
    UInt8 endpoints = c->config->alts[interface_id].endpoint_count;
    *out_pipe = *in_pipe = 0;
    for (UInt8 pipe = 1; pipe <= endpoints; pipe++) {
        UInt8 direction = 0, endpoint = 0, type = 0, interval = 0;
        UInt16 packet = 0;
        IOReturn kr = (*interface)->GetPipeProperties(interface, pipe, &direction, &endpoint,
                                                      &type, &packet, &interval);
        if (kr != KERN_SUCCESS) return io_status("pipe properties", kr);
        if (type != kUSBBulk) continue;
        uint8_t address = endpoint | (direction == kUSBIn ? 0x80 : 0);
        if (direction == kUSBOut && address == AT_PROBE_OUT_ADDRESS && packet == AT_PROBE_MAX_PACKET) {
            if (*out_pipe) return DJI_INVALID_DESCRIPTOR;
            *out_pipe = pipe;
        } else if (direction == kUSBIn && address == AT_PROBE_IN_ADDRESS && packet == AT_PROBE_MAX_PACKET) {
            if (*in_pipe) return DJI_INVALID_DESCRIPTOR;
            *in_pipe = pipe;
        } else return DJI_INVALID_DESCRIPTOR;
    }
    if (*out_pipe == 0 || *in_pipe == 0 || *in_pipe != bulk_in) return DJI_INVALID_DESCRIPTOR;
    return DJI_OK;
}

static int probe_open(void *context) {
    struct usb_context *c = context;
    if (c->current != c->services[AT_PROBE_INTERFACE]) return DJI_INVALID_DESCRIPTOR;
    return open_interface(context);
}

/* The single SDK OUT call. Probe and PPP policies stay separate above it. */
static int sdk_write(struct usb_context *c, uint8_t pipe, uint8_t *copy,
                     uint32_t size, uint32_t timeout) {
    IOReturn kr = (*c->interface)->WritePipeTO(c->interface, pipe, copy, size, timeout, timeout);
    if (kr == kIOUSBTransactionTimeout || kr == kIOReturnTimeout) {
        fprintf(stderr, "WritePipeTO timeout: IOReturn=0x%08x 재시도/clear-stall 없음\n", (unsigned)kr);
        return AT_PROBE_TIMEOUT;
    }
    return io_status("WritePipeTO", kr);
}

/* WritePipeTO reports no byte count. Timeout means delivery unknown; the caller
 * must not retry, read or clear the stall/data toggle. */
static int write_bulk(void *context, uint8_t pipe, const uint8_t *bytes, uint32_t size, uint32_t timeout) {
    struct usb_context *c = context;
    if (cancelled(c)) return DJI_CANCELLED;
    /* Only the fixed source constant lengths (3, 4, 7, 9, 10, 12) and only buffers that are
     * exactly an entry of the source command table are accepted. */
    uint8_t copy[12];
    if ((size != 3 && size != 4 && size != 7 && size != 9 && size != 10 && size != 12) ||
        !at_probe_is_fixed_command(bytes, size) || c->current != c->services[AT_PROBE_INTERFACE])
        return DJI_INVALID_DESCRIPTOR;
    memcpy(copy, bytes, size);
    return sdk_write(c, pipe, copy, size, timeout);
}

static int probe_read(void *context, uint8_t pipe, void *buffer, uint32_t *size, uint32_t timeout) {
    int status = read_bulk(context, pipe, buffer, size, timeout);
    return status == OBSERVER_TIMEOUT ? AT_PROBE_TIMEOUT : status;
}

struct usb_session {
    struct usb_context context;
    struct descriptor_config config;
    int opened, closed, close_status;
};

void usb_session_release(struct usb_session *s) {
    if (!s || (s->opened && !s->closed)) return;
    release_interface(&s->context);
    for (unsigned i = 0; i < OBSERVER_INTERFACES; i++)
        if (s->context.services[i]) IOObjectRelease(s->context.services[i]);
    if (s->context.device) (*s->context.device)->Release(s->context.device);
    if (s->context.device_service) IOObjectRelease(s->context.device_service);
    free(s);
}
int usb_session_close(struct usb_session *s) {
    if (!s) return DJI_SYSTEM_ERROR;
    if (!s->closed) {
        s->closed = 1;
        if (s->opened) s->close_status = close_interface(&s->context);
    }
    return s->close_status;
}
IOUSBInterfaceInterface182 **usb_session_interface(struct usb_session *s) {
    return s && s->opened && !s->closed ? s->context.interface : NULL;
}
int usb_session_validate(struct usb_session *s, uint8_t *out, uint8_t *in) {
    if (!s || !out || !in || !s->opened || s->closed) return DJI_SYSTEM_ERROR;
    return probe_validate(&s->context, AT_PROBE_INTERFACE, 1, out, in);
}
int usb_session_write_validated(struct usb_session *s, uint8_t out, const uint8_t *data, uint32_t size,
                                uint32_t timeout) {
    if (!s || !data || !size || size > 4096 || !timeout || !out || !s->opened || s->closed)
        return DJI_SYSTEM_ERROR;
    uint8_t copy[4096];
    memcpy(copy, data, size);
    return sdk_write(&s->context, out, copy, size, timeout);
}
int usb_session_write(struct usb_session *s, const uint8_t *data, uint32_t size, uint32_t timeout) {
    /* The adapter separately checks the bound supervisor gate; this policy does
     * not weaken the source-pointer allowlist of write_bulk. */
    if (!s || !data || !size || size > 4096 || !timeout) return DJI_SYSTEM_ERROR;
    uint8_t out = 0, in = 0, copy[4096];
    int status = usb_session_validate(s, &out, &in);
    if (status != DJI_OK) return status;
    memcpy(copy, data, size);
    return sdk_write(&s->context, out, copy, size, timeout);
}
int usb_session_acquire(io_service_t service, IOUSBDeviceInterface **device,
                        uint64_t registry_id, const struct descriptor_config *config,
                        struct usb_session **session) {
    if (!session) return DJI_SYSTEM_ERROR;
    *session = NULL;
    if (!service || !device || !observer_config_allowed(config)) return DJI_INVALID_DESCRIPTOR;
    struct usb_session *s = calloc(1, sizeof(*s));
    if (!s) return DJI_SYSTEM_ERROR;
    s->config = *config;
    s->context.config = &s->config;
    s->context.device_id = registry_id;
    int status = DJI_SYSTEM_ERROR;
    io_iterator_t iterator = IO_OBJECT_NULL;
    IOReturn kr = IOObjectRetain(service);
    if (kr != KERN_SUCCESS) goto cleanup;
    s->context.device_service = service;
    if (!(*device)->AddRef(device)) goto cleanup;
    s->context.device = device;
    status = device_matches(&s->context);
    if (status != DJI_OK) goto cleanup;
    IOUSBFindInterfaceRequest request = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                        kIOUSBFindInterfaceDontCare, 0};
    kr = (*device)->CreateInterfaceIterator(device, &request, &iterator);
    if (kr != KERN_SUCCESS) { status = io_status("interface iterator", kr); goto cleanup; }
    unsigned count = 0;
    io_service_t next;
    while ((next = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        int32_t number = -1;
        if (count >= OBSERVER_INTERFACES || !registry_number(next, CFSTR(kUSBInterfaceNumber), &number) ||
            number < 0 || number >= (int32_t)OBSERVER_INTERFACES || s->context.services[number]) {
            IOObjectRelease(next); status = DJI_INVALID_DESCRIPTOR; goto cleanup;
        }
        s->context.services[number] = next;
        kr = IORegistryEntryGetRegistryEntryID(next, &s->context.interface_ids[number]);
        if (kr != KERN_SUCCESS) { status = io_status("interface ID", kr); goto cleanup; }
        count++;
    }
    if (!IOIteratorIsValid(iterator) || count != OBSERVER_INTERFACES) { status = DJI_GONE; goto cleanup; }
    status = prepare(&s->context, AT_PROBE_INTERFACE);
    if (status != DJI_OK) goto cleanup;
    uint8_t out = 0, in = 0;
    status = probe_validate(&s->context, AT_PROBE_INTERFACE, 0, &out, &in);
    if (status != DJI_OK) goto cleanup;
    /* No global observer signal state is borrowed by this owned session. */
    status = io_status("USBInterfaceOpen", (*s->context.interface)->USBInterfaceOpen(s->context.interface));
    if (status != DJI_OK) goto cleanup;
    s->opened = 1;
    s->context.opened = 1;
    status = usb_session_validate(s, &out, &in);
cleanup:
    if (iterator) IOObjectRelease(iterator);
    if (status != DJI_OK) { (void)usb_session_close(s); usb_session_release(s); }
    else *session = s;
    return status;
}

static int run_mode(io_service_t service, IOUSBDeviceInterface **device,
                    uint64_t registry_id, const struct descriptor_config *config, int analyze,
                    const volatile sig_atomic_t *prior_cancelled, int probe,
                    enum at_probe_command command) {
    struct usb_context c = {.device_service = service, .device = device,
                            .device_id = registry_id, .config = config,
                            .prior_cancelled = prior_cancelled};
    io_iterator_t iterator = IO_OBJECT_NULL;
    int status = DJI_SYSTEM_ERROR;
    struct sigaction action, old_int, old_term;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    interrupted = 0;
    if (sigaction(SIGINT, &action, &old_int) != 0) return DJI_SYSTEM_ERROR;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        sigaction(SIGINT, &old_int, NULL);
        return DJI_SYSTEM_ERROR;
    }
    if (cancelled(&c)) { status = DJI_CANCELLED; goto cleanup; }
    status = device_matches(&c);
    if (status != DJI_OK) goto cleanup;
    if (cancelled(&c)) { status = DJI_CANCELLED; goto cleanup; }
    IOUSBFindInterfaceRequest request = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                        kIOUSBFindInterfaceDontCare, 0};
    IOReturn kr = (*device)->CreateInterfaceIterator(device, &request, &iterator);
    if (kr != KERN_SUCCESS) { status = io_status("interface iterator", kr); goto cleanup; }
    unsigned count = 0;
    io_service_t next;
    while ((next = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        if (cancelled(&c)) {
            IOObjectRelease(next);
            status = DJI_CANCELLED;
            goto cleanup;
        }
        int32_t number = -1;
        if (count >= OBSERVER_INTERFACES || !registry_number(next, CFSTR(kUSBInterfaceNumber), &number) ||
            number < 0 || number >= (int32_t)OBSERVER_INTERFACES || c.services[number]) {
            IOObjectRelease(next);
            status = DJI_INVALID_DESCRIPTOR;
            goto cleanup;
        }
        c.services[number] = next;
        kr = IORegistryEntryGetRegistryEntryID(next, &c.interface_ids[number]);
        if (kr != KERN_SUCCESS) { status = io_status("interface ID", kr); goto cleanup; }
        count++;
    }
    if (!IOIteratorIsValid(iterator)) { status = DJI_GONE; goto cleanup; }
    if (count != OBSERVER_INTERFACES) { status = DJI_GONE; goto cleanup; }
    if (probe) {
        const struct at_probe_backend probe_backend = {&c, now_ms, cancelled, prepare, probe_validate,
            probe_open, write_bulk, probe_read, close_interface, release_interface};
        if (probe == 2 || probe == 3 || probe == 4) {
            struct status_probe_result status_result;
            if (probe == 2) status = status_probe_run(&probe_backend, &status_result, stdout);
            else if (probe == 3) status = pdp_probe_run(&probe_backend, &status_result, stdout);
            else status = data_support_probe_run(&probe_backend, &status_result, stdout);
            goto cleanup;
        }
        struct at_probe_result probe_result;
        status = at_probe_run_command(&probe_backend, command, &probe_result, stdout);
        goto cleanup;
    }
    struct observer_backend backend = {&c, now_ms, cancelled, prepare, validate,
                                        open_interface, read_bulk, close_interface, release_interface};
    struct observer_result result;
    status = observer_run(&backend, &result, stdout, analyze ? payload_analyze_read : NULL);
cleanup:
    if (cancelled(&c) && status != DJI_CLOSE_ERROR) status = DJI_CANCELLED;
    if (iterator) IOObjectRelease(iterator);
    release_interface(&c);
    for (unsigned i = 0; i < OBSERVER_INTERFACES; i++)
        if (c.services[i]) IOObjectRelease(c.services[i]);
    int term_error = sigaction(SIGTERM, &old_term, NULL);
    int int_error = sigaction(SIGINT, &old_int, NULL);
    if (term_error != 0 || int_error != 0) status = DJI_SYSTEM_ERROR;
    return status;
}

int usb_observer_observe(io_service_t service, IOUSBDeviceInterface **device,
                         uint64_t registry_id, const struct descriptor_config *config, int analyze,
                         const volatile sig_atomic_t *prior_cancelled) {
    return run_mode(service, device, registry_id, config, analyze, prior_cancelled, 0, AT_PROBE_COMMAND_AT);
}

int usb_observer_probe_at(io_service_t service, IOUSBDeviceInterface **device,
                          uint64_t registry_id, const struct descriptor_config *config) {
    return run_mode(service, device, registry_id, config, 0, NULL, 1, AT_PROBE_COMMAND_AT);
}

int usb_observer_probe_ati(io_service_t service, IOUSBDeviceInterface **device,
                           uint64_t registry_id, const struct descriptor_config *config) {
    return run_mode(service, device, registry_id, config, 0, NULL, 1, AT_PROBE_COMMAND_ATI);
}

int usb_observer_probe_status(io_service_t service, IOUSBDeviceInterface **device,
                              uint64_t registry_id, const struct descriptor_config *config) {
    return run_mode(service, device, registry_id, config, 0, NULL, 2, AT_PROBE_COMMAND_AT);
}

int usb_observer_probe_pdp(io_service_t service, IOUSBDeviceInterface **device,
                           uint64_t registry_id, const struct descriptor_config *config) {
    return run_mode(service, device, registry_id, config, 0, NULL, 3, AT_PROBE_COMMAND_AT);
}

int usb_observer_probe_data_support(io_service_t service, IOUSBDeviceInterface **device,
                                    uint64_t registry_id, const struct descriptor_config *config) {
    return run_mode(service, device, registry_id, config, 0, NULL, 4, AT_PROBE_COMMAND_CGDATA_TEST);
}
