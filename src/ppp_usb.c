#include "ppp_usb.h"
#include "discovery.h"
#include "at_probe.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ppp_usb {
    struct usb_session *session;
    struct ppp_supervisor *supervisor;
    CFRunLoopRef loop;
    CFRunLoopSourceRef source;
    pthread_t owner;
    unsigned char input[PPP_SUPERVISOR_BUFFER];
    uint32_t timeout, read_timeout; /* OUT and IN transfer timeouts, kept separate. */
    uint8_t in_pipe;
    uintptr_t count;
    IOReturn error, completion;
    unsigned submissions, callbacks, aborts;
    int pending, received, completed, stopping, pumping, closed, close_error, connect_attempted;
    int dispose_active;
    enum ppp_usb_error_kind error_kind;
    enum ppp_usb_error_site error_site;
};
static int owner(struct ppp_usb *a) {
    return a && pthread_equal(a->owner, pthread_self()) && a->loop == CFRunLoopGetCurrent();
}
static enum ppp_usb_error_kind error_kind(IOReturn error) {
    if (error == kIOReturnTimeout || error == kIOUSBTransactionTimeout) return PPP_USB_ERROR_TIMEOUT;
    if (error == kIOReturnAborted || error == kIOUSBTransactionReturned) return PPP_USB_ERROR_ABORTED;
    if (error == kIOReturnNoDevice || error == kIOReturnNotAttached || error == kIOReturnOffline)
        return PPP_USB_ERROR_NO_DEVICE;
    if (error == kIOReturnNotOpen || error == kIOReturnExclusiveAccess) return PPP_USB_ERROR_NOT_OPEN;
    if (error == kIOUSBPipeStalled) return PPP_USB_ERROR_STALL;
    if (error == kIOReturnOverrun) return PPP_USB_ERROR_OVERRUN;
    return PPP_USB_ERROR_OTHER;
}
/* Sticky: only the first error is kept, together with its fixed label. kind
 * NONE maps the IOReturn; internal judgments pass their own kind. Labels are
 * diagnostics only and copied to a bound supervisor, which outlives us. */
