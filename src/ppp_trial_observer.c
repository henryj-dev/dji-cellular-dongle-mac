#define _DARWIN_C_SOURCE
#include "ppp_trial_observer.h"
#include <errno.h>
#include <libproc.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc_info.h>

static CFDictionaryRef services(SCDynamicStoreRef store) {
    const void *pattern = CFSTR("State:/Network/Service/[^/]+/PPP");
    CFArrayRef patterns = CFArrayCreate(NULL, &pattern, 1, &kCFTypeArrayCallBacks);
    if (!patterns) return NULL;
    CFDictionaryRef values = SCDynamicStoreCopyMultiple(store, NULL, patterns);
    CFRelease(patterns);
    return values;
}
static const CFStringRef global_keys[] = {
    CFSTR("State:/Network/Global/IPv4"), CFSTR("State:/Network/Global/IPv6"),
    CFSTR("State:/Network/Global/DNS")
};
static CFDictionaryRef globals(SCDynamicStoreRef store) {
    CFArrayRef keys = CFArrayCreate(NULL, (const void **)global_keys, 3, &kCFTypeArrayCallBacks);
    if (!keys) return NULL;
    CFDictionaryRef values = SCDynamicStoreCopyMultiple(store, keys, NULL);
    CFRelease(keys);
    return values;
}
static int process_identity(pid_t pid, uint64_t *sec, uint64_t *usec) {
    struct proc_bsdinfo info = {0};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != (int)sizeof(info) ||
        info.pbi_pid != (uint32_t)pid || (!info.pbi_start_tvsec && !info.pbi_start_tvusec)) return -1;
    *sec = info.pbi_start_tvsec; *usec = info.pbi_start_tvusec;
    return 0;
}
void ppp_trial_observer_begin(struct ppp_trial_observer *o) {
    if (!o) return;
    *o = (struct ppp_trial_observer){0};
    o->store = SCDynamicStoreCreate(NULL, CFSTR("PPP local auth trial"), NULL, NULL);
    if (!o->store) { o->snapshot_error = 1; return; }
    o->before_services = services(o->store);
    o->before_globals = globals(o->store);
    if (!o->before_services || !o->before_globals) o->snapshot_error = 1;
}
void ppp_trial_observer_child(struct ppp_trial_observer *o, pid_t pid) {
    if (!o || pid <= 0 || o->pid || o->finished) return;
    o->pid = pid;
    o->identity = !process_identity(pid, &o->start_sec, &o->start_usec);
}
struct candidates {
    struct ppp_trial_observer *observer;
    unsigned fresh, reused;
    CFStringRef key;
    CFDictionaryRef value;
};
/* Apple pppd pin 06a73c4 sys-MacOSX.c sys_init() publishes "pid" (line 803)
 * before publish_dict exists (line 815), so a live PPP entry normally has no
 * pid. Every key absent from the baseline counts as new, whatever its value;
 * only a sole new key whose pid is absent or equal to ours can be attributed.
 * A baseline key carrying our pid stays ambiguous (possible PID reuse). */
