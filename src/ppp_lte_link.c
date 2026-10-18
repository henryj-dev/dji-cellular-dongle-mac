#define _DARWIN_C_SOURCE
#include "ppp_lte_link.h"
#include "ppp_lte_network.h"
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>

int ppp_lte_link_init(struct ppp_lte_link *l, const char *name) {
    if (!l || !name || !name[0] || !memchr(name, 0, sizeof(l->name))) return -1;
    *l = (struct ppp_lte_link){.state = PPP_LTE_LINK_ABSENT};
    memcpy(l->name, name, strlen(name) + 1);
    return 0;
}
static void counted(unsigned *value) { if (*value != UINT_MAX) (*value)++; }
/* A stable link that stops being stable starts one loss period. */
static void lose(struct ppp_lte_link *l, uint64_t now) {
    if (l->stable_once && !l->lost_since_ms && l->state == PPP_LTE_LINK_STABLE) {
        l->lost_since_ms = now ? now : 1;
        counted(&l->losses);
    }
}
static int finish(struct ppp_lte_link *l, enum ppp_lte_link_state state, uint64_t now) {
    if (state != PPP_LTE_LINK_STABLE) lose(l, now);
    l->state = state;
    return (int)state;
}
int ppp_lte_link_step(struct ppp_lte_link *l, uint64_t now) {
    if (!l || !l->name[0] || now == UINT64_MAX || now < l->last_ms) return -1;
    l->last_ms = now;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) {
        l->pinned = 0;
        return finish(l, PPP_LTE_LINK_ERROR, now);
    }
    unsigned found = 0, walked = 0;
    struct in_addr local = {0}, peer = {0};
    for (struct ifaddrs *item = list; item; item = item->ifa_next) {
        if (++walked > 1024) { found = 2; break; }
        if (!item->ifa_addr || !item->ifa_name || strcmp(item->ifa_name, l->name) ||
            item->ifa_addr->sa_family != AF_INET) continue;
        if (!item->ifa_dstaddr || item->ifa_addr->sa_len != sizeof(struct sockaddr_in) ||
            item->ifa_dstaddr->sa_len != sizeof(struct sockaddr_in) ||
            item->ifa_dstaddr->sa_family != AF_INET ||
            (item->ifa_flags & (IFF_POINTOPOINT | IFF_UP)) != (IFF_POINTOPOINT | IFF_UP)) {
            found = 2; break;
        }
        local = ((struct sockaddr_in *)item->ifa_addr)->sin_addr;
        peer = ((struct sockaddr_in *)item->ifa_dstaddr)->sin_addr;
        if (ppp_lte_endpoint_allowed(local) || ppp_lte_endpoint_allowed(peer) ||
            local.s_addr == peer.s_addr) { found = 2; break; }
        found++;
    }
    freeifaddrs(list);
    if (found > 1) { l->pinned = 0; return finish(l, PPP_LTE_LINK_AMBIGUOUS, now); }
    if (!found) {
        if (l->pinned) counted(&l->resets);
        l->pinned = 0;
        return finish(l, PPP_LTE_LINK_ABSENT, now);
    }
    if (l->pinned && (local.s_addr != l->local.s_addr || peer.s_addr != l->peer.s_addr)) {
        /* A replaced pair restarts the settle wait like a vanished one. */
        counted(&l->resets);
        l->pinned = 0;
        lose(l, now);
    }
    if (!l->pinned) {
        l->pinned = 1; l->local = local; l->peer = peer; l->settle_start_ms = now;
        if (!l->first_pin_ms) l->first_pin_ms = now ? now : 1;
    }
    if (now - l->settle_start_ms < PPP_LTE_ADDRESS_SETTLE_MS) return finish(l, PPP_LTE_LINK_SETTLING, now);
    if (!l->first_stable_ms) l->first_stable_ms = now ? now : 1;
    if (l->lost_since_ms) { l->lost_since_ms = 0; counted(&l->restores); }
    l->stable_once = 1;
    l->state = PPP_LTE_LINK_STABLE;
    return (int)l->state;
}
const char *ppp_lte_link_label(enum ppp_lte_link_state state) {
    static const char *const labels[] = {"absent", "settling", "stable", "ambiguous", "error"};
    return (unsigned)state < sizeof(labels) / sizeof(labels[0]) ? labels[state] : "unknown";
}
