#define _DARWIN_C_SOURCE
#include "ppp_lte_link.h"
#include "ppp_lte_network.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-link line=%d\n", __LINE__); } } while (0)
/* Fake interface table: up to four IPv4 entries, all freed by the fake free. */
static struct {
    struct ifaddrs items[4];
    struct sockaddr_in local[4], peer[4];
    unsigned count, lists, frees;
    int error, loop;
} f;
static void entry(const char *name, const char *local, const char *peer, unsigned flags) {
    unsigned i = f.count++;
    f.local[i] = (struct sockaddr_in){.sin_len = sizeof(struct sockaddr_in), .sin_family = AF_INET};
    f.peer[i] = f.local[i];
    (void)inet_pton(AF_INET, local, &f.local[i].sin_addr);
    if (peer) (void)inet_pton(AF_INET, peer, &f.peer[i].sin_addr);
    f.items[i] = (struct ifaddrs){.ifa_name = (char *)name, .ifa_flags = flags,
        .ifa_addr = (struct sockaddr *)&f.local[i], .ifa_dstaddr = peer ? (struct sockaddr *)&f.peer[i] : NULL};
}
static int fake_getifaddrs(struct ifaddrs **list) {
    f.lists++;
    if (f.error) return -1;
    for (unsigned i = 0; i + 1 < f.count; i++) f.items[i].ifa_next = &f.items[i + 1];
    if (f.count) f.items[f.count - 1].ifa_next = f.loop ? &f.items[0] : NULL;
    *list = f.count ? &f.items[0] : NULL;
    return 0;
}
static void fake_freeifaddrs(struct ifaddrs *list) { (void)list; f.frees++; }
static int fake_endpoint(struct in_addr address) {
    uint32_t host = ntohl(address.s_addr);
    return host && host != 0xffffffffu && (host >> 24) != 127 &&
        (host & 0xffff0000u) != 0xa9fe0000u && (host >> 28) < 0xe ? 0 : -1;
}
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#define ppp_lte_endpoint_allowed fake_endpoint
#include "../src/ppp_lte_link.c"
#undef ppp_lte_endpoint_allowed
#undef freeifaddrs
#undef getifaddrs

#define P2P (IFF_UP | IFF_POINTOPOINT)
static void table(const char *local, const char *peer) {
    f.count = 0; f.error = 0; f.loop = 0;
    entry("en0", "192.168.1.5", "192.168.1.1", IFF_UP);
    if (local) entry("ppp7", local, peer, P2P);
}
static void inputs_test(void) {
    struct ppp_lte_link l;
    char long_name[80]; memset(long_name, 'a', sizeof(long_name) - 1); long_name[sizeof(long_name) - 1] = 0;
    CHECK(ppp_lte_link_init(NULL, "ppp7") == -1 && ppp_lte_link_init(&l, NULL) == -1);
    CHECK(ppp_lte_link_init(&l, "") == -1 && ppp_lte_link_init(&l, long_name) == -1);
    CHECK(ppp_lte_link_step(NULL, 1) == -1);
    CHECK(ppp_lte_link_init(&l, "ppp7") == 0 && l.state == PPP_LTE_LINK_ABSENT && !strcmp(l.name, "ppp7"));
    table(NULL, NULL);
    CHECK(ppp_lte_link_step(&l, 100) == PPP_LTE_LINK_ABSENT);
    /* Clock failure or regression: rejected without a host call. */
    unsigned lists = f.lists;
    CHECK(ppp_lte_link_step(&l, 99) == -1 && ppp_lte_link_step(&l, UINT64_MAX) == -1 && f.lists == lists);
    const char *labels[] = {"absent", "settling", "stable", "ambiguous", "error"};
    for (unsigned i = 0; i < 5; i++) CHECK(!strcmp(ppp_lte_link_label(i), labels[i]));
    CHECK(!strcmp(ppp_lte_link_label((enum ppp_lte_link_state)9), "unknown"));
}
/* Pin, settle exactly at the window, then stay stable. */
static void settle_test(void) {
    struct ppp_lte_link l;
    CHECK(ppp_lte_link_init(&l, "ppp7") == 0);
    table("10.20.30.40", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 1000) == PPP_LTE_LINK_SETTLING && l.pinned && l.first_pin_ms == 1000);
    CHECK(ppp_lte_link_step(&l, 1000 + PPP_LTE_ADDRESS_SETTLE_MS - 1) == PPP_LTE_LINK_SETTLING && !l.first_stable_ms);
    CHECK(ppp_lte_link_step(&l, 1000 + PPP_LTE_ADDRESS_SETTLE_MS) == PPP_LTE_LINK_STABLE);
    CHECK(l.first_stable_ms == 1000 + PPP_LTE_ADDRESS_SETTLE_MS && l.stable_once && !l.lost_since_ms);
    CHECK(ppp_lte_link_step(&l, 9000) == PPP_LTE_LINK_STABLE && !l.resets && !l.losses && !l.restores);
    CHECK(f.lists == f.frees);
}
/* The modem's IPCP renegotiation right after opening: the pair vanishes before
 * it settled, comes back and settles; no loss is counted before stable. */
