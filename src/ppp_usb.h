#ifndef DJI_PPP_USB_H
#define DJI_PPP_USB_H

#include "usb_session.h"
#include "ppp_supervisor.h"

/* Internal, not linked to the production CLI. Acquire on the sole owner thread
 * and its CFRunLoop. Session and fixed IN buffer are owned until release.
 * timeout_ms is the OUT (write/connect hook) timeout; NULL chooses 1000 ms.
 * read_timeout_ms is the IN no-data/completion timeout; NULL chooses the OUT
 * value (former single-timeout behavior). Zero in either is rejected. An IN
 * timeout completion stays a fatal error: never resubmitted, cleared or reset
 * (after a timeout the data toggle may need resync, so the IN is not reused).
 * SDK timeouts are not hard wall bounds.
 * Peer binds the future supervisor; only its ready+CONNECT gate admits OUT.
 * Successful start transfers ownership to supervisor: discard caller adapter/
 * peer aliases and use only supervisor cleanup. Otherwise the caller owns the
 * adapter (including failed bind/start) and must finish dispose_step below.
 * Never abandon WAIT by free. */
struct ppp_usb;
int ppp_usb_acquire(io_service_t service, IOUSBDeviceInterface **device,
                    uint64_t registry_id, const struct descriptor_config *config,
                    const uint32_t *timeout_ms, const uint32_t *read_timeout_ms,
                    struct ppp_usb **adapter);
int ppp_usb_peer(struct ppp_usb *adapter, struct ppp_supervisor *supervisor, struct ppp_peer *peer);
/* Caller-owned adapters only: ownership is a caller precondition, NOT checked
 * against supervisor state. Same owner thread/CFRunLoop, serialized, outside
 * signal handlers. Once disposal starts, never rebind/restart this adapter.
 * released=0 retains the handle and every resource even when error=1: keep
 * calling until quiet. released=1 sets *adapter=NULL; discard borrowed peers.
 * error includes sticky USB/quiesce/poll/close failures, independently of release;
 * consume this result before a later NULL-handle no-op loses the old error.
 * Completion proves local release, not device/network restoration. Repeat only
 * with the same updated handle, never a copied stale pointer. NULL pointer is
 * {0,1}; an already NULL handle is {1,0}. Invalid owner/pumping/reentry is {0,1}
 * without mutation. Never use after successful supervisor start. */
struct ppp_usb_dispose_result { int released, error; };
struct ppp_usb_dispose_result ppp_usb_dispose_step(struct ppp_usb **adapter);
int ppp_usb_diagnostic(const struct ppp_usb *adapter, char *buffer, size_t capacity);

#endif
