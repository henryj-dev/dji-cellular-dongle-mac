#ifndef DJI_PPP_LTE_SERVICE_H
#define DJI_PPP_LTE_SERVICE_H

#include <SystemConfiguration/SystemConfiguration.h>
#include <stddef.h>

/* State keys of the fixed connection-keeping service ID only. Every key this
 * module may remove is a compile-time constant; nothing read from the store is
 * ever turned into a key. Setup:, Global and other services are never written. */
enum ppp_lte_entity {
    PPP_LTE_ENTITY_PPP, PPP_LTE_ENTITY_IPV4, PPP_LTE_ENTITY_IPV6, PPP_LTE_ENTITY_DNS,
    PPP_LTE_ENTITY_PROXIES, PPP_LTE_ENTITY_INTERFACE, PPP_LTE_ENTITY_SMB, PPP_LTE_ENTITIES
};
/* Borrowed constant key, NULL for an out-of-range entity. */
CFStringRef ppp_lte_service_key(enum ppp_lte_entity entity);
struct ppp_lte_service_scan {
    unsigned present;   /* bit i set: entity i present */
    unsigned unknown;   /* other keys under the fixed ID; counted, never removed */
    int error;          /* the key list could not be read */
};
/* Read-only. 0 with a filled scan, -1 on invalid input (scan->error on read failure). */
int ppp_lte_service_scan(SCDynamicStoreRef store, struct ppp_lte_service_scan *scan);

enum ppp_lte_processes {
    PPP_LTE_PROCESSES_NONE, PPP_LTE_PROCESSES_PPPD, PPP_LTE_PROCESSES_PEER,
    PPP_LTE_PROCESSES_UNKNOWN
};
/* Any live /usr/sbin/pppd, or another ppp_lte_up than this process, blocks
 * cleanup. An unreadable process list is UNKNOWN (fail closed). */
enum ppp_lte_processes ppp_lte_service_processes(void);

enum ppp_lte_cleanup_status {
    PPP_LTE_CLEANUP_NOT_RUN, PPP_LTE_CLEANUP_DONE, PPP_LTE_CLEANUP_BLOCKED_PPPD,
    PPP_LTE_CLEANUP_BLOCKED_PEER, PPP_LTE_CLEANUP_BLOCKED_PROCESSES, PPP_LTE_CLEANUP_STORE_ERROR
};
struct ppp_lte_service_cleanup {
    enum ppp_lte_cleanup_status status;
    struct ppp_lte_service_scan before, after;
    unsigned removed, failed;
};
/* Process check, scan, RemoveValue for each present fixed key, scan again.
 * Blocked runs still scan (read-only) but remove nothing. Idempotent. */
int ppp_lte_service_cleanup(SCDynamicStoreRef store, struct ppp_lte_service_cleanup *result);
/* 1 only when the cleanup ran and the final scan read no fixed or unknown key. */
int ppp_lte_service_clean(const struct ppp_lte_service_cleanup *result);
/* Fixed entity labels joined by '+', "none", or "unproven" on a scan error;
 * unknown keys add "other:N". Returns the snprintf length or -1. */
int ppp_lte_service_label(const struct ppp_lte_service_scan *scan, char *buffer, size_t capacity);
const char *ppp_lte_service_status_label(enum ppp_lte_cleanup_status status);

/* Read-only primary probe; fixed states only, never names or addresses. */
enum ppp_lte_primary_state {
    PPP_LTE_PRIMARY_UNPROVEN, PPP_LTE_PRIMARY_NONE, PPP_LTE_PRIMARY_THIS, PPP_LTE_PRIMARY_OTHER
};
enum ppp_lte_dns_state {
    PPP_LTE_DNS_UNPROVEN, PPP_LTE_DNS_ABSENT, PPP_LTE_DNS_ACTIVE, PPP_LTE_DNS_INACTIVE
};
struct ppp_lte_primary {
    enum ppp_lte_primary_state ipv4, ipv6;
    /* Global IPv4 PrimaryInterface: 1 equals the given name, 0 differs or is
     * absent, -1 unproven (no name given or read failure). */
    int interface;
    /* Service DNS absent, present and equal to the global DNS servers, or present
     * but not (yet) the global set. */
    enum ppp_lte_dns_state dns;
};
int ppp_lte_service_primary(SCDynamicStoreRef store, const char *interface_name, struct ppp_lte_primary *primary);
const char *ppp_lte_primary_label(enum ppp_lte_primary_state state);
const char *ppp_lte_dns_label(enum ppp_lte_dns_state state);

#endif
