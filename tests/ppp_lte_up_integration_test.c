#define _DARWIN_C_SOURCE
#include "discovery_internal.h"
#include "discovery.h"
#include "ppp_lte_link.h"
#include "ppp_lte_network.h"
#include "ppp_lte_service.h"
#include "ppp_runner.h"
#include "ppp_trial_preflight.h"
#include "status_probe.h"
#include "usb_recover.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <net/if.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Real up + link + service code together. Only OS boundaries are fake: a
 * dictionary store with a tiny configd model (the fixed service is primary
 * while its IPv4 key exists), a process table, an interface table and the
 * runner/observer/discovery. */
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL lte-up-integration scenario=%d line=%d\n", (int)f.scenario, __LINE__); } } while (0)
#define ID PPP_LTE_UP_SERVICE_ID
#define FIXED(entity) CFSTR("State:/Network/Service/" ID "/" entity)
enum scenario { USER_STOP, UNPLUG, STALE_LEFTOVERS, VPN_PPPD, SCENARIOS };
static struct {
    enum scenario scenario;
    CFMutableDictionaryRef store;
    unsigned removes, slices, stores, store_releases;
    int pppd_listed, address_up, stop, closed, connected;
    uint64_t now, start_ms;
    struct ppp_runner_status status;
    struct ppp_session_observation child;
    struct ifaddrs item;
    struct sockaddr_in local, peer;
} f;
static int last_error;
/* configd model: Global IPv4/IPv6 follow the fixed service's keys, else Wi-Fi. */
static CFDictionaryRef global(int family) {
    int ours = CFDictionaryContainsKey(f.store, family == 4 ? FIXED("IPv4") : FIXED("IPv6"));
    const void *keys[] = {CFSTR("PrimaryService"), CFSTR("PrimaryInterface")};
    const void *values[] = {ours ? CFSTR(ID) : CFSTR("WIFI-SERVICE"), ours ? CFSTR("ppp-private-marker") : CFSTR("en0")};
    return CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}
