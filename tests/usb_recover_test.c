#include "../src/usb_recover.h"

#include <stdio.h>
#include <string.h>

/* Fake device object: only the vtable slots the recovery may touch are set;
 * any other slot is NULL and would crash the test if called. */
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL usb-recover line=%d\n", __LINE__); } } while (0)
static struct {
    int query_fails, open_result, reenumerate_result, null_interface;
    unsigned queries, opens, reenumerates, closes, releases;
    UInt32 options;
    int closed_after_open, reenumerate_while_open, open_now;
} f;
static IOUSBDeviceInterface187 vtable187;
static IOUSBDeviceInterface187 *object187 = &vtable187;
static HRESULT fake_query(void *self, REFIID iid, LPVOID *out) {
    (void)self; f.queries++;
    CFUUIDRef wanted = CFUUIDCreateFromUUIDBytes(NULL, iid);
    CHECK(wanted && CFEqual(wanted, kIOUSBDeviceInterfaceID187));
    if (wanted) CFRelease(wanted);
    if (f.query_fails) { *out = NULL; return E_NOINTERFACE; }
    *out = f.null_interface ? NULL : &object187;
    return S_OK;
}
static IOReturn fake_open(void *self) {
    CHECK(self == &object187); f.opens++;
    if (f.open_result == kIOReturnSuccess) f.open_now = 1;
    return f.open_result;
}
static IOReturn fake_reenumerate(void *self, UInt32 options) {
    CHECK(self == &object187); f.reenumerates++; f.options = options;
    f.reenumerate_while_open = f.open_now;
    return f.reenumerate_result;
}
static IOReturn fake_close(void *self) {
    CHECK(self == &object187); f.closes++; f.closed_after_open = f.open_now; f.open_now = 0;
    return kIOReturnNotOpen; /* the device is gone after a re-enumeration */
}
static ULONG fake_release(void *self) { CHECK(self == &object187); f.releases++; return 0; }

int main(void) {
    vtable187 = (IOUSBDeviceInterface187){0};
    vtable187.USBDeviceOpen = fake_open;
    vtable187.USBDeviceReEnumerate = fake_reenumerate;
    vtable187.USBDeviceClose = fake_close;
    vtable187.Release = fake_release;
    IOUSBDeviceInterface base = {0};
    base.QueryInterface = fake_query;
    IOUSBDeviceInterface *object = &base;
    IOUSBDeviceInterface **device = &object;

    /* Success: open, one re-enumeration with no options while open, close, release. */
    memset(&f, 0, sizeof(f));
    CHECK(usb_recover_reenumerate(device) == USB_RECOVER_OK);
    CHECK(f.queries == 1 && f.opens == 1 && f.reenumerates == 1 && !f.options && f.reenumerate_while_open &&
          f.closes == 1 && f.closed_after_open && f.releases == 1);

    /* Open refused (another client holds the device): nothing forced, no re-enumeration. */
    memset(&f, 0, sizeof(f)); f.open_result = kIOReturnExclusiveAccess;
    CHECK(usb_recover_reenumerate(device) == USB_RECOVER_OPEN_ERROR);
    CHECK(f.opens == 1 && !f.reenumerates && !f.closes && f.releases == 1);

    /* Re-enumeration refused: still closed and released. */
    memset(&f, 0, sizeof(f)); f.reenumerate_result = kIOReturnNotPermitted;
    CHECK(usb_recover_reenumerate(device) == USB_RECOVER_ERROR);
    CHECK(f.opens == 1 && f.reenumerates == 1 && f.closes == 1 && f.releases == 1);

    /* No 187 interface: error, nothing opened or released. */
    memset(&f, 0, sizeof(f)); f.query_fails = 1;
    CHECK(usb_recover_reenumerate(device) == USB_RECOVER_ERROR);
    CHECK(f.queries == 1 && !f.opens && !f.releases);
    memset(&f, 0, sizeof(f)); f.null_interface = 1;
    CHECK(usb_recover_reenumerate(device) == USB_RECOVER_ERROR && !f.opens && !f.releases);

    /* Invalid handles never reach the device. */
    memset(&f, 0, sizeof(f));
    IOUSBDeviceInterface *none = NULL;
    CHECK(usb_recover_reenumerate(NULL) == USB_RECOVER_ERROR);
    CHECK(usb_recover_reenumerate(&none) == USB_RECOVER_ERROR && !f.queries);

    printf("usb-recover checks=%u failures=%u (fake device vtable; no device)\n", checks, failures);
    return failures ? 1 : 0;
}
