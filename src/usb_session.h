#ifndef DJI_USB_SESSION_H
#define DJI_USB_SESSION_H

#include "usb_observer.h"

/* Internal owned IF2 session; acquisition retains service/device and copies the
 * parsed configuration. No stack observer context is borrowed. Caller serializes
 * use and must prove async callbacks returned before close/release. */
struct usb_session;
int usb_session_acquire(io_service_t service, IOUSBDeviceInterface **device,
                        uint64_t registry_id, const struct descriptor_config *config,
                        struct usb_session **session);
IOUSBInterfaceInterface182 **usb_session_interface(struct usb_session *session);
int usb_session_validate(struct usb_session *session, uint8_t *out_pipe, uint8_t *in_pipe);
int usb_session_write(struct usb_session *session, const uint8_t *data, uint32_t size, uint32_t timeout);
/* Same single writer, without re-validation: out_pipe must come from a recent
 * successful usb_session_validate() on this open session (caller-bounded age). */
int usb_session_write_validated(struct usb_session *session, uint8_t out_pipe, const uint8_t *data,
                                uint32_t size, uint32_t timeout);
int usb_session_close(struct usb_session *session);
void usb_session_release(struct usb_session *session);

#endif