static SCDynamicStoreRef fake_create(CFAllocatorRef a, CFStringRef n, SCDynamicStoreCallBack c, SCDynamicStoreContext *x) {
    (void)a; (void)n; (void)c; (void)x; f.stores++;
    return (SCDynamicStoreRef)CFRetain(CFSTR("fake-store"));
}
static CFArrayRef fake_key_list(SCDynamicStoreRef store, CFStringRef pattern) {
    (void)store; CHECK(CFStringHasPrefix(pattern, CFSTR("^State:/Network/Service/" ID "/")));
    CFIndex count = CFDictionaryGetCount(f.store);
    const void **keys = calloc((size_t)count + 1, sizeof(*keys));
    CFDictionaryGetKeysAndValues(f.store, keys, NULL);
    CFMutableArrayRef list = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (CFIndex i = 0; i < count; i++)
        if (CFStringHasPrefix(keys[i], CFSTR("State:/Network/Service/" ID "/"))) CFArrayAppendValue(list, keys[i]);
    free(keys);
    return list;
}
static Boolean fake_remove(SCDynamicStoreRef store, CFStringRef key) {
    (void)store; f.removes++;
    CHECK(CFStringHasPrefix(key, CFSTR("State:/Network/Service/" ID "/")));
    if (!CFDictionaryContainsKey(f.store, key)) { last_error = kSCStatusNoKey; return false; }
    CFDictionaryRemoveValue(f.store, key); return true;
}
static CFPropertyListRef fake_copy(SCDynamicStoreRef store, CFStringRef key) {
    (void)store;
    if (CFEqual(key, CFSTR("State:/Network/Global/IPv4"))) return global(4);
    if (CFEqual(key, CFSTR("State:/Network/Global/IPv6"))) return global(6);
    CFTypeRef value = CFDictionaryGetValue(f.store, key);
    if (!value) { last_error = kSCStatusNoKey; return NULL; }
    return CFRetain(value);
}
static int fake_error(void) { return last_error; }
static int fake_listallpids(void *buffer, int size) {
    int count = f.pppd_listed ? 3 : 2;
    if (!buffer) return count;
    pid_t pids[] = {1, 4242, 98765};
    CHECK(size / (int)sizeof(pid_t) > count);
    memcpy(buffer, pids, (size_t)count * sizeof(pid_t));
    return count;
}
static int fake_pidpath(int pid, void *buffer, uint32_t size) {
    const char *path = pid == 1 ? "/sbin/launchd" : pid == 4242 ? "/private-build/ppp_lte_up" :
        pid == 98765 ? "/usr/sbin/pppd" : NULL;
    if (!path) { errno = ESRCH; return 0; }
    (void)snprintf(buffer, size, "%s", path); return (int)strlen(path);
}
static pid_t fake_getpid(void) { return 4242; }
static int fake_getifaddrs(struct ifaddrs **list) {
    f.local = (struct sockaddr_in){.sin_len = sizeof(struct sockaddr_in), .sin_family = AF_INET};
    f.peer = f.local;
    (void)inet_pton(AF_INET, "10.20.30.40", &f.local.sin_addr);
    (void)inet_pton(AF_INET, "10.64.64.64", &f.peer.sin_addr);
    f.item = (struct ifaddrs){.ifa_name = "ppp-private-marker", .ifa_flags = IFF_UP | IFF_POINTOPOINT,
        .ifa_addr = (struct sockaddr *)&f.local, .ifa_dstaddr = (struct sockaddr *)&f.peer};
    *list = f.address_up ? &f.item : NULL;
    return 0;
}
static void fake_freeifaddrs(struct ifaddrs *list) { (void)list; }
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
int ppp_lte_endpoint_allowed(struct in_addr address) {
    uint32_t host = ntohl(address.s_addr);
    return host && host != 0xffffffffu && (host >> 24) != 127 &&
        (host & 0xffff0000u) != 0xa9fe0000u && (host >> 28) < 0xe ? 0 : -1;
}
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#include "../src/ppp_lte_link.c"
#undef freeifaddrs
#undef getifaddrs

