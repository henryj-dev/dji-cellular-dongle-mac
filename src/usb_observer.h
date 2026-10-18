#ifndef DJI_USB_OBSERVER_H
#define DJI_USB_OBSERVER_H
#include "descriptor.h"
#include <signal.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

/* Caller retains the singleton device service and cached accessor throughout. */
int usb_observer_observe(io_service_t service, IOUSBDeviceInterface **device,
                         uint64_t registry_id, const struct descriptor_config *config, int analyze,
                         const volatile sig_atomic_t *prior_cancelled);
/* Approved --probe-at only: interface 2, fixed AT\r once, bounded BULK IN replies. */
int usb_observer_probe_at(io_service_t service, IOUSBDeviceInterface **device,
                          uint64_t registry_id, const struct descriptor_config *config);
/* Approved --probe-ati only: interface 2, fixed ATI\r once, filtered/masked reply lines. */
int usb_observer_probe_ati(io_service_t service, IOUSBDeviceInterface **device,
                           uint64_t registry_id, const struct descriptor_config *config);
/* Approved --probe-status only: interface 2, fixed CPIN?/CSQ/COPS?/CEREG? once each. */
int usb_observer_probe_status(io_service_t service, IOUSBDeviceInterface **device,
                              uint64_t registry_id, const struct descriptor_config *config);
/* Approved --probe-pdp only: interface 2, fixed AT+CGDCONT? once, cid/type/APN only. */
int usb_observer_probe_pdp(io_service_t service, IOUSBDeviceInterface **device,
                           uint64_t registry_id, const struct descriptor_config *config);
/* Approved --probe-data-support only: interface 2, fixed AT+CGDATA=? once. */
int usb_observer_probe_data_support(io_service_t service, IOUSBDeviceInterface **device,
                                    uint64_t registry_id, const struct descriptor_config *config);
/* Before a PPP session only: interface 2, fixed AT+CSQ then AT+CESQ once each, same
 * guards as the probes. Fills *signal (numbers only); prints nothing. */
struct status_probe_result;
int usb_observer_probe_signal(io_service_t service, IOUSBDeviceInterface **device,
                              uint64_t registry_id, const struct descriptor_config *config,
                              const volatile sig_atomic_t *prior_cancelled,
                              struct status_probe_result *signal);
#endif
