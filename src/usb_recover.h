#ifndef DJI_USB_RECOVER_H
#define DJI_USB_RECOVER_H

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

/* One-shot modem recovery: a plain (never seizing) device open, one USB
 * re-enumeration without options, then close. Only ppp_lte_up calls it, before
 * any PPP session, when the pre-dial query met the PPP data stream instead of
 * AT replies. The device disappears and comes back as a new object, so the
 * caller must not use the same device handle for anything else afterwards. */
enum usb_recover_result { USB_RECOVER_OK, USB_RECOVER_OPEN_ERROR, USB_RECOVER_ERROR };
enum usb_recover_result usb_recover_reenumerate(IOUSBDeviceInterface **device);

#endif