static uid_t fake_uid(void *context) { (void)context; return 0; }
static int fake_home(void *context, uid_t uid, const char **home) { (void)context; (void)uid; *home = "/fixture"; return 0; }
static int fake_metadata(void *context, const char *path, struct stat *st) {
    (void)context; *st = (struct stat){.st_mode = !strcmp(path, "/usr/sbin/pppd") ? S_IFREG | 0755 : S_IFDIR | 0755};
    return 0;
}
static int fake_identity(void *context) { (void)context; return 0; }
static const char *fake_executable(const struct trial_backend *b) { (void)b; return "clear"; }
static int fake_preflight(void *context, struct trial_preflight *p) {
    (void)context;
    *p = (struct trial_preflight){.status = TRIAL_PREFLIGHT_COMPLETE, .metadata.clear = 1,
        .backend = {&f, fake_uid, fake_home, fake_metadata}};
    return 0;
}
static int fake_clock(clockid_t clock, struct timespec *ts) {
    (void)clock; uint64_t value = f.now += 10;
    ts->tv_sec = (time_t)(value / 1000); ts->tv_nsec = (long)(value % 1000) * 1000000; return 0;
}
static int fake_sleep(const struct timespec *delay, struct timespec *remaining) { (void)delay; (void)remaining; return 0; }
static int fake_sigaction(int number, const struct sigaction *action, struct sigaction *old) {
    (void)number; (void)action; if (old) *old = (struct sigaction){.sa_handler = SIG_DFL}; return 0;
}
static void fake_observer_begin(struct ppp_trial_observer *o) {
    memset(o, 0, sizeof(*o));
    /* The baseline comes after the pre-start cleanup: no fixed key is left. */
    CHECK(!CFDictionaryContainsKey(f.store, FIXED("PPP")) && !CFDictionaryContainsKey(f.store, FIXED("IPv6")));
}
static void fake_observer_child(struct ppp_trial_observer *o, pid_t pid) { o->identity = 1; o->pid = pid; f.start_ms = f.now; }
static void fake_observer_sample(struct ppp_trial_observer *o) {
    CHECK(!f.child.reaped);
    o->current_valid = CFDictionaryContainsKey(f.store, FIXED("PPP"));
    o->attributed = o->current_valid;
    if (o->current_valid) o->service = ppp_lte_service_key(PPP_LTE_ENTITY_PPP);
}
static int fake_observer_current(const struct ppp_trial_observer *o, struct ppp_trial_interface *i) {
    if (!o->current_valid) return -1;
    memset(i, 0, sizeof(*i)); strcpy(i->name, "ppp-private-marker"); i->index = 7; return 0;
}
static void fake_observer_finish(struct ppp_trial_observer *o) { (void)o; }
static int fake_observer_result(const struct ppp_trial_observer *o) { (void)o; return 0; }
static int fake_observer_diagnostic(const struct ppp_trial_observer *o, char *b, size_t n) { (void)o; return snprintf(b, n, "attribution=fixture"); }
static void fake_observer_dispose(struct ppp_trial_observer *o) { (void)o; }
static int fake_session_start(const struct ppp_session_request *r, const struct ppp_metadata_backend *b, struct ppp_session **s) {
    (void)r; (void)b; *s = (struct ppp_session *)&f; return 0;
}
static int fake_session_stop(struct ppp_session *s) { (void)s; return 0; }
static int fake_session_step(struct ppp_session *s) { (void)s; return 1; }
static int fake_session_release(struct ppp_session **s) { *s = NULL; return 0; }
static int fake_runner_begin(struct ppp_session **s, struct ppp_runner **r) { *s = NULL; *r = (struct ppp_runner *)&f; return 0; }
static int fake_runner_observe(struct ppp_runner *r, struct ppp_runner_status *s, struct ppp_session_observation *o) {
    (void)r; *s = f.status; *o = f.child; return 0;
}
static int fake_runner_restore(struct ppp_runner *r, struct ppp_runner_status *s) { (void)r; *s = f.status; return 0; }
static int fake_runner_release(struct ppp_runner **r) { *r = NULL; return 0; }
/* pppd's own publishing: after IPCP its keys appear; TERM removes all but
 * the IPv6 dummy primary (pin bug); KILL leaves every key. */
