#define _DARWIN_C_SOURCE
#include "ppp_trial_observer.h"
#include <errno.h>
#include <libproc.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc_info.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL trial-observer line=%d\n", __LINE__); } } while (0)
static struct {
    CFDictionaryRef current_services, current_globals;
    int create_error, services_error, globals_error, identity_error, interface_present, interface_error;
    uint64_t seconds;
    unsigned identity_calls, interface_calls, host_calls;
} fake;
static SCDynamicStoreRef fake_create(CFAllocatorRef allocator, CFStringRef name,
                                     SCDynamicStoreCallBack callback, SCDynamicStoreContext *context) {
    (void)allocator; (void)name; (void)callback; (void)context;
    fake.host_calls++;
    return fake.create_error ? NULL : (SCDynamicStoreRef)CFRetain(CFSTR("fake-store-private-marker"));
}
static CFDictionaryRef fake_multiple(SCDynamicStoreRef store, CFArrayRef keys, CFArrayRef patterns) {
    CHECK(store != NULL && (!!keys != !!patterns)); fake.host_calls++;
    CFDictionaryRef value = patterns ? fake.current_services : fake.current_globals;
    if (patterns ? fake.services_error : fake.globals_error) return NULL;
    return value ? CFRetain(value) : NULL;
}
static int fake_proc(int pid, int flavor, uint64_t argument, void *buffer, int size) {
    CHECK(pid == 321 && flavor == PROC_PIDTBSDINFO && argument == 0 && size == (int)sizeof(struct proc_bsdinfo));
    fake.identity_calls++; fake.host_calls++;
    if (fake.identity_error) return 0;
    struct proc_bsdinfo *info = buffer;
    *info = (struct proc_bsdinfo){.pbi_pid = (uint32_t)pid, .pbi_start_tvsec = fake.seconds};
    return sizeof(*info);
}
static unsigned fake_index(const char *name) {
    CHECK(!strcmp(name, "private-interface-marker"));
    fake.interface_calls++; fake.host_calls++;
    if (fake.interface_present) return 9;
    errno = fake.interface_error;
    return 0;
}
#define SCDynamicStoreCreate fake_create
#define SCDynamicStoreCopyMultiple fake_multiple
#define proc_pidinfo fake_proc
#define if_nametoindex fake_index
#include "../src/ppp_trial_observer.c"
#undef if_nametoindex
#undef proc_pidinfo
#undef SCDynamicStoreCopyMultiple
#undef SCDynamicStoreCreate

static CFMutableDictionaryRef dictionary(void) {
    return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}
