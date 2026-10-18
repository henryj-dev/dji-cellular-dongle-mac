#ifndef DJI_PPP_LTE_NETWORK_H
#define DJI_PPP_LTE_NETWORK_H
#include "ppp_trial_observer.h"
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/* Single fixed echo transaction, not a general transport/configuration ABI.
 * Native OS boundaries are replaced in the test translation unit. No DNS. */
#define PPP_LTE_PROBE "dji-lte-once\r\n"
/* In the IP stage one local/peer pair must hold this long before the route
 * query and socket start. Bounded by the IP deadline; not used after it. */
#define PPP_LTE_ADDRESS_SETTLE_MS 3000
enum ppp_lte_stage { PPP_LTE_IP, PPP_LTE_OPEN, PPP_LTE_CONNECT,
    PPP_LTE_CONNECTED, PPP_LTE_SEND, PPP_LTE_SHUTDOWN, PPP_LTE_RECEIVE,
    PPP_LTE_DONE, PPP_LTE_FAILED };
struct ppp_lte_network {
    struct in_addr endpoint, local, peer;
    uint16_t port;
    struct ppp_trial_interface interface;
    /* gate rechecks pending stop, live child and current unique attribution.
     * It never steps/polls the child. Zero means currently eligible. */
    int (*gate)(void *context, struct ppp_trial_interface *interface);
    void *context;
    enum ppp_lte_stage stage;
    const char *reason;
    int socket_fd, route_fd, route_pending, sequence;
    pid_t route_pid;
    int pinned, connected_once, route_sent, write_closed;
    uint64_t overall_deadline, ip_deadline, app_deadline, last_ms;
    size_t sent, received;
    unsigned socket_closes, route_closes;
    /* last_ms when the current pin was taken; reset with the pin in the IP stage. */
    uint64_t settle_start_ms;
    /* Diagnostics only, never used for a decision. pinned_ms: last_ms when an
     * address was pinned the very first time, 0 if never; an IP-stage re-pin
     * does not overwrite it. failed_stage: stage at the first fail() (sticky,
     * same call as the first reason); PPP_LTE_FAILED if none. lost_ms: last_ms
     * when the first failure was address-missing or address-changed (after the
     * IP stage), 0 otherwise; an IP-stage loss only counts in address_resets.
     * stable_ms: last_ms when the current pin passed the settle window, 0 if
     * not (cleared by a reset). address_resets: IP-stage unpins, saturating. */
    uint64_t pinned_ms, lost_ms, stable_ms;
    unsigned address_resets;
    enum ppp_lte_stage failed_stage;
};
/* No OS access. start_ms must come from a checked monotonic clock. */
int ppp_lte_network_init(struct ppp_lte_network *network, struct in_addr endpoint,
    uint16_t port, uint64_t start_ms,
    int (*gate)(void *, struct ppp_trial_interface *), void *context);
/* One zero-wait bounded stage, 0 pending / 1 exact echo+EOF / -1 failure.
 * Call runner_slice between stages. Abort before stepping if caller stops.
 * Every terminal path closes each owned descriptor once. */
int ppp_lte_network_step(struct ppp_lte_network *network);
void ppp_lte_network_abort(struct ppp_lte_network *network, const char *reason);
int ppp_lte_endpoint_allowed(struct in_addr endpoint);
#endif