static void pppd_publish(void) {
    const CFStringRef keys[] = {FIXED("PPP"), FIXED("IPv4"), FIXED("IPv6"), FIXED("DNS"), FIXED("Interface"),
        FIXED("Proxies"), FIXED("SMB")};
    for (unsigned i = 0; i < 7; i++) CFDictionarySetValue(f.store, keys[i], CFSTR("v"));
    f.address_up = 1;
}
static void pppd_term_exit(void) {
    CFDictionaryRemoveValue(f.store, FIXED("PPP")); CFDictionaryRemoveValue(f.store, FIXED("IPv4"));
    CFDictionaryRemoveValue(f.store, FIXED("DNS")); CFDictionaryRemoveValue(f.store, FIXED("Interface"));
    CFDictionaryRemoveValue(f.store, FIXED("Proxies")); CFDictionaryRemoveValue(f.store, FIXED("SMB"));
    f.address_up = 0; f.pppd_listed = 0;
    f.child.reaped = 1; f.child.exited = 1; f.child.exit_code = 5; f.child.term_signals = 1;
}
static int fake_runner_slice(struct ppp_runner *r, const volatile sig_atomic_t *stop, const uint32_t *wait,
                             struct ppp_runner_status *s) {
    (void)r; (void)wait; f.slices++;
    if (f.slices > 100000) { fprintf(stderr, "runaway\n"); exit(1); }
    if (f.slices == 1) {
        f.child = (struct ppp_session_observation){.pid = 98765, .started = 1, .exec_done = 1};
        f.status.session = (struct ppp_session_status){.state = PPP_SESSION_RUNNING, .supervisor_valid = 1,
            .supervisor_phase = PPP_SESSION};
        f.pppd_listed = 1;
    }
    if (f.slices == 50) pppd_publish();
    uint64_t elapsed = f.start_ms ? f.now - f.start_ms : 0;
    if (!f.stop && elapsed >= 20000) {
        if (f.scenario == UNPLUG) {
            /* USB gone: supervisor kills pppd, every key stays; callbacks are slow. */
            f.status.session.error = PPP_SESSION_ERROR_SUPERVISOR; f.status.session.supervisor_error = PPP_ERROR_USB;
            f.child.reaped = 1; f.child.signaled = 1; f.child.term_signal = SIGKILL; f.child.kill_signals = 1;
            f.address_up = 0; f.pppd_listed = 0; f.stop = 1;
        } else f.status.signal_number = SIGINT;
    }
    static unsigned stopped_at;
    if (!f.closed && (*stop || f.status.signal_number || f.stop)) {
        if (!f.stop || !stopped_at) { f.stop = 1; stopped_at = f.slices; }
        if (f.scenario != UNPLUG && f.slices - stopped_at == 2) pppd_term_exit();
        /* The unplugged session closes late; the reap cleanup must not wait for it. */
        if (f.scenario == UNPLUG && f.slices - stopped_at == 5)
            CHECK(!CFDictionaryContainsKey(f.store, FIXED("IPv4")));
        if (f.slices - stopped_at >= (f.scenario == UNPLUG ? 30u : 4u)) {
            f.child.peer_quiet = 1; f.status.session.local_done = 1; f.status.session.state = PPP_SESSION_CLOSED;
            f.status.session.supervisor_phase = PPP_CLOSED; f.closed = 1;
        }
    }
    if (f.closed) stopped_at = 0;
    *s = f.status; return f.status.session.local_done;
}
static int fake_discovery(discovery_target_callback callback, void *context) {
    struct descriptor_config config = {0};
    IOUSBDeviceInterface *device = NULL;
    return callback(context, 1, &device, 42, &config);
}
/* Lock file: never the host /var/run; counts must balance after each run. */
static unsigned lock_opens, lock_closes, lock_takes;
static int lock_busy, lock_open_error;
static int lock_fake_open(const char *path, int flags, ...) {
    if (strcmp(path, "/var/run/dji-cellular-dongle-mac.lock") || !(flags & O_CLOEXEC) || !(flags & O_NOFOLLOW)) return -1;
    if (lock_open_error) { errno = EACCES; return -1; }
    lock_opens++; return 777;
}
static int lock_fake_flock(int fd, int operation) {
    if (fd != 777 || operation != (LOCK_EX | LOCK_NB)) return -1;
    if (lock_busy) { errno = EWOULDBLOCK; return -1; }
    lock_takes++; return 0;
}
static int lock_planted; /* 1: FIFO, 2: second hard link, 3: not root-owned */
static int lock_fake_fstat(int fd, struct stat *st) {
    if (fd != 777) return -1;
    *st = (struct stat){.st_mode = lock_planted == 1 ? S_IFIFO | 0644 : S_IFREG | 0644,
        .st_nlink = lock_planted == 2 ? 2 : 1, .st_uid = lock_planted == 3 ? 501 : 0};
    return 0;
}
static int lock_fake_close(int fd) { if (fd == 777) lock_closes++; return 0; }
int trial_preflight(void *context, struct trial_preflight *p) { return fake_preflight(context, p); }
#define trial_identity fake_identity
#define trial_metadata fake_metadata
#define trial_executable fake_executable
#define trial_preflight(...) fake_preflight(__VA_ARGS__)
#define clock_gettime fake_clock
#define nanosleep fake_sleep
#define sigaction(...) fake_sigaction(__VA_ARGS__)
#define SCDynamicStoreCreate fake_create
#define ppp_trial_observer_begin fake_observer_begin
#define ppp_trial_observer_child fake_observer_child
#define ppp_trial_observer_sample fake_observer_sample
#define ppp_trial_observer_current fake_observer_current
#define ppp_trial_observer_finish fake_observer_finish
#define ppp_trial_observer_result fake_observer_result
#define ppp_trial_observer_diagnostic fake_observer_diagnostic
#define ppp_trial_observer_dispose fake_observer_dispose
#define ppp_session_start fake_session_start
#define ppp_session_stop fake_session_stop
#define ppp_session_step fake_session_step
#define ppp_session_release fake_session_release
#define ppp_runner_begin fake_runner_begin
#define ppp_runner_slice fake_runner_slice
#define ppp_runner_observe fake_runner_observe
#define ppp_runner_restore fake_runner_restore
#define ppp_runner_release fake_runner_release
#define discovery_with_one_target fake_discovery
/* Tentative declaration of up.c's latch, so the fake can compare it. */
static volatile sig_atomic_t up_hangup;
/* Signal query before dialing: 0 confirmed numbers, 1 I/O error, 2 cancelled,
 * 3 CSQ only (CESQ answered ERROR). Kept outside f, which setup() clears. */