static void early_reset_test(void) {
    struct ppp_lte_link l;
    CHECK(ppp_lte_link_init(&l, "ppp7") == 0);
    table("10.20.30.40", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 1000) == PPP_LTE_LINK_SETTLING);
    table(NULL, NULL);
    CHECK(ppp_lte_link_step(&l, 2000) == PPP_LTE_LINK_ABSENT && l.resets == 1 && !l.pinned);
    table("10.20.30.41", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 2500) == PPP_LTE_LINK_SETTLING && l.first_pin_ms == 1000);
    CHECK(ppp_lte_link_step(&l, 2500 + PPP_LTE_ADDRESS_SETTLE_MS) == PPP_LTE_LINK_STABLE);
    CHECK(!l.losses && !l.restores && !l.lost_since_ms && l.first_stable_ms == 2500 + PPP_LTE_ADDRESS_SETTLE_MS);
    /* A replaced pair before settling restarts the window too. */
    CHECK(ppp_lte_link_init(&l, "ppp7") == 0);
    table("10.20.30.40", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 1000) == PPP_LTE_LINK_SETTLING);
    table("10.20.30.42", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 3500) == PPP_LTE_LINK_SETTLING && l.resets == 1 && l.settle_start_ms == 3500);
    CHECK(ppp_lte_link_step(&l, 4100) == PPP_LTE_LINK_SETTLING && !l.losses);
}
/* After stable: a loss starts once, the return restores, the time is kept. */
static void loss_test(void) {
    struct ppp_lte_link l;
    CHECK(ppp_lte_link_init(&l, "ppp7") == 0);
    table("10.20.30.40", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 0) == PPP_LTE_LINK_SETTLING && l.first_pin_ms == 1);
    CHECK(ppp_lte_link_step(&l, 5000) == PPP_LTE_LINK_STABLE);
    table(NULL, NULL);
    CHECK(ppp_lte_link_step(&l, 6000) == PPP_LTE_LINK_ABSENT && l.lost_since_ms == 6000 && l.losses == 1);
    CHECK(ppp_lte_link_step(&l, 7000) == PPP_LTE_LINK_ABSENT && l.lost_since_ms == 6000 && l.losses == 1);
    table("10.20.30.40", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 8000) == PPP_LTE_LINK_SETTLING && l.lost_since_ms == 6000);
    CHECK(ppp_lte_link_step(&l, 11000) == PPP_LTE_LINK_STABLE && !l.lost_since_ms && l.restores == 1);
    /* A replaced pair while stable is a loss as well. */
    table("10.20.30.99", "10.64.64.64");
    CHECK(ppp_lte_link_step(&l, 12000) == PPP_LTE_LINK_SETTLING && l.lost_since_ms == 12000 && l.losses == 2);
    CHECK(l.resets == 2);
    CHECK(ppp_lte_link_step(&l, 15000) == PPP_LTE_LINK_STABLE && l.restores == 2 && !l.lost_since_ms);
    /* Unreadable table and ambiguity after stable: losses, never stable. */
    f.error = 1;
    CHECK(ppp_lte_link_step(&l, 16000) == PPP_LTE_LINK_ERROR && l.lost_since_ms == 16000 && l.losses == 3 && !l.pinned);
    table("10.20.30.40", "10.64.64.64"); entry("ppp7", "10.20.30.50", "10.64.64.64", P2P);
    CHECK(ppp_lte_link_step(&l, 17000) == PPP_LTE_LINK_AMBIGUOUS && l.lost_since_ms == 16000 && l.losses == 3);
    CHECK(f.lists == f.frees + 1); /* the failed list was never allocated */
}
static void ambiguous_test(void) {
    struct ppp_lte_link l;
    f.lists = f.frees = 0;
    const char *bad[][2] = {{"127.0.0.2", "10.64.64.64"}, {"10.20.30.40", "0.0.0.0"},
        {"169.254.1.1", "10.64.64.64"}, {"10.20.30.40", "10.20.30.40"}, {"224.0.0.1", "10.64.64.64"}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(ppp_lte_link_init(&l, "ppp7") == 0);
        table(bad[i][0], bad[i][1]);
        CHECK(ppp_lte_link_step(&l, 100) == PPP_LTE_LINK_AMBIGUOUS && !l.pinned && !l.first_pin_ms);
    }
    /* Not point-to-point, not up, or no peer address. */
    CHECK(ppp_lte_link_init(&l, "ppp7") == 0);
    table(NULL, NULL); entry("ppp7", "10.20.30.40", "10.64.64.64", IFF_UP);
    CHECK(ppp_lte_link_step(&l, 100) == PPP_LTE_LINK_AMBIGUOUS);
    table(NULL, NULL); entry("ppp7", "10.20.30.40", "10.64.64.64", IFF_POINTOPOINT);
    CHECK(ppp_lte_link_step(&l, 200) == PPP_LTE_LINK_AMBIGUOUS);
    table(NULL, NULL); entry("ppp7", "10.20.30.40", NULL, P2P);
    CHECK(ppp_lte_link_step(&l, 300) == PPP_LTE_LINK_AMBIGUOUS);
    /* Another interface's address never counts; a looping list is bounded. */
    table(NULL, NULL); entry("ppp8", "10.20.30.40", "10.64.64.64", P2P);
    CHECK(ppp_lte_link_step(&l, 400) == PPP_LTE_LINK_ABSENT);
    table("10.20.30.40", "10.64.64.64"); f.loop = 1;
    CHECK(ppp_lte_link_step(&l, 500) == PPP_LTE_LINK_AMBIGUOUS);
    CHECK(f.lists == f.frees);
}
int main(void) {
    inputs_test(); settle_test(); early_reset_test(); loss_test(); ambiguous_test();
    printf("lte-link fake-interface checks=%u failures=%u (no host interface or network)\n", checks, failures);
    return failures ? 1 : 0;
}
