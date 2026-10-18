#include "discovery.h"
#include "descriptor.h"
#include "observer_policy.h"
#include "usb_observer.h"
#include "reconnect.h"
#include "discovery_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#define TARGET_VID 0x2ca3
#define TARGET_PID 0x4009

static volatile sig_atomic_t wait_interrupted;

static int io_error(const char *operation, IOReturn code) {
    int status = DJI_SYSTEM_ERROR;
    if (code == kIOReturnNoDevice || code == kIOReturnNotAttached) status = DJI_GONE;
    else if (code == kIOReturnNotPermitted || code == kIOReturnNotPrivileged) status = DJI_ACCESS;
    else if (code == kIOReturnExclusiveAccess || code == kIOReturnBusy) status = DJI_BUSY;
    fprintf(stderr, "%s 실패: status=%d IOReturn=0x%08x\n", operation, status, (unsigned)code);
    return status;
}

static int target_service(io_service_t *service) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    *service = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) { fputs("장치 검색 사전을 만들지 못했습니다\n", stderr); return DJI_SYSTEM_ERROR; }
    int32_t vid_value = TARGET_VID, pid_value = TARGET_PID;
    CFNumberRef vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vid_value);
    CFNumberRef pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pid_value);
    if (!vid || !pid) {
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        CFRelease(match);
        fputs("장치 검색 값을 만들지 못했습니다\n", stderr);
        return DJI_SYSTEM_ERROR;
    }
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), vid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), pid);
    CFRelease(vid);
    CFRelease(pid);
    IOReturn kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);
    if (kr != KERN_SUCCESS) {
        if (iterator) IOObjectRelease(iterator);
        return io_error("IOServiceGetMatchingServices", kr);
    }
    unsigned count = 0;
    io_service_t next;
    while ((next = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        count++;
        if (count == 1) *service = next;
        else IOObjectRelease(next);
        if (count == 2) break;
    }
    int valid = IOIteratorIsValid(iterator);
    IOObjectRelease(iterator);
    /* A known duplicate is refused even if the registry changed during enumeration. */
    if (count > 1 || !valid) {
        if (*service) IOObjectRelease(*service);
        *service = IO_OBJECT_NULL;
        return count > 1 ? DJI_MULTIPLE : DJI_GONE;
    }
    return count == 1 ? DJI_OK : DJI_NOT_FOUND;
}