static void remember_error(struct ppp_usb *a, IOReturn error, enum ppp_usb_error_kind kind,
                           enum ppp_usb_error_site site) {
    if (error == kIOReturnSuccess || a->error != kIOReturnSuccess) return;
    a->error = error;
    a->error_kind = kind == PPP_USB_ERROR_NONE ? error_kind(error) : kind;
    a->error_site = site;
    if (a->supervisor) {
        a->supervisor->usb_error_kind = a->error_kind;
        a->supervisor->usb_error_site = a->error_site;
    }
}
static enum ppp_usb_error_kind write_kind(int status) {
    return status == AT_PROBE_TIMEOUT ? PPP_USB_ERROR_TIMEOUT :
           status == DJI_GONE ? PPP_USB_ERROR_NO_DEVICE : PPP_USB_ERROR_OTHER;
}
static void read_complete(void *context, IOReturn result, void *count) {
    struct ppp_usb *a = context;
    /* No supervisor calls, free, close, or pending decrement here. Pump observes
     * received only after SDK dispatch has returned from this callback. */
    a->callbacks++;
    a->completion = result;
    a->count = result == kIOReturnSuccess ? (uintptr_t)count : 0;
    a->received = 1;
}
static void pump(struct ppp_usb *a) {
    a->pumping = 1;
    (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
    a->pumping = 0;
    if (a->received) {
        a->received = 0;
        a->pending = 0;
        a->completed = 1;
        if (a->completion != kIOReturnSuccess) {
            /* Only cancellation we requested is a normal end; a timeout stays fatal. */
            int aborted = a->completion == kIOReturnAborted || a->completion == kIOUSBTransactionReturned;
            if (!(a->stopping && aborted))
                remember_error(a, a->completion, PPP_USB_ERROR_NONE, PPP_USB_SITE_COMPLETION);
        }
        else if (!a->count)
            remember_error(a, kIOReturnBadArgument, PPP_USB_ERROR_ZERO_LENGTH, PPP_USB_SITE_COMPLETION);
        else if (a->count > sizeof(a->input))
            remember_error(a, kIOReturnBadArgument, PPP_USB_ERROR_OVERSIZE, PPP_USB_SITE_COMPLETION);
    }
}
static uint64_t usb_now(void *context) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping) return UINT64_MAX;
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static struct offline_io usb_read(void *context, unsigned char *data, size_t capacity) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || a->closed || a->stopping || !data || !capacity)
        return (struct offline_io){OFFLINE_IO_ERROR, 0};
    pump(a);
    if (a->error) return (struct offline_io){OFFLINE_IO_ERROR, 0};
    if (a->completed) {
        if (a->count > capacity) {
            remember_error(a, kIOReturnBadArgument, PPP_USB_ERROR_OVERSIZE, PPP_USB_SITE_COMPLETION);
            return (struct offline_io){OFFLINE_IO_ERROR, 0};
        }
        memcpy(data, a->input, a->count);
        size_t count = a->count;
        a->completed = 0;
        a->count = 0;
        return (struct offline_io){OFFLINE_IO_OK, count};
    }
    if (!a->pending) {
        uint8_t out = 0, in = 0;
        if (usb_session_validate(a->session, &out, &in) != DJI_OK) {
            remember_error(a, kIOReturnNoDevice, PPP_USB_ERROR_NONE, PPP_USB_SITE_VALIDATE);
            return (struct offline_io){OFFLINE_IO_ERROR, 0};
        }
        IOUSBInterfaceInterface182 **interface = usb_session_interface(a->session);
        a->in_pipe = in;
        a->pending = 1;
        a->submissions++;
        IOReturn result = (*interface)->ReadPipeAsyncTO(interface, in, a->input, sizeof(a->input),
                                                      a->read_timeout, a->read_timeout, read_complete, a);
        if (result != kIOReturnSuccess) {
            /* Failed submission has no outstanding SDK request. */
            a->pending = 0;
            remember_error(a, result, PPP_USB_ERROR_NONE, PPP_USB_SITE_SUBMIT);
            return (struct offline_io){OFFLINE_IO_ERROR, 0};
        }
    }
    return (struct offline_io){OFFLINE_IO_AGAIN, 0};
}
static struct offline_io usb_write(void *context, const unsigned char *data, size_t length) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || a->closed || a->stopping || a->error || !a->supervisor ||
        a->supervisor->peer.context != a || !a->supervisor->ready || !a->supervisor->connected ||
        !a->supervisor->stream.ready || a->supervisor->error ||
        (a->supervisor->phase != PPP_SESSION && a->supervisor->phase != PPP_DRAIN) ||
        !data || !length || length > PPP_SUPERVISOR_BUFFER)
        return (struct offline_io){OFFLINE_IO_ERROR, 0};
    int status = usb_session_write(a->session, data, (uint32_t)length, a->timeout);
    if (status != DJI_OK) {
        remember_error(a, kIOReturnError, write_kind(status), PPP_USB_SITE_WRITE);
        return (struct offline_io){OFFLINE_IO_ERROR, 0};
    }
    /* Requested span completed, not measured device reception. */
    return (struct offline_io){OFFLINE_IO_OK, length};
}
static int usb_begin_connect(void *context) {
    static const uint8_t command[] = "AT+CGDATA=\"PPP\",1\r";
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || a->closed || a->stopping || a->error ||
        a->connect_attempted || !a->supervisor || a->supervisor->peer.context != a ||
        a->supervisor->phase != PPP_CONNECT || !a->supervisor->ready ||
        !a->supervisor->child.exec_done || a->supervisor->child.exec_error ||
        a->supervisor->child.io_error || a->supervisor->child.reaped ||
        a->supervisor->child.ownership_lost || a->supervisor->child.transport_abandoned ||
        a->supervisor->stdout_eof || a->supervisor->error || a->supervisor->connected ||
        a->supervisor->stopped || !a->supervisor->connect_attempted)
        return -1;
    a->connect_attempted = 1; /* Even failed SDK calls must never be replayed. */
    int status = usb_session_write(a->session, command, sizeof(command) - 1, a->timeout);
    if (status != DJI_OK) {
        remember_error(a, kIOReturnError, write_kind(status), PPP_USB_SITE_CONNECT);
        return -1;
    }
    return 0; /* Request complete, not CONNECT or measured reception. */
}
static int usb_quiesce(void *context) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || a->closed) return -1;
    if (!a->stopping) {
        a->stopping = 1;
        if (a->pending) {
            IOUSBInterfaceInterface182 **interface = usb_session_interface(a->session);
            /* Validated pipe captured at acquisition; cancellation must remain
             * possible even if registry revalidation now reports unplug. */
            a->aborts++;
            IOReturn result = (*interface)->AbortPipe(interface, a->in_pipe);
            remember_error(a, result, PPP_USB_ERROR_NONE, PPP_USB_SITE_ABORT);
        }
    }
    return a->error ? -1 : 0;
}
static struct ppp_quiet usb_poll_quiet(void *context) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || !a->stopping) return (struct ppp_quiet){0, 1};
    pump(a);
    /* Quiet only ends callback ownership. A successful IN retained across
     * quiesce was never delivered and must fail drain before release. */
    if (a->completed && a->completion == kIOReturnSuccess && a->count)
        remember_error(a, kIOReturnError, PPP_USB_ERROR_OTHER, PPP_USB_SITE_COMPLETION);
    return (struct ppp_quiet){!a->pending, !!a->error};
}
static int usb_close(void *context) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || !a->stopping || a->pending || a->received) return -1;
    if (!a->closed) {
        a->closed = 1;
        CFRunLoopRemoveSource(a->loop, a->source, kCFRunLoopDefaultMode);
        a->close_error = usb_session_close(a->session) != DJI_OK;
    }
    return a->close_error;
}
static void usb_release(void *context) {
    struct ppp_usb *a = context;
    if (!owner(a) || a->pumping || !a->closed || a->pending || a->received) return;
    CFRelease(a->source);
    CFRelease(a->loop);
    usb_session_release(a->session);
    free(a);
}
int ppp_usb_acquire(io_service_t service, IOUSBDeviceInterface **device,
                    uint64_t registry_id, const struct descriptor_config *config,
                    const uint32_t *timeout_ms, const uint32_t *read_timeout_ms,
                    struct ppp_usb **adapter) {
    if (!adapter) return DJI_SYSTEM_ERROR;
    *adapter = NULL;
    uint32_t timeout = timeout_ms ? *timeout_ms : 1000;
    uint32_t read_timeout = read_timeout_ms ? *read_timeout_ms : timeout;
    if (!timeout || !read_timeout) return DJI_SYSTEM_ERROR;
    struct ppp_usb *a = calloc(1, sizeof(*a));
    if (!a) return DJI_SYSTEM_ERROR;
    int status = usb_session_acquire(service, device, registry_id, config, &a->session);
    if (status != DJI_OK) { free(a); return status; }
    a->timeout = timeout;
    a->read_timeout = read_timeout;
    a->owner = pthread_self();
    a->loop = CFRunLoopGetCurrent();
    CFRetain(a->loop);
    IOUSBInterfaceInterface182 **interface = usb_session_interface(a->session);
    IOReturn result = (*interface)->CreateInterfaceAsyncEventSource(interface, &a->source);
    if (result != kIOReturnSuccess || !a->source) {
        if (a->source) CFRelease(a->source);
        CFRelease(a->loop);
        (void)usb_session_close(a->session);
        usb_session_release(a->session);
        free(a);
        return DJI_SYSTEM_ERROR;
    }
    CFRunLoopAddSource(a->loop, a->source, kCFRunLoopDefaultMode);
    *adapter = a;
    return DJI_OK;
}
int ppp_usb_peer(struct ppp_usb *a, struct ppp_supervisor *s, struct ppp_peer *peer) {
    if (!owner(a) || !s || !peer || a->supervisor || a->stopping || a->closed) return -1;
    a->supervisor = s;
    *peer = (struct ppp_peer){a, usb_read, usb_write, usb_now, usb_close, usb_release,
                             usb_quiesce, usb_poll_quiet, usb_begin_connect};
    return 0;
}
struct ppp_usb_dispose_result ppp_usb_dispose_step(struct ppp_usb **adapter) {
    if (!adapter) return (struct ppp_usb_dispose_result){0, 1};
    struct ppp_usb *a = *adapter;
    if (!a) return (struct ppp_usb_dispose_result){1, 0};
    if (!owner(a) || a->pumping || a->dispose_active)
        return (struct ppp_usb_dispose_result){0, 1};
    /* Cancellation may dispatch inline before pump sets pumping. */
    a->dispose_active = 1;
    int error = !!a->error || a->close_error;
    if (!a->closed && usb_quiesce(a)) error = 1;
    struct ppp_quiet quiet = usb_poll_quiet(a);
    if (quiet.error) error = 1;
    if (!quiet.quiet) {
        a->dispose_active = 0;
        return (struct ppp_usb_dispose_result){0, error};
    }
    if (usb_close(a)) error = 1;
    /* Preserve the result before release frees a; never read it afterwards. */
    usb_release(a);
    *adapter = NULL;
    return (struct ppp_usb_dispose_result){1, error};
}
int ppp_usb_diagnostic(const struct ppp_usb *a, char *buffer, size_t capacity) {
    if (!a || (!buffer && capacity)) return -1;
    return snprintf(buffer, capacity, "usb phase=%s error=%d pending=%d submissions=%u callbacks=%u aborts=%u",
                    a->closed ? "closed" : a->stopping ? (a->pending ? "wait" : "quiet") : "active",
                    !!a->error, a->pending, a->submissions, a->callbacks, a->aborts);
}
