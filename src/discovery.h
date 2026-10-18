#ifndef DJI_DISCOVERY_H
#define DJI_DISCOVERY_H

#include <stdint.h>

/* Shared CLI exit statuses; no device handles leave this module. */
enum dji_status {
    DJI_OK = 0,
    DJI_USAGE = 2,
    DJI_NOT_FOUND = 3,
    DJI_MULTIPLE = 4,
    DJI_GONE = 5,
    DJI_ACCESS = 6,
    DJI_BUSY = 7,
    DJI_UNSUPPORTED = 8,
    DJI_INVALID_DESCRIPTOR = 9,
    DJI_SYSTEM_ERROR = 10,
    DJI_CANCELLED = 11,
    DJI_CLOSE_ERROR = 12,
    DJI_RECONNECT_TIMEOUT = 13
};

/* Cached IOKit accessors only: no device open or USB requests. */
int discovery_inspect(void);
/* Explicit approved normal interface open and bounded BULK IN only. */
int discovery_observe(int analyze);
/* Approved --probe-at: same guards as observe, then interface 2 fixed AT\r once. */
int discovery_probe_at(void);
/* Approved --probe-ati: same guards, interface 2 fixed ATI\r once, filtered lines only. */
int discovery_probe_ati(void);
/* Approved --probe-status: same guards, interface 2 fixed status queries once each. */
int discovery_probe_status(void);
/* Approved --probe-pdp: same guards, interface 2 fixed AT+CGDCONT? once. */
int discovery_probe_pdp(void);
/* Approved --probe-data-support: interface 2 fixed AT+CGDATA=? once, capability only. */
int discovery_probe_data_support(void);
/* Registry-only reconnect wait, then one analysis of the captured generation. */
int discovery_wait_reconnect(void);
/* Shared pre-open identity guard; zero is never a captured generation. */
int discovery_generation_matches(uint64_t expected_id, uint64_t current_id);

#endif
