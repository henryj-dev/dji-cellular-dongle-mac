#ifndef DJI_DISCOVERY_INTERNAL_H
#define DJI_DISCOVERY_INTERNAL_H
#include "ppp_usb.h"
/* Internal synchronous borrow only. Callback MUST retain ownership until every
 * owned session/runner reaches CLOSED and is released, including WAIT paths.
 * No raw cached diagnostics in this entry point. Not an execution permit. */
typedef int (*discovery_target_callback)(void *context, io_service_t service,
    IOUSBDeviceInterface **device, uint64_t registry_id, const struct descriptor_config *config);
int discovery_with_one_target(discovery_target_callback callback, void *context);
#endif