static int inspect_target(int observe, int analyze, uint64_t expected_id,
                          discovery_target_callback callback, void *context) {
    int result = DJI_SYSTEM_ERROR;
    io_service_t service = IO_OBJECT_NULL;
    IOCFPlugInInterface **plugin = NULL;
    IOUSBDeviceInterface **device = NULL;
    result = target_service(&service);
    if (result != DJI_OK) {
        fprintf(stderr, "VID=0x%04x PID=0x%04x 단일 장치 검색 실패: status=%d\n",
                TARGET_VID, TARGET_PID, result);
        goto cleanup;
    }
    uint64_t registry_id = 0, final_registry_id = 0;
    IOReturn kr = IORegistryEntryGetRegistryEntryID(service, &registry_id);
    if (kr != KERN_SUCCESS) { result = io_error("registry ID", kr); goto cleanup; }
    /* Refuse a replacement between wait detection and discovery, before any open. */
    if (expected_id && discovery_generation_matches(expected_id, registry_id) != DJI_OK) {
        result = DJI_GONE;
        goto cleanup;
    }
    SInt32 score = 0;
    kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                          kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        result = kr == KERN_SUCCESS ? DJI_UNSUPPORTED : io_error("device plugin", kr);
        goto cleanup;
    }
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                            (LPVOID *)&device);
    (*plugin)->Release(plugin);
    plugin = NULL;
    if (hr || !device) {
        fprintf(stderr, "캐시 accessor 인터페이스를 얻지 못했습니다: HRESULT=0x%08x\n", (unsigned)hr);
        result = DJI_UNSUPPORTED;
        goto cleanup;
    }
    UInt16 actual_vid = 0, actual_pid = 0, release = 0;
    UInt8 class_code = 0, subclass = 0, protocol = 0, configurations = 0;
    if ((kr = (*device)->GetDeviceVendor(device, &actual_vid)) != KERN_SUCCESS ||
        (kr = (*device)->GetDeviceProduct(device, &actual_pid)) != KERN_SUCCESS ||
        (kr = (*device)->GetDeviceReleaseNumber(device, &release)) != KERN_SUCCESS ||
        (kr = (*device)->GetDeviceClass(device, &class_code)) != KERN_SUCCESS ||
        (kr = (*device)->GetDeviceSubClass(device, &subclass)) != KERN_SUCCESS ||
        (kr = (*device)->GetDeviceProtocol(device, &protocol)) != KERN_SUCCESS ||
        (kr = (*device)->GetNumberOfConfigurations(device, &configurations)) != KERN_SUCCESS) {
        result = io_error("device cached accessor", kr);
        goto cleanup;
    }
    if (actual_vid != TARGET_VID || actual_pid != TARGET_PID) {
        fputs("캐시 VID/PID가 검색 대상과 다릅니다\n", stderr);
        result = DJI_GONE;
        goto cleanup;
    }
    if (configurations == 0) {
        result = DJI_INVALID_DESCRIPTOR;
        fputs("구성 설명자가 없습니다\n", stderr);
        goto cleanup;
    }
    if ((observe || callback) && (release != 0x0318 || configurations != 1)) {
        result = DJI_INVALID_DESCRIPTOR;
        goto cleanup;
    }
    if (!callback) {
    printf("mode=%s registryEntryID=0x%" PRIx64 "\n", observe == 6 ? "probe-data-support" : observe == 5 ? "probe-pdp" : observe == 4 ? "probe-status" : observe == 3 ? "probe-ati" : observe == 2 ? "probe-at" : observe ? "observe" : "cached", registry_id);
    printf("device VID=0x%04x PID=0x%04x bcdDevice=0x%04x class=0x%02x subclass=0x%02x protocol=0x%02x configurations=%u\n",
           actual_vid, actual_pid, release, class_code, subclass, protocol, configurations);
    }
    for (UInt8 ci = 0; ci < configurations; ci++) {
        IOUSBConfigurationDescriptorPtr cached = NULL;
        kr = (*device)->GetConfigurationDescriptorPtr(device, ci, &cached);
        if (kr != KERN_SUCCESS || !cached) {
            result = kr == KERN_SUCCESS ? DJI_INVALID_DESCRIPTOR : io_error("configuration cache", kr);
            goto cleanup;
        }
        /* SDK-owned kernel-trusted blob, NOT an arbitrary caller/file pointer.
         * This legacy API supplies no allocation length. We trust its fixed header
         * and wTotalLength, capped at 4096, before passing the derived length. */
        const uint8_t *bytes = (const uint8_t *)cached;
        size_t total = (size_t)bytes[2] | ((size_t)bytes[3] << 8);
        if (bytes[0] < 9 || bytes[1] != 2 || total < bytes[0] || total > DESCRIPTOR_LIMIT) {
            fputs("캐시 구성 헤더 또는 길이가 잘못됐습니다\n", stderr);
            result = DJI_INVALID_DESCRIPTOR;
            goto cleanup;
        }
        struct descriptor_config parsed;
        const char *error = NULL;
        if (descriptor_parse(bytes, total, &parsed, &error) != 0) {
            fprintf(stderr, "설명자 파싱 실패: %s\n", error);
            result = DJI_INVALID_DESCRIPTOR;
            goto cleanup;
        }
        if (callback) {
            if (!observer_config_allowed(&parsed)) { result = DJI_INVALID_DESCRIPTOR; goto cleanup; }
            kr = IORegistryEntryGetRegistryEntryID(service, &final_registry_id);
            if (kr != KERN_SUCCESS || !registry_id || final_registry_id != registry_id) {
                result = DJI_GONE; goto cleanup;
            }
            if ((*device)->GetDeviceVendor(device, &actual_vid) != KERN_SUCCESS ||
                (*device)->GetDeviceProduct(device, &actual_pid) != KERN_SUCCESS ||
                actual_vid != TARGET_VID || actual_pid != TARGET_PID) {
                result = DJI_GONE; goto cleanup;
            }
            result = callback(context, service, device, registry_id, &parsed);
            goto cleanup;
        }
        printf("configuration_index=%u\n", ci);
        descriptor_print(&parsed, stdout);
        if (observe) {
            if (!observer_config_allowed(&parsed)) {
                result = DJI_INVALID_DESCRIPTOR;
                goto cleanup;
            }
            if (expected_id && wait_interrupted) {
                result = DJI_CANCELLED;
                goto cleanup;
            }
            if (observe == 2) result = usb_observer_probe_at(service, device, registry_id, &parsed);
            else if (observe == 3) result = usb_observer_probe_ati(service, device, registry_id, &parsed);
            else if (observe == 4) result = usb_observer_probe_status(service, device, registry_id, &parsed);
            else if (observe == 5) result = usb_observer_probe_pdp(service, device, registry_id, &parsed);
            else if (observe == 6) result = usb_observer_probe_data_support(service, device, registry_id, &parsed);
            else result = usb_observer_observe(service, device, registry_id, &parsed, analyze,
                                               expected_id ? &wait_interrupted : NULL);
            goto cleanup;
        }
    }
    kr = IORegistryEntryGetRegistryEntryID(service, &final_registry_id);
    if (kr != KERN_SUCCESS) { result = io_error("registry ID 재확인", kr); goto cleanup; }
    if (final_registry_id != registry_id) { result = DJI_GONE; goto cleanup; }
    if ((kr = (*device)->GetDeviceVendor(device, &actual_vid)) != KERN_SUCCESS ||
        (kr = (*device)->GetDeviceProduct(device, &actual_pid)) != KERN_SUCCESS) {
        result = io_error("VID/PID 재확인", kr);
        goto cleanup;
    }
    result = actual_vid == TARGET_VID && actual_pid == TARGET_PID ? DJI_OK : DJI_GONE;
