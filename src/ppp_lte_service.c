#define _DARWIN_C_SOURCE
#include "ppp_lte_service.h"
#include "ppp_launch.h"

#include <errno.h>
#include <libproc.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PPP_LTE_SERVICE_PREFIX "State:/Network/Service/" PPP_LTE_UP_SERVICE_ID "/"
/* Compile-time constants only: the sole keys RemoveValue may ever receive. */
static const CFStringRef service_keys[PPP_LTE_ENTITIES] = {
    CFSTR(PPP_LTE_SERVICE_PREFIX "PPP"), CFSTR(PPP_LTE_SERVICE_PREFIX "IPv4"),
    CFSTR(PPP_LTE_SERVICE_PREFIX "IPv6"), CFSTR(PPP_LTE_SERVICE_PREFIX "DNS"),
    CFSTR(PPP_LTE_SERVICE_PREFIX "Proxies"), CFSTR(PPP_LTE_SERVICE_PREFIX "Interface"),
    CFSTR(PPP_LTE_SERVICE_PREFIX "SMB")
};
static const char *const entity_labels[PPP_LTE_ENTITIES] = {
    "ppp", "ipv4", "ipv6", "dns", "proxies", "interface", "smb"
};
CFStringRef ppp_lte_service_key(enum ppp_lte_entity entity) {
    return (unsigned)entity < PPP_LTE_ENTITIES ? service_keys[entity] : NULL;
}
int ppp_lte_service_scan(SCDynamicStoreRef store, struct ppp_lte_service_scan *scan) {
    if (!store || !scan) return -1;
    *scan = (struct ppp_lte_service_scan){0};
    CFArrayRef keys = SCDynamicStoreCopyKeyList(store, CFSTR("^" PPP_LTE_SERVICE_PREFIX "[^/]+$"));
    if (!keys) { scan->error = 1; return 0; }
    CFIndex count = CFArrayGetCount(keys);
    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef key = CFArrayGetValueAtIndex(keys, i);
        unsigned match = PPP_LTE_ENTITIES;
        if (key && CFGetTypeID(key) == CFStringGetTypeID())
            for (unsigned j = 0; j < PPP_LTE_ENTITIES && match == PPP_LTE_ENTITIES; j++)
                if (CFEqual(key, service_keys[j])) match = j;
        if (match < PPP_LTE_ENTITIES) scan->present |= 1u << match;
        else if (scan->unknown != UINT_MAX) scan->unknown++;
    }
    CFRelease(keys);
    return 0;
}
static int base_is(const char *path, const char *name) {
    const char *slash = strrchr(path, '/');
    return !strcmp(slash ? slash + 1 : path, name);
}
enum ppp_lte_processes ppp_lte_service_processes(void) {
    int estimate = proc_listallpids(NULL, 0);
    if (estimate <= 0 || estimate > 1 << 20) return PPP_LTE_PROCESSES_UNKNOWN;
    size_t capacity = (size_t)estimate + 256;
    pid_t *pids = calloc(capacity, sizeof(*pids));
    if (!pids) return PPP_LTE_PROCESSES_UNKNOWN;
    int count = proc_listallpids(pids, (int)(capacity * sizeof(*pids)));
    /* A full buffer may have dropped processes started meanwhile. */
    if (count <= 0 || (size_t)count >= capacity) { free(pids); return PPP_LTE_PROCESSES_UNKNOWN; }
    pid_t self = getpid();
    enum ppp_lte_processes result = PPP_LTE_PROCESSES_NONE;
    for (int i = 0; i < count; i++) {
        if (pids[i] <= 0 || pids[i] == self) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        /* Only a process that exited meanwhile is skipped; any other unreadable
         * path could be a pppd, so the whole check fails closed. */
        errno = 0;
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0) {
            if (errno == ESRCH) continue;
            result = PPP_LTE_PROCESSES_UNKNOWN; break;
        }
        if (!strcmp(path, "/usr/sbin/pppd") || base_is(path, "pppd")) { result = PPP_LTE_PROCESSES_PPPD; break; }
        if (base_is(path, "ppp_lte_up")) result = PPP_LTE_PROCESSES_PEER;
    }
    free(pids);
    return result;
}
int ppp_lte_service_cleanup(SCDynamicStoreRef store, struct ppp_lte_service_cleanup *r) {
    if (!store || !r) return -1;
    *r = (struct ppp_lte_service_cleanup){.status = PPP_LTE_CLEANUP_NOT_RUN};
    enum ppp_lte_processes processes = ppp_lte_service_processes();
    (void)ppp_lte_service_scan(store, &r->before);
    if (processes != PPP_LTE_PROCESSES_NONE) {
        r->status = processes == PPP_LTE_PROCESSES_PPPD ? PPP_LTE_CLEANUP_BLOCKED_PPPD :
            processes == PPP_LTE_PROCESSES_PEER ? PPP_LTE_CLEANUP_BLOCKED_PEER :
            PPP_LTE_CLEANUP_BLOCKED_PROCESSES;
        r->after = r->before;
        return 0;
    }
    if (r->before.error) { r->status = PPP_LTE_CLEANUP_STORE_ERROR; r->after = r->before; return 0; }
    for (unsigned i = 0; i < PPP_LTE_ENTITIES; i++) {
        if (!(r->before.present & (1u << i))) continue;
        if (SCDynamicStoreRemoveValue(store, service_keys[i])) r->removed++;
        else r->failed++;
    }
    (void)ppp_lte_service_scan(store, &r->after);
    r->status = r->after.error ? PPP_LTE_CLEANUP_STORE_ERROR : PPP_LTE_CLEANUP_DONE;
    return 0;
}
int ppp_lte_service_clean(const struct ppp_lte_service_cleanup *r) {
    return r && r->status == PPP_LTE_CLEANUP_DONE && !r->after.error && !r->after.present &&
        !r->after.unknown ? 1 : 0;
}
int ppp_lte_service_label(const struct ppp_lte_service_scan *scan, char *buffer, size_t capacity) {
    if (!scan || (!buffer && capacity)) return -1;
    if (scan->error) return snprintf(buffer, capacity, "unproven");
    if (!scan->present && !scan->unknown) return snprintf(buffer, capacity, "none");
    char text[96];
    size_t used = 0;
    text[0] = 0;
    for (unsigned i = 0; i < PPP_LTE_ENTITIES; i++) {
        if (!(scan->present & (1u << i))) continue;
        int added = snprintf(text + used, sizeof(text) - used, "%s%s", used ? "+" : "", entity_labels[i]);
        if (added < 0 || (size_t)added >= sizeof(text) - used) return -1;
        used += (size_t)added;
    }
    if (scan->unknown) {
        int added = snprintf(text + used, sizeof(text) - used, "%sother:%u", used ? "+" : "", scan->unknown);
        if (added < 0 || (size_t)added >= sizeof(text) - used) return -1;
    }
    return snprintf(buffer, capacity, "%s", text);
}
const char *ppp_lte_service_status_label(enum ppp_lte_cleanup_status status) {
    static const char *const labels[] = {"not-run", "done", "blocked-pppd", "blocked-instance",
        "blocked-processes", "store-error"};
    return (unsigned)status < sizeof(labels) / sizeof(labels[0]) ? labels[status] : "unknown";
}
static CFDictionaryRef copy_dictionary(SCDynamicStoreRef store, CFStringRef key, int *error) {
    CFPropertyListRef value = SCDynamicStoreCopyValue(store, key);
    if (!value) {
        /* kSCStatusNoKey is absence; anything else is an unproven read. */
        if (SCError() != kSCStatusNoKey) *error = 1;
        return NULL;
    }
    if (CFGetTypeID(value) != CFDictionaryGetTypeID()) { CFRelease(value); *error = 1; return NULL; }
    return value;
}
static enum ppp_lte_primary_state primary_service(CFDictionaryRef global, int error) {
    if (error) return PPP_LTE_PRIMARY_UNPROVEN;
    if (!global) return PPP_LTE_PRIMARY_NONE;
    CFTypeRef service = CFDictionaryGetValue(global, CFSTR("PrimaryService"));
    if (!service) return PPP_LTE_PRIMARY_NONE;
    if (CFGetTypeID(service) != CFStringGetTypeID()) return PPP_LTE_PRIMARY_UNPROVEN;
    return CFEqual(service, CFSTR(PPP_LTE_UP_SERVICE_ID)) ? PPP_LTE_PRIMARY_THIS : PPP_LTE_PRIMARY_OTHER;
}
int ppp_lte_service_primary(SCDynamicStoreRef store, const char *name, struct ppp_lte_primary *p) {
    if (!store || !p) return -1;
    *p = (struct ppp_lte_primary){.interface = -1};
    int error4 = 0, error6 = 0, error_dns = 0, error_global_dns = 0;
    CFDictionaryRef ipv4 = copy_dictionary(store, CFSTR("State:/Network/Global/IPv4"), &error4);
    CFDictionaryRef ipv6 = copy_dictionary(store, CFSTR("State:/Network/Global/IPv6"), &error6);
    CFDictionaryRef dns = copy_dictionary(store, service_keys[PPP_LTE_ENTITY_DNS], &error_dns);
    CFDictionaryRef global_dns = copy_dictionary(store, CFSTR("State:/Network/Global/DNS"), &error_global_dns);
    p->ipv4 = primary_service(ipv4, error4);
    p->ipv6 = primary_service(ipv6, error6);
    if (name && name[0] && !error4) {
        CFTypeRef current = ipv4 ? CFDictionaryGetValue(ipv4, CFSTR("PrimaryInterface")) : NULL;
        CFStringRef wanted = CFStringCreateWithCString(NULL, name, kCFStringEncodingASCII);
        if (wanted) {
            p->interface = current && CFGetTypeID(current) == CFStringGetTypeID() &&
                CFEqual(current, wanted) ? 1 : 0;
            CFRelease(wanted);
        }
    }
    if (error_dns || error_global_dns) p->dns = PPP_LTE_DNS_UNPROVEN;
    else if (!dns) p->dns = PPP_LTE_DNS_ABSENT;
    else {
        CFTypeRef mine = CFDictionaryGetValue(dns, CFSTR("ServerAddresses"));
        CFTypeRef active = global_dns ? CFDictionaryGetValue(global_dns, CFSTR("ServerAddresses")) : NULL;
        p->dns = mine && active && CFEqual(mine, active) ? PPP_LTE_DNS_ACTIVE : PPP_LTE_DNS_INACTIVE;
    }
    if (ipv4) CFRelease(ipv4);
    if (ipv6) CFRelease(ipv6);
    if (dns) CFRelease(dns);
    if (global_dns) CFRelease(global_dns);
    return 0;
}
const char *ppp_lte_primary_label(enum ppp_lte_primary_state state) {
    static const char *const labels[] = {"unproven", "none", "this-service", "other-service"};
    return (unsigned)state < sizeof(labels) / sizeof(labels[0]) ? labels[state] : "unknown";
}
const char *ppp_lte_dns_label(enum ppp_lte_dns_state state) {
    static const char *const labels[] = {"unproven", "absent", "active", "inactive"};
    return (unsigned)state < sizeof(labels) / sizeof(labels[0]) ? labels[state] : "unknown";
}
