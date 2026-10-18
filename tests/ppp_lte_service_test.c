#define _DARWIN_C_SOURCE
#include "ppp_lte_service.h"
#include "ppp_launch.h"
#include <errno.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-service line=%d\n", __LINE__); } } while (0)
#define ID PPP_LTE_UP_SERVICE_ID
#define FIXED(entity) "State:/Network/Service/" ID "/" entity
/* Fake dynamic store: one dictionary of keys, every remove recorded. */
static struct {
    CFMutableDictionaryRef store;
    CFMutableArrayRef removed;
    int list_error, remove_failure, read_error, list_count_error;
    unsigned lists, removes, reads;
    /* Fake process table. */
    pid_t pids[8];
    const char *paths[8];
    int count, list_fail, oversize, path_errno;
    unsigned process_calls;
} f;
static const SCDynamicStoreRef fake_store = (SCDynamicStoreRef)(uintptr_t)0x5c;
static CFArrayRef fake_key_list(SCDynamicStoreRef store, CFStringRef pattern) {
    CHECK(store == fake_store);
    CHECK(CFEqual(pattern, CFSTR("^State:/Network/Service/" ID "/[^/]+$")));
    f.lists++;
    if (f.list_error) return NULL;
    CFIndex count = CFDictionaryGetCount(f.store);
    const void **keys = calloc((size_t)count + 1, sizeof(*keys));
    CFDictionaryGetKeysAndValues(f.store, keys, NULL);
    CFMutableArrayRef list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    /* Like the store pattern: only keys under the fixed ID prefix. */
    for (CFIndex i = 0; i < count; i++)
        if (CFStringHasPrefix(keys[i], CFSTR("State:/Network/Service/" ID "/")))
            CFArrayAppendValue(list, keys[i]);
    free(keys);
    return list;
}
static Boolean fake_remove(SCDynamicStoreRef store, CFStringRef key) {
    CHECK(store == fake_store); f.removes++;
    CFArrayAppendValue(f.removed, key);
    if (f.remove_failure) return false;
    CFDictionaryRemoveValue(f.store, key);
    return true;
}
static int last_error;
static CFPropertyListRef fake_copy(SCDynamicStoreRef store, CFStringRef key) {
    CHECK(store == fake_store); f.reads++;
    if (f.read_error) { last_error = kSCStatusFailed; return NULL; }
    CFTypeRef value = CFDictionaryGetValue(f.store, key);
    if (!value) { last_error = kSCStatusNoKey; return NULL; }
    return CFRetain(value);
}
static int fake_error(void) { return last_error; }
static int fake_listallpids(void *buffer, int size) {
    f.process_calls++;
    if (f.list_fail) return 0;
    if (!buffer) return f.count;
    int capacity = size / (int)sizeof(pid_t);
    if (f.oversize) return capacity;
    CHECK(capacity > f.count);
    memcpy(buffer, f.pids, (size_t)f.count * sizeof(pid_t));
    return f.count;
}
static int fake_pidpath(int pid, void *buffer, uint32_t size) {
    for (int i = 0; i < f.count; i++) {
        if (f.pids[i] != pid) continue;
        if (!f.paths[i]) { errno = f.path_errno ? f.path_errno : ESRCH; return 0; }
        (void)snprintf(buffer, size, "%s", f.paths[i]);
        return (int)strlen(f.paths[i]);
    }
    errno = ESRCH; return 0;
}
static pid_t fake_getpid(void) { return 4242; }
#define SCDynamicStoreCopyKeyList fake_key_list
#define SCDynamicStoreRemoveValue fake_remove
#define SCDynamicStoreCopyValue fake_copy
#define SCError fake_error
#define proc_listallpids fake_listallpids
#define proc_pidpath fake_pidpath
#define getpid fake_getpid
#include "../src/ppp_lte_service.c"
#undef getpid
#undef proc_pidpath
#undef proc_listallpids
#undef SCError
#undef SCDynamicStoreCopyValue
#undef SCDynamicStoreRemoveValue
#undef SCDynamicStoreCopyKeyList