cleanup:
    if (device) (*device)->Release(device);
    if (plugin) (*plugin)->Release(plugin);
    if (service) IOObjectRelease(service);
    return result;
}

static int inspect(int observe, int analyze, uint64_t expected_id) {
    return inspect_target(observe, analyze, expected_id, NULL, NULL);
}
int discovery_with_one_target(discovery_target_callback callback, void *context) {
    if (!callback) return DJI_USAGE;
    return inspect_target(0, 0, 0, callback, context);
}
int discovery_inspect(void) { return inspect(0, 0, 0); }
int discovery_observe(int analyze) { return inspect(1, analyze, 0); }
int discovery_probe_at(void) { return inspect(2, 0, 0); }
int discovery_probe_ati(void) { return inspect(3, 0, 0); }
int discovery_probe_status(void) { return inspect(4, 0, 0); }
int discovery_probe_pdp(void) { return inspect(5, 0, 0); }
int discovery_probe_data_support(void) { return inspect(6, 0, 0); }

static void wait_signal(int signal_number) { wait_interrupted = signal_number; }
static int wait_cancelled(void *context) { (void)context; return wait_interrupted != 0; }

static uint64_t wait_now(void *context) {
    (void)context;
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return UINT64_MAX;
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static int wait_sleep(void *context, uint32_t milliseconds) {
    (void)context;
    struct timespec delay = {0, (long)milliseconds * 1000000};
    if (nanosleep(&delay, NULL) == 0) return DJI_OK;
    return errno == EINTR && wait_interrupted ? DJI_CANCELLED : DJI_SYSTEM_ERROR;
}

/* No plugin, device open, descriptor access or USB request while waiting. */
static int registry_snapshot(void *context, uint64_t *registry_id) {
    (void)context;
    *registry_id = 0;
    io_service_t service = IO_OBJECT_NULL;
    int status = target_service(&service);
    if (status == DJI_NOT_FOUND) return DJI_OK;
    if (status != DJI_OK) return status;
    const CFStringRef keys[] = {CFSTR(kUSBVendorID), CFSTR(kUSBProductID)};
    const int32_t values[] = {TARGET_VID, TARGET_PID};
    for (unsigned i = 0; i < 2; i++) {
        int32_t value = 0;
        CFTypeRef property = IORegistryEntryCreateCFProperty(service, keys[i], kCFAllocatorDefault, 0);
        int valid = property && CFGetTypeID(property) == CFNumberGetTypeID() &&
                    CFNumberGetValue(property, kCFNumberSInt32Type, &value);
        if (property) CFRelease(property);
        if (!valid || value != values[i]) {
            status = valid ? DJI_GONE : DJI_UNSUPPORTED;
            goto cleanup_snapshot;
        }
    }
    uint64_t id = 0;
    IOReturn kr = IORegistryEntryGetRegistryEntryID(service, &id);
    if (kr == kIOReturnNoDevice || kr == kIOReturnNotAttached) status = DJI_OK;
    else if (kr != KERN_SUCCESS) status = io_error("wait registry ID", kr);
    else if (id == 0) status = DJI_UNSUPPORTED;
    else *registry_id = id;
cleanup_snapshot:
    IOObjectRelease(service);
    /* Disconnection is allowed, but missing/unknown metadata is not a fallback. */
    return status;
}

int discovery_wait_reconnect(void) {
    struct sigaction action, old_int, old_term;
    memset(&action, 0, sizeof(action));
    action.sa_handler = wait_signal;
    sigemptyset(&action.sa_mask);
    wait_interrupted = 0;
    if (sigaction(SIGINT, &action, &old_int) != 0) return DJI_SYSTEM_ERROR;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        sigaction(SIGINT, &old_int, NULL);
        return DJI_SYSTEM_ERROR;
    }
    fputs("mode=wait-reconnect timeout_ms=90000 poll_ms=100 protocol=unconfirmed\n", stdout);
    fflush(stdout);
    const struct reconnect_backend backend = {NULL, wait_now, wait_cancelled,
                                               registry_snapshot, wait_sleep};
    uint64_t expected_id = 0;
    int status = reconnect_wait(&backend, &expected_id);
    if (wait_interrupted) status = DJI_CANCELLED;
    if (status == DJI_OK) {
        if (expected_id == 0) status = DJI_GONE;
        else status = inspect(1, 1, expected_id);
    }
    if (wait_interrupted && status != DJI_CLOSE_ERROR) status = DJI_CANCELLED;
    int term_error = sigaction(SIGTERM, &old_term, NULL);
    int int_error = sigaction(SIGINT, &old_int, NULL);
    if (term_error != 0 || int_error != 0) status = DJI_SYSTEM_ERROR;
    if (status != DJI_OK) fprintf(stderr, "재연결 관찰 종료: status=%d\n", status);
    return status;
}
