#ifndef DJI_PPP_LTE_LINK_H
#define DJI_PPP_LTE_LINK_H
#include <netinet/in.h>
#include <stdint.h>

/* Read-only IPv4 watch of one attributed PPP interface for the
 * connection-keeping mode. Same address rules as the single-shot network
 * module's address check, but no socket, route query or echo, and a loss is
 * an event, not a failure: the modem renegotiates IPCP after it opens. */
enum ppp_lte_link_state {
    PPP_LTE_LINK_ABSENT, PPP_LTE_LINK_SETTLING, PPP_LTE_LINK_STABLE,
    PPP_LTE_LINK_AMBIGUOUS, PPP_LTE_LINK_ERROR
};
struct ppp_lte_link {
    char name[64];
    struct in_addr local, peer;  /* private; never printed */
    enum ppp_lte_link_state state;
    int pinned, stable_once;
    uint64_t settle_start_ms, last_ms;
    /* Relative times are the caller's clock. first_*: 0 until it happened.
     * lost_since_ms: start of the current loss after the link was stable once
     * (address gone, replaced, ambiguous or unreadable), 0 while stable. */
    uint64_t first_pin_ms, first_stable_ms, lost_since_ms;
    /* resets: a pinned pair vanished or was replaced (not read errors or
     * ambiguity). losses/restores: stable periods ended/regained. All saturate. */
    unsigned resets, losses, restores;
};
/* name must be a NUL-terminated interface name shorter than 64 bytes. */
int ppp_lte_link_init(struct ppp_lte_link *link, const char *name);
/* One getifaddrs pass at now_ms (checked monotonic, not before the last
 * call). Returns the new state, or -1 on invalid input. */
int ppp_lte_link_step(struct ppp_lte_link *link, uint64_t now_ms);
const char *ppp_lte_link_label(enum ppp_lte_link_state state);
#endif