static int signal_mode;
static unsigned signal_probes;
static int fake_probe_signal(io_service_t service, IOUSBDeviceInterface **device, uint64_t registry_id,
                             const struct descriptor_config *config, const volatile sig_atomic_t *prior,
                             struct status_probe_result *r) {
    (void)service; (void)device; (void)registry_id; (void)config;
    signal_probes++;
    CHECK(prior == &up_hangup);
    memset(r, 0, sizeof(*r));
    r->stop_index = STATUS_PROBE_SLOTS;
    if (signal_mode == 1 || signal_mode == 2) {
        r->stop = signal_mode == 1 ? STATUS_STOP_IO_ERROR : STATUS_STOP_CANCELLED;
        return signal_mode == 1 ? DJI_GONE : DJI_CANCELLED;
    }
    struct status_command_result *csq = &r->commands[1], *cesq = &r->commands[STATUS_PROBE_CESQ_INDEX];
    *csq = (struct status_command_result){.sent = 1, .complete = 1, .values = 2, .value_lines = 1, .value = {20, 99},
        .result = STATUS_RESULT_OK, .attribution = STATUS_ATTRIBUTION_MATCHED};
    if (signal_mode == 3) {
        *cesq = (struct status_command_result){.sent = 1, .complete = 1, .result = STATUS_RESULT_ERROR,
            .attribution = STATUS_ATTRIBUTION_MATCHED};
        r->stop = STATUS_STOP_FINAL_ERROR; r->stop_index = STATUS_PROBE_CESQ_INDEX;
        return DJI_OK;
    }
    *cesq = (struct status_command_result){.sent = 1, .complete = 1, .values = 6, .value_lines = 1, .value = {99, 99, 255, 255, 19, 46},
        .result = STATUS_RESULT_OK, .attribution = STATUS_ATTRIBUTION_MATCHED};
    r->stop = STATUS_STOP_NONE;
    return DJI_OK;
}
#define usb_observer_probe_signal fake_probe_signal
/* Confirmed numbers only here, so the modem is never re-enumerated. */
static enum usb_recover_result fake_recover(IOUSBDeviceInterface **device) {
    (void)device; CHECK(0);
    return USB_RECOVER_ERROR;
}
#define usb_recover_reenumerate fake_recover
#define open(...) lock_fake_open(__VA_ARGS__)
#define flock(...) lock_fake_flock(__VA_ARGS__)
#define close(...) lock_fake_close(__VA_ARGS__)
#define fstat(...) lock_fake_fstat(__VA_ARGS__)
#define main up_main
#include "../src/ppp_lte_up.c"
#undef main
#undef close
#undef fstat
#undef flock
#undef open