static void candidate(const void *key, const void *value, void *context) {
    struct candidates *c = context;
    if (CFGetTypeID(key) != CFStringGetTypeID()) return;
    int dict = CFGetTypeID(value) == CFDictionaryGetTypeID();
    CFTypeRef number = dict ? CFDictionaryGetValue(value, CFSTR("pid")) : NULL;
    int64_t pid = 0;
    int ours = number && CFGetTypeID(number) == CFNumberGetTypeID() &&
        CFNumberGetValue(number, kCFNumberSInt64Type, &pid) && pid == c->observer->pid;
    if (CFDictionaryContainsKey(c->observer->before_services, key)) { if (ours) c->reused++; return; }
    c->fresh++; c->key = key;
    c->value = dict && (!number || ours) ? value : NULL;
}
struct interface_matches { CFStringRef name; unsigned count; };
static void interface_match(const void *key, const void *value, void *context) {
    (void)key;
    struct interface_matches *matches = context;
    if (CFGetTypeID(value) != CFDictionaryGetTypeID()) return;
    CFTypeRef name = CFDictionaryGetValue(value, CFSTR("InterfaceName"));
    if (name && CFGetTypeID(name) == CFStringGetTypeID() && CFEqual(name, matches->name)) matches->count++;
}
void ppp_trial_observer_sample(struct ppp_trial_observer *o) {
    if (!o) return;
    o->current_valid = 0;
    if (!o->store || !o->identity || o->ambiguous || o->finished) return;
    uint64_t sec = 0, usec = 0;
    if (process_identity(o->pid, &sec, &usec) || sec != o->start_sec || usec != o->start_usec) {
        o->identity = 0; o->attributed = 0; return;
    }
    if (!o->before_services) { o->ambiguous = 1; o->attributed = 0; return; }
    CFDictionaryRef current = services(o->store);
    if (!current) { o->snapshot_error = 1; return; }
    struct candidates c = {.observer = o};
    CFDictionaryApplyFunction(current, candidate, &c);
    /* Once attributed, the same key later carrying another pid (or no longer a
     * dictionary) is not ours any more: the pid rule applies on every sample. */
    if (c.reused || c.fresh > 1 || (c.fresh == 1 && o->service && !CFEqual(o->service, c.key)) ||
        (c.fresh == 1 && o->service && !c.value)) {
        o->ambiguous = 1; o->attributed = 0;
    } else if (c.value) {
        if (!o->service) o->service = CFRetain(c.key);
        CFTypeRef name = CFDictionaryGetValue(c.value, CFSTR("InterfaceName"));
        char text[sizeof(o->interface_name)] = {0};
        if (name && CFGetTypeID(name) == CFStringGetTypeID() &&
            CFStringGetCString(name, text, sizeof(text), kCFStringEncodingASCII) && text[0]) {
            struct interface_matches present = {.name = name}, before = {.name = name};
            CFDictionaryApplyFunction(current, interface_match, &present);
            CFDictionaryApplyFunction(o->before_services, interface_match, &before);
            if (present.count != 1 || before.count ||
                (o->interface_name[0] && strcmp(o->interface_name, text))) {
                o->ambiguous = 1; o->attributed = 0;
            } else {
                unsigned index = if_nametoindex(text);
                if (index && (!o->interface_index || o->interface_index == index)) {
                    memcpy(o->interface_name, text, sizeof(text)); o->attributed = 1;
                    o->interface_index = index; o->current_valid = 1;
                } else if (index) o->ambiguous = 1;
            }
        }
    }
    CFRelease(current);
}
int ppp_trial_observer_current(const struct ppp_trial_observer *o, struct ppp_trial_interface *i) {
    if (!o || !i || !o->current_valid || !o->identity || o->ambiguous ||
        o->snapshot_error || o->finished || !o->interface_index) return -1;
    memcpy(i->name, o->interface_name, sizeof(i->name));
    i->index = o->interface_index;
    return 0;
}
void ppp_trial_observer_finish(struct ppp_trial_observer *o) {
    if (!o || o->finished) return;
    o->finished = 1;
    if (!o->store) return;
    o->after_globals = globals(o->store);
    if (!o->after_globals) o->snapshot_error = 1;
    /* Do not query this PID after reap: only retained service identity is used. */
    if (o->attributed && o->service && !o->ambiguous && o->identity) {
        CFDictionaryRef current = services(o->store);
        if (current) {
            o->service_removed = CFDictionaryContainsKey(current, o->service) ? -1 : 1;
            CFRelease(current);
        }
        /* Darwin Libinfo if_nametoindex sets ENXIO for a missing name;
         * getifaddrs failures also return zero, retaining their errno. Never
         * turn an unspecified zero or another lookup failure into removal. */
        errno = 0;
        unsigned index = if_nametoindex(o->interface_name);
        int error = errno;
        o->interface_removed = index ? -1 : error == ENXIO ? 1 : 0;
    }
}
static int global_change(const struct ppp_trial_observer *o, unsigned index) {
    if (!o->before_globals || !o->after_globals) return -1;
    CFTypeRef a = CFDictionaryGetValue(o->before_globals, global_keys[index]);
    CFTypeRef b = CFDictionaryGetValue(o->after_globals, global_keys[index]);
    return a && b ? !CFEqual(a, b) : a || b ? 1 : 0;
}
static const char *change_label(int value) { return value < 0 ? "unknown" : value ? "changed" : "unchanged"; }
static const char *removed_label(int value) { return value < 0 ? "present" : value ? "removed" : "unproven"; }
int ppp_trial_observer_result(const struct ppp_trial_observer *o) {
    if (!o) return -1;
    for (unsigned i = 0; i < 3; i++) if (global_change(o, i) == 1) return 1;
    if (o->service_removed < 0 || o->interface_removed < 0) return 1;
    if (!o->finished || o->snapshot_error || !o->attributed || !o->identity || o->ambiguous ||
        o->service_removed != 1 || o->interface_removed != 1) return -1;
    return 0;
}
int ppp_trial_observer_diagnostic(const struct ppp_trial_observer *o, char *buffer, size_t capacity) {
    if (!o || (!buffer && capacity)) return -1;
    return snprintf(buffer, capacity, "attribution=%s service-state=%s kernel-interface=%s"
        " sc-global-ipv4=%s sc-global-ipv6=%s sc-global-dns=%s"
        " sc-scope=dynamic-store-global-only kernel-routes=unobserved descendants=unproven next-run=%s",
        o->attributed && o->identity && !o->ambiguous ? "unique-observed" : "unproven",
        removed_label(o->service_removed), removed_label(o->interface_removed),
        change_label(global_change(o, 0)), change_label(global_change(o, 1)), change_label(global_change(o, 2)),
        ppp_trial_observer_result(o) == 1 ? "blocked-until-resolved" : "separate-review-required");
}
void ppp_trial_observer_dispose(struct ppp_trial_observer *o) {
    if (!o) return;
    if (o->before_services) CFRelease(o->before_services);
    if (o->before_globals) CFRelease(o->before_globals);
    if (o->after_globals) CFRelease(o->after_globals);
    if (o->service) CFRelease(o->service);
    if (o->store) CFRelease(o->store);
    o->before_services = o->before_globals = o->after_globals = NULL;
    o->service = NULL; o->store = NULL;
}
