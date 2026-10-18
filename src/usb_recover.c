#include "usb_recover.h"

/* The cached accessor is the base device interface; re-enumeration lives in
 * the 187 vtable of the same object. A failed open is never forced. */
enum usb_recover_result usb_recover_reenumerate(IOUSBDeviceInterface **device) {
    if (!device || !*device) return USB_RECOVER_ERROR;
    IOUSBDeviceInterface187 **dev = NULL;
    HRESULT hr = (*device)->QueryInterface(device, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID187), (LPVOID *)&dev);
    if (hr != S_OK || !dev || !*dev) return USB_RECOVER_ERROR;
    enum usb_recover_result result = USB_RECOVER_OPEN_ERROR;
    if ((*dev)->USBDeviceOpen(dev) == kIOReturnSuccess) {
        result = (*dev)->USBDeviceReEnumerate(dev, 0) == kIOReturnSuccess ? USB_RECOVER_OK : USB_RECOVER_ERROR;
        /* After a successful re-enumeration the device is already gone; the
         * close result says nothing about the recovery. */
        (void)(*dev)->USBDeviceClose(dev);
    }
    (void)(*dev)->Release(dev);
    return result;
}