static int fixed_keys(void) {
    int count = 0;
    for (unsigned i = 0; i < PPP_LTE_ENTITIES; i++) count += CFDictionaryContainsKey(f.store, ppp_lte_service_key(i));
    return count;
}
static void run(enum scenario s) {
    if (f.store) CFRelease(f.store);
    memset(&f, 0, sizeof(f)); f.scenario = s; f.now = 1000;
    f.store = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(f.store, CFSTR("State:/Network/Service/WIFI-SERVICE/IPv4"), CFSTR("wifi"));
    if (s == STALE_LEFTOVERS) {
        /* A previous SIGKILL left these behind. */
        CFDictionarySetValue(f.store, FIXED("PPP"), CFSTR("old")); CFDictionarySetValue(f.store, FIXED("IPv6"), CFSTR("old"));
        CFDictionarySetValue(f.store, FIXED("IPv4"), CFSTR("old"));
    }
    if (s == VPN_PPPD) { f.pppd_listed = 1; CFDictionarySetValue(f.store, FIXED("IPv6"), CFSTR("old")); }
    FILE *output = tmpfile();
    char *argv[] = {"up", "--up"};
    int result = lte_up(2, argv, output, NULL);
    fseek(output, 0, SEEK_SET);
    char text[8192] = {0}; (void)fread(text, 1, sizeof(text) - 1, output); fclose(output);
    CHECK(!strstr(text, "private") && !strstr(text, ID) && !strstr(text, "98765") && !strstr(text, "10.20") &&
          !strstr(text, "WIFI") && !strstr(text, "en0"));
    CHECK(CFDictionaryContainsKey(f.store, CFSTR("State:/Network/Service/WIFI-SERVICE/IPv4")));
    CHECK(f.stores == 1);
    const char *want = NULL;
    if (s == USER_STOP || s == STALE_LEFTOVERS) {
        CHECK(result == 0 && !fixed_keys());
        want = "up-result=stopped reason=user-stop connected=yes";
        CHECK(strstr(text, " pppd-leftover=ipv6 sc-reap=done:removed:1:failed:0:before:ipv6:after:none") &&
              strstr(text, " primary-restored=yes ") && strstr(text, "\nup=connected elapsed-ms=") &&
              strstr(text, " primary-ipv4=this-service primary-ipv6=this-service primary-interface=this-interface"));
        if (s == STALE_LEFTOVERS) CHECK(strstr(text, "up=starting sc-pre-removed=3\n") == text);
        else CHECK(strstr(text, "up=starting sc-pre-removed=0\n") == text);
    }
    if (s == UNPLUG) {
        CHECK(result == 3 && !fixed_keys());
        want = "up-result=ended reason=usb-error connected=yes";
        CHECK(strstr(text, " pppd-leftover=ppp+ipv4+ipv6+dns+proxies+interface+smb sc-reap=done:removed:7:failed:0") &&
              strstr(text, " primary-restored=yes ") && strstr(text, " next-run-cleanup=not-required "));
    }
    if (s == VPN_PPPD) {
        CHECK(result == 3 && fixed_keys() == 1 && !f.removes);
        want = "up=blocked gate=pppd-running sc-pre=blocked-pppd:removed:0:failed:0:before:ipv6:after:ipv6";
    }
    if (want && !strstr(text, want)) { failures++; fprintf(stderr, "FAIL scenario=%d text=%s\n", (int)s, text); }
    checks++;
}
int main(void) {
    for (enum scenario s = USER_STOP; s < SCENARIOS; s++) run(s);
    if (f.store) CFRelease(f.store);
    printf("lte-up-integration checks=%u failures=%u (real up/link/service; fake store/process/interface/runner)\n",
           checks, failures);
    return failures ? 1 : 0;
}