enum { NO_PID = -1, TEXT_PID = -2 }; /* omit the pid key / publish pid as a string */
static void set_service(CFMutableDictionaryRef d, CFStringRef key, int64_t pid, int name) {
    CFMutableDictionaryRef value = dictionary();
    if (pid == TEXT_PID) CFDictionarySetValue(value, CFSTR("pid"), CFSTR("321"));
    else if (pid >= 0) {
        CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt64Type, &pid);
        CFDictionarySetValue(value, CFSTR("pid"), number); CFRelease(number);
    }
    if (name) CFDictionarySetValue(value, CFSTR("InterfaceName"), CFSTR("private-interface-marker"));
    CFDictionarySetValue(d, key, value); CFRelease(value);
}
static void label(struct ppp_trial_observer *o, const char *expected) {
    char text[2048];
    int n = ppp_trial_observer_diagnostic(o, text, sizeof(text));
    CHECK(n > 0 && (size_t)n < sizeof(text) && strstr(text, expected));
    const char *private[] = {"private-service-marker", "private-old-service-marker", "private-second-service-marker",
                             "private-interface-marker", "private-address-marker", "321", "999"};
    for (unsigned i = 0; i < sizeof(private) / sizeof(private[0]); i++) CHECK(!strstr(text, private[i]));
    CHECK(strstr(text, "kernel-routes=unobserved descendants=unproven"));
}
int main(void) {
    for (unsigned mode = 0; mode < 18; mode++) {
        fake = (__typeof__(fake)){.seconds = 100, .interface_error = ENXIO};
        CFMutableDictionaryRef baseline = dictionary(), current = dictionary(), global = dictionary(), after = dictionary();
        fake.current_services = baseline; fake.current_globals = global;
        if (mode == 0) fake.create_error = 1;
        if (mode == 1) fake.services_error = 1;
        if (mode == 2) fake.globals_error = 1;
        if (mode == 3) fake.identity_error = 1;
        if (mode == 4) set_service(baseline, CFSTR("private-service-marker"), 321, 1);
        if (mode == 16) set_service(baseline, CFSTR("private-old-service-marker"), 999, 1);
        struct ppp_trial_observer o;
        ppp_trial_observer_begin(&o);
        ppp_trial_observer_child(&o, 321);
        CHECK(fake.identity_calls == 1);
        set_service(current, CFSTR("private-service-marker"), mode == 5 ? 999 : 321, mode != 6);
        if (mode == 7) set_service(current, CFSTR("private-second-service-marker"), 321, 1);
        if (mode == 15) set_service(current, CFSTR("private-second-service-marker"), 999, 1);
        if (mode == 8) fake.seconds++;
        fake.current_services = current; fake.interface_present = mode != 17;
        ppp_trial_observer_sample(&o);
        if (mode == 1) CHECK(o.ambiguous && !o.attributed); /* no baseline snapshot */
        fake.interface_present = 0;
        if (mode >= 9 && mode <= 14) CHECK(o.attributed);
        if (mode == 9) {
            set_service(current, CFSTR("private-second-service-marker"), 321, 1);
            ppp_trial_observer_sample(&o);
        }
        if (mode == 10) fake.services_error = 1;
        if (mode == 11) fake.interface_present = 1;
        if (mode == 12) CFDictionarySetValue(after, global_keys[0], CFSTR("private-address-marker"));
        if (mode == 13) fake.globals_error = 1;
        if (mode != 14) fake.current_services = baseline;
        fake.current_globals = after;
        unsigned identities = fake.identity_calls;
        ppp_trial_observer_finish(&o);
        CHECK(fake.identity_calls == identities); /* no post-reap numeric PID lookup */
        ppp_trial_observer_sample(&o);
        CHECK(fake.identity_calls == identities);
        int result = ppp_trial_observer_result(&o);
        if (mode <= 10 || mode == 13 || mode >= 15) CHECK(result == -1);
        else CHECK(result == 1);
        if ((mode <= 9 && mode != 2) || mode >= 15) label(&o, "attribution=unproven");
        if (mode == 2) {
            label(&o, "attribution=unique-observed");
            label(&o, "sc-global-ipv4=unknown sc-global-ipv6=unknown sc-global-dns=unknown");
        }
        if (mode == 11) label(&o, "kernel-interface=present");
        if (mode == 12) label(&o, "sc-global-ipv4=changed");
        if (mode == 14) label(&o, "service-state=present");
        ppp_trial_observer_dispose(&o); ppp_trial_observer_dispose(&o);
        CFRelease(baseline); CFRelease(current); CFRelease(global); CFRelease(after);
    }
    /* Full synthetic unique attribution + retained key removal, no host access. */
    fake = (__typeof__(fake)){.seconds = 100, .interface_error = ENXIO};
    CFMutableDictionaryRef empty = dictionary(), current = dictionary();
    fake.current_services = empty; fake.current_globals = empty;
    struct ppp_trial_observer o;
    ppp_trial_observer_begin(&o); ppp_trial_observer_child(&o, 321);
    set_service(current, CFSTR("private-service-marker"), 321, 1);
    fake.current_services = current; fake.interface_present = 1; ppp_trial_observer_sample(&o);
    fake.current_services = empty; fake.interface_present = 0; ppp_trial_observer_finish(&o);
    CHECK(ppp_trial_observer_result(&o) == 0);
    label(&o, "attribution=unique-observed service-state=removed kernel-interface=removed");
    label(&o, "sc-global-ipv4=unchanged sc-global-ipv6=unchanged sc-global-dns=unchanged");
    ppp_trial_observer_dispose(&o); CFRelease(empty); CFRelease(current);
    const int lookup_errors[] = {EIO, ENOBUFS, EPERM, 0};
    for (unsigned i = 0; i < sizeof(lookup_errors) / sizeof(lookup_errors[0]); i++) {
        fake = (__typeof__(fake)){.seconds = 100, .interface_error = lookup_errors[i]};
        empty = dictionary(); current = dictionary();
        fake.current_services = empty; fake.current_globals = empty;
        ppp_trial_observer_begin(&o); ppp_trial_observer_child(&o, 321);
        set_service(current, CFSTR("private-service-marker"), 321, 1);
        fake.current_services = current; fake.interface_present = 1;
        ppp_trial_observer_sample(&o);
        CHECK(o.attributed);
        fake.current_services = empty; fake.interface_present = 0;
        errno = ENXIO; /* stale absence must be cleared before the lookup */
        ppp_trial_observer_finish(&o);
        CHECK(o.service_removed == 1 && o.interface_removed == 0);
        CHECK(ppp_trial_observer_result(&o) == -1);
        label(&o, "attribution=unique-observed service-state=removed kernel-interface=unproven");
        ppp_trial_observer_dispose(&o); CFRelease(empty); CFRelease(current);
    }
    /* Historical attribution is not current readiness: service disappearance,
     * interface disappearance, changed index, lookup errors and stale PID. */
    for (unsigned mode = 0; mode < 5; mode++) {
        fake = (__typeof__(fake)){.seconds = 100, .interface_error = ENXIO};
        empty = dictionary(); current = dictionary();
        fake.current_services = empty; fake.current_globals = empty;
        ppp_trial_observer_begin(&o); ppp_trial_observer_child(&o, 321);
        set_service(current, CFSTR("private-service-marker"), 321, 1);
        fake.current_services = current; fake.interface_present = 1;
        ppp_trial_observer_sample(&o);
        struct ppp_trial_interface copy;
        CHECK(ppp_trial_observer_current(&o, &copy) == 0 && copy.index == 9);
        copy.name[0] = 'x'; copy.index = 123;
        CHECK(o.interface_index == 9 && o.interface_name[0] == 'p');
        if (mode == 0) fake.current_services = empty;
        if (mode == 1) fake.interface_present = 0;
        if (mode == 2) o.interface_index = 10;
        if (mode == 3) fake.services_error = 1;
        if (mode == 4) fake.seconds++;
        ppp_trial_observer_sample(&o);
        CHECK(ppp_trial_observer_current(&o, &copy) == -1 && !o.current_valid);
        ppp_trial_observer_finish(&o);
        unsigned identities = fake.identity_calls;
        CHECK(ppp_trial_observer_current(&o, &copy) == -1);
        CHECK(fake.identity_calls == identities);
        ppp_trial_observer_dispose(&o); CFRelease(empty); CFRelease(current);
    }
    /* Published PPP entry without pid (pppd pin 06a73c4 sys-MacOSX.c:803 runs
     * before :815): 0 sole new, 1 two new, 2 new + other-pid new, 3 baseline
     * InterfaceName reused, 4 pid-less baseline key kept, 5 string pid,
     * 6 second pid-less key after attribution, 7 the attributed key later
     * carries another pid, 8 the attributed key is replaced by another sole
     * new key, 9 the attributed key's value stops being a dictionary. */
    for (unsigned mode = 0; mode < 10; mode++) {
        fake = (__typeof__(fake)){.seconds = 100, .interface_error = ENXIO};
        CFMutableDictionaryRef before = dictionary(), now = dictionary();
        empty = dictionary();
        if (mode == 3) set_service(before, CFSTR("private-old-service-marker"), NO_PID, 1);
        if (mode == 4) {
            set_service(before, CFSTR("private-old-service-marker"), NO_PID, 0);
            set_service(now, CFSTR("private-old-service-marker"), NO_PID, 0);
        }
        fake.current_services = before; fake.current_globals = empty;
        ppp_trial_observer_begin(&o); ppp_trial_observer_child(&o, 321);
        set_service(now, CFSTR("private-service-marker"), mode == 5 ? TEXT_PID : NO_PID, 1);
        if (mode == 1) set_service(now, CFSTR("private-second-service-marker"), NO_PID, 1);
        if (mode == 2) set_service(now, CFSTR("private-second-service-marker"), 999, 1);
        fake.current_services = now; fake.interface_present = 1;
        ppp_trial_observer_sample(&o);
        int attributed = mode == 0 || mode == 4 || mode >= 6;
        struct ppp_trial_interface copy = {{0}, 0};
        CHECK(!!o.attributed == attributed && !!o.current_valid == attributed);
        CHECK((ppp_trial_observer_current(&o, &copy) == 0) == attributed);
        if (attributed) CHECK(copy.index == 9 && !strcmp(copy.name, "private-interface-marker"));
        else CHECK(!fake.interface_calls); /* rejected before any kernel lookup */
        CHECK(o.ambiguous == (mode >= 1 && mode <= 3));
        label(&o, attributed ? "attribution=unique-observed" : "attribution=unproven");
        if (mode >= 6) {
            if (mode == 6) set_service(now, CFSTR("private-second-service-marker"), NO_PID, 1);
            if (mode == 7) set_service(now, CFSTR("private-service-marker"), 999, 1);
            if (mode == 8) {
                CFDictionaryRemoveValue(now, CFSTR("private-service-marker"));
                set_service(now, CFSTR("private-second-service-marker"), NO_PID, 1);
            }
            if (mode == 9) CFDictionarySetValue(now, CFSTR("private-service-marker"), CFSTR("private-address-marker"));
            unsigned lookups = fake.interface_calls;
            ppp_trial_observer_sample(&o);
            CHECK(o.ambiguous && !o.attributed && ppp_trial_observer_current(&o, &copy) == -1);
            CHECK(fake.interface_calls == lookups); /* no kernel lookup for a key that is not ours */
            attributed = 0;
        }
        fake.current_services = before; fake.interface_present = 0;
        unsigned identities = fake.identity_calls;
        ppp_trial_observer_finish(&o);
        CHECK(fake.identity_calls == identities);
        CHECK(ppp_trial_observer_result(&o) == (attributed ? 0 : -1));
        label(&o, attributed ? "attribution=unique-observed service-state=removed kernel-interface=removed"
                             : "attribution=unproven service-state=unproven kernel-interface=unproven");
        ppp_trial_observer_dispose(&o); CFRelease(before); CFRelease(now); CFRelease(empty);
    }
    printf("ppp_trial_observer fake-only checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