static void reset(void) {
    if (f.store) CFRelease(f.store);
    if (f.removed) CFRelease(f.removed);
    memset(&f, 0, sizeof(f));
    f.store = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    f.removed = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    /* A normal host: launchd, this process, a shell, a VPN that is not pppd. */
    const pid_t pids[] = {1, 4242, 300, 301};
    const char *paths[] = {"/sbin/launchd", "/private-build-marker/ppp_lte_up", "/bin/zsh",
        "/Applications/private-vpn-marker"};
    f.count = 4;
    memcpy(f.pids, pids, sizeof(pids)); memcpy(f.paths, paths, sizeof(paths));
}
static void put(CFStringRef key, CFTypeRef value) { CFDictionarySetValue(f.store, key, value); }
static CFDictionaryRef dict2(CFStringRef k1, CFTypeRef v1, CFStringRef k2, CFTypeRef v2) {
    const void *keys[] = {k1, k2}, *values[] = {v1, v2};
    return CFDictionaryCreate(NULL, keys, values, k2 ? 2 : 1, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
}
static void others(void) {
    /* Other services and globals must survive every cleanup untouched. */
    put(CFSTR("State:/Network/Service/OTHER-SERVICE/IPv4"), CFSTR("v"));
    put(CFSTR("State:/Network/Service/OTHER-SERVICE/PPP"), CFSTR("v"));
    put(CFSTR("Setup:/Network/Service/" ID "/PPP"), CFSTR("v"));
    put(CFSTR("State:/Network/Global/IPv4"), CFSTR("v"));
}
static int others_intact(void) {
    return CFDictionaryContainsKey(f.store, CFSTR("State:/Network/Service/OTHER-SERVICE/IPv4")) &&
        CFDictionaryContainsKey(f.store, CFSTR("State:/Network/Service/OTHER-SERVICE/PPP")) &&
        CFDictionaryContainsKey(f.store, CFSTR("Setup:/Network/Service/" ID "/PPP")) &&
        CFDictionaryContainsKey(f.store, CFSTR("State:/Network/Global/IPv4"));
}
/* Every removed key is one of the seven compile-time constants. */
static int removed_fixed_only(void) {
    for (CFIndex i = 0; i < CFArrayGetCount(f.removed); i++) {
        CFTypeRef key = CFArrayGetValueAtIndex(f.removed, i);
        int match = 0;
        for (unsigned j = 0; j < PPP_LTE_ENTITIES; j++) if (CFEqual(key, ppp_lte_service_key(j))) match = 1;
        if (!match) return 0;
    }
    return 1;
}
static void label_is(const struct ppp_lte_service_scan *scan, const char *expected) {
    char text[128];
    int n = ppp_lte_service_label(scan, text, sizeof(text));
    CHECK(n > 0 && (size_t)n < sizeof(text) && !strcmp(text, expected));
    CHECK(!strstr(text, ID) && !strstr(text, "private"));
    CHECK(ppp_lte_service_label(scan, NULL, 0) == n);
}
static void keys_test(void) {
    const char *names[] = {"PPP", "IPv4", "IPv6", "DNS", "Proxies", "Interface", "SMB"};
    for (unsigned i = 0; i < PPP_LTE_ENTITIES; i++) {
        char expected[128];
        (void)snprintf(expected, sizeof(expected), "State:/Network/Service/%s/%s", ID, names[i]);
        CFStringRef wanted = CFStringCreateWithCString(NULL, expected, kCFStringEncodingASCII);
        CHECK(wanted && CFEqual(ppp_lte_service_key(i), wanted));
        if (wanted) CFRelease(wanted);
    }
    CHECK(!ppp_lte_service_key(PPP_LTE_ENTITIES) && !ppp_lte_service_key((enum ppp_lte_entity)-1));
}
static void scan_test(void) {
    reset(); others();
    struct ppp_lte_service_scan scan;
    CHECK(ppp_lte_service_scan(NULL, &scan) == -1 && ppp_lte_service_scan(fake_store, NULL) == -1);
    CHECK(ppp_lte_service_scan(fake_store, &scan) == 0 && !scan.present && !scan.unknown && !scan.error);
    label_is(&scan, "none");
    put(CFSTR(FIXED("IPv6")), CFSTR("v")); put(CFSTR(FIXED("PPP")), CFSTR("v"));
    put(CFSTR(FIXED("Private-Marker-Entity")), CFSTR("v"));
    CHECK(ppp_lte_service_scan(fake_store, &scan) == 0 && !scan.error);
    CHECK(scan.present == ((1u << PPP_LTE_ENTITY_PPP) | (1u << PPP_LTE_ENTITY_IPV6)) && scan.unknown == 1);
    label_is(&scan, "ppp+ipv6+other:1");
    f.list_error = 1;
    CHECK(ppp_lte_service_scan(fake_store, &scan) == 0 && scan.error && !scan.present);
    label_is(&scan, "unproven");
    for (unsigned i = 0; i < PPP_LTE_ENTITIES; i++) {
        reset(); put(ppp_lte_service_key(i), CFSTR("v"));
        CHECK(ppp_lte_service_scan(fake_store, &scan) == 0 && scan.present == 1u << i && !scan.unknown);
    }
    struct ppp_lte_service_scan all = {.present = (1u << PPP_LTE_ENTITIES) - 1, .unknown = 4294967295u};
    label_is(&all, "ppp+ipv4+ipv6+dns+proxies+interface+smb+other:4294967295");
    CHECK(ppp_lte_service_label(NULL, NULL, 0) == -1);
    char tiny[4];
    CHECK(ppp_lte_service_label(&all, tiny, sizeof(tiny)) > 3 && !tiny[3]);
}
static void processes_test(void) {
    reset();
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_NONE);
    /* Another pppd (even an unrelated VPN one) blocks: never remove under a live pppd. */
    reset(); f.paths[2] = "/usr/sbin/pppd";
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_PPPD);
    /* Another instance of this tool blocks; this very process does not. */
    reset(); f.paths[3] = "/elsewhere/ppp_lte_up";
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_PEER);
    reset(); f.paths[3] = "/elsewhere/ppp_lte_up_test";
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_NONE);
    /* pppd wins over a peer, wherever it is in the list. */
    reset(); f.paths[2] = "/elsewhere/ppp_lte_up"; f.paths[3] = "/usr/sbin/pppd";
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_PPPD);
    /* An exited process is skipped; an unreadable or truncated list fails closed. */
    reset(); f.paths[2] = NULL;
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_NONE);
    reset(); f.list_fail = 1;
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_UNKNOWN);
    /* An unreadable path that is not an exit could hide a pppd. */
    reset(); f.paths[2] = NULL; f.path_errno = EPERM;
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_UNKNOWN);
    /* A pppd copied elsewhere still counts as pppd. */
    reset(); f.paths[2] = "/private-copy-marker/pppd";
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_PPPD);
    reset(); f.oversize = 1;
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_UNKNOWN);
    reset(); f.count = 0;
    CHECK(ppp_lte_service_processes() == PPP_LTE_PROCESSES_UNKNOWN);
}
static void cleanup_test(void) {
    struct ppp_lte_service_cleanup r;
    CHECK(ppp_lte_service_cleanup(NULL, &r) == -1 && ppp_lte_service_cleanup(fake_store, NULL) == -1);
    CHECK(!ppp_lte_service_clean(NULL));
    /* Nothing left: done, no remove at all. */
    reset(); others();
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_DONE);
    CHECK(!r.removed && !r.failed && !f.removes && ppp_lte_service_clean(&r) && others_intact());
    /* pppd's leftovers after a normal exit: only the IPv6 dummy primary. */
    reset(); others(); put(CFSTR(FIXED("IPv6")), CFSTR("v"));
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_DONE);
    CHECK(r.before.present == 1u << PPP_LTE_ENTITY_IPV6 && r.removed == 1 && !r.after.present);
    CHECK(ppp_lte_service_clean(&r) && others_intact() && removed_fixed_only());
    /* SIGKILL leftovers: all seven fixed keys plus an unknown one, which stays. */
    reset(); others();
    for (unsigned i = 0; i < PPP_LTE_ENTITIES; i++) put(ppp_lte_service_key(i), CFSTR("v"));
    put(CFSTR(FIXED("Private-Marker-Entity")), CFSTR("v"));
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_DONE);
    CHECK(r.removed == PPP_LTE_ENTITIES && f.removes == PPP_LTE_ENTITIES && !r.failed);
    CHECK(!r.after.present && r.after.unknown == 1 && !ppp_lte_service_clean(&r));
    CHECK(CFDictionaryContainsKey(f.store, CFSTR(FIXED("Private-Marker-Entity"))));
    CHECK(others_intact() && removed_fixed_only());
    /* Idempotent: the second run removes nothing more. */
    unsigned removes = f.removes;
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && !r.removed && f.removes == removes);
    /* A live pppd blocks every remove but the scan is still reported. */
    reset(); others(); put(CFSTR(FIXED("IPv4")), CFSTR("v")); f.paths[2] = "/usr/sbin/pppd";
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_BLOCKED_PPPD);
    CHECK(!f.removes && r.before.present == 1u << PPP_LTE_ENTITY_IPV4 && r.after.present == r.before.present);
    CHECK(!ppp_lte_service_clean(&r) && CFDictionaryContainsKey(f.store, CFSTR(FIXED("IPv4"))));
    reset(); put(CFSTR(FIXED("IPv4")), CFSTR("v")); f.paths[3] = "/x/ppp_lte_up";
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_BLOCKED_PEER && !f.removes);
    reset(); put(CFSTR(FIXED("IPv4")), CFSTR("v")); f.list_fail = 1;
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_BLOCKED_PROCESSES && !f.removes);
    /* Unreadable store: no remove, unproven. */
    reset(); put(CFSTR(FIXED("IPv4")), CFSTR("v")); f.list_error = 1;
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_STORE_ERROR);
    CHECK(!f.removes && !ppp_lte_service_clean(&r));
    /* Remove failures are counted and the key stays. */
    reset(); put(CFSTR(FIXED("DNS")), CFSTR("v")); put(CFSTR(FIXED("PPP")), CFSTR("v")); f.remove_failure = 1;
    CHECK(ppp_lte_service_cleanup(fake_store, &r) == 0 && r.status == PPP_LTE_CLEANUP_DONE);
    CHECK(r.failed == 2 && !r.removed && r.after.present == r.before.present && !ppp_lte_service_clean(&r));
    const char *labels[] = {"not-run", "done", "blocked-pppd", "blocked-instance", "blocked-processes", "store-error"};
    for (unsigned i = 0; i < 6; i++) CHECK(!strcmp(ppp_lte_service_status_label(i), labels[i]));
    CHECK(!strcmp(ppp_lte_service_status_label((enum ppp_lte_cleanup_status)99), "unknown"));
}
static void primary_test(void) {
    struct ppp_lte_primary p;
    CHECK(ppp_lte_service_primary(NULL, "ppp0", &p) == -1 && ppp_lte_service_primary(fake_store, "ppp0", NULL) == -1);
    /* Empty store: nothing primary, no DNS. */
    reset();
    CHECK(ppp_lte_service_primary(fake_store, "private-if-marker", &p) == 0);
    CHECK(p.ipv4 == PPP_LTE_PRIMARY_NONE && p.ipv6 == PPP_LTE_PRIMARY_NONE && p.interface == 0 &&
          p.dns == PPP_LTE_DNS_ABSENT);
    /* This service is primary for both families and its DNS is the active set. */
    CFArrayRef servers = CFArrayCreate(NULL, (const void *[]){CFSTR("private-dns-marker")}, 1, &kCFTypeArrayCallBacks);
    CFArrayRef other_servers = CFArrayCreate(NULL, (const void *[]){CFSTR("private-other-dns")}, 1, &kCFTypeArrayCallBacks);
    CFDictionaryRef ipv4 = dict2(CFSTR("PrimaryService"), CFSTR(ID), CFSTR("PrimaryInterface"), CFSTR("private-if-marker"));
    CFDictionaryRef ipv6 = dict2(CFSTR("PrimaryService"), CFSTR(ID), NULL, NULL);
    CFDictionaryRef dns = dict2(CFSTR("ServerAddresses"), servers, NULL, NULL);
    CFDictionaryRef other_dns = dict2(CFSTR("ServerAddresses"), other_servers, NULL, NULL);
    put(CFSTR("State:/Network/Global/IPv4"), ipv4); put(CFSTR("State:/Network/Global/IPv6"), ipv6);
    put(CFSTR(FIXED("DNS")), dns); put(CFSTR("State:/Network/Global/DNS"), dns);
    CHECK(ppp_lte_service_primary(fake_store, "private-if-marker", &p) == 0);
    CHECK(p.ipv4 == PPP_LTE_PRIMARY_THIS && p.ipv6 == PPP_LTE_PRIMARY_THIS && p.interface == 1 &&
          p.dns == PPP_LTE_DNS_ACTIVE);
    /* Another interface name, no name, and a DNS set that is not active. */
    CHECK(ppp_lte_service_primary(fake_store, "ppp9", &p) == 0 && p.interface == 0);
    CHECK(ppp_lte_service_primary(fake_store, NULL, &p) == 0 && p.interface == -1);
    put(CFSTR("State:/Network/Global/DNS"), other_dns);
    CHECK(ppp_lte_service_primary(fake_store, NULL, &p) == 0 && p.dns == PPP_LTE_DNS_INACTIVE);
    /* Wi-Fi back as primary after the stop. */
    CFDictionaryRef wifi = dict2(CFSTR("PrimaryService"), CFSTR("OTHER-SERVICE"), CFSTR("PrimaryInterface"), CFSTR("en0"));
    put(CFSTR("State:/Network/Global/IPv4"), wifi);
    CFDictionaryRemoveValue(f.store, CFSTR("State:/Network/Global/IPv6"));
    CHECK(ppp_lte_service_primary(fake_store, "private-if-marker", &p) == 0);
    CHECK(p.ipv4 == PPP_LTE_PRIMARY_OTHER && p.ipv6 == PPP_LTE_PRIMARY_NONE && p.interface == 0);
    /* Wrong types and read failures stay unproven, never other/none. */
    put(CFSTR("State:/Network/Global/IPv4"), CFSTR("not-a-dictionary"));
    CHECK(ppp_lte_service_primary(fake_store, "x", &p) == 0 && p.ipv4 == PPP_LTE_PRIMARY_UNPROVEN && p.interface == -1);
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &(int){1});
    CFDictionaryRef odd = dict2(CFSTR("PrimaryService"), number, NULL, NULL);
    put(CFSTR("State:/Network/Global/IPv4"), odd);
    CHECK(ppp_lte_service_primary(fake_store, "x", &p) == 0 && p.ipv4 == PPP_LTE_PRIMARY_UNPROVEN);
    f.read_error = 1;
    CHECK(ppp_lte_service_primary(fake_store, "x", &p) == 0 && p.ipv4 == PPP_LTE_PRIMARY_UNPROVEN &&
          p.ipv6 == PPP_LTE_PRIMARY_UNPROVEN && p.interface == -1 && p.dns == PPP_LTE_DNS_UNPROVEN);
    /* Read-only: no remove happened anywhere in the probe. */
    CHECK(!f.removes);
    const char *primary[] = {"unproven", "none", "this-service", "other-service"};
    for (unsigned i = 0; i < 4; i++) CHECK(!strcmp(ppp_lte_primary_label(i), primary[i]));
    const char *dns_labels[] = {"unproven", "absent", "active", "inactive"};
    for (unsigned i = 0; i < 4; i++) CHECK(!strcmp(ppp_lte_dns_label(i), dns_labels[i]));
    CHECK(!strcmp(ppp_lte_primary_label((enum ppp_lte_primary_state)9), "unknown"));
    CHECK(!strcmp(ppp_lte_dns_label((enum ppp_lte_dns_state)9), "unknown"));
    CFRelease(servers); CFRelease(other_servers); CFRelease(ipv4); CFRelease(ipv6); CFRelease(dns);
    CFRelease(other_dns); CFRelease(wifi); CFRelease(number); CFRelease(odd);
}
int main(void) {
    keys_test(); scan_test(); processes_test(); cleanup_test(); primary_test();
    if (f.store) CFRelease(f.store);
    if (f.removed) CFRelease(f.removed);
    printf("lte-service fake-store checks=%u failures=%u (no host store, process or network)\n", checks, failures);
    return failures ? 1 : 0;
}
