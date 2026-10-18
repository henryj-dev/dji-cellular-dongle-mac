#define _DARWIN_C_SOURCE
#include "../src/ppp_launch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL launch line=%d\n", __LINE__); failures++; } } while (0)
struct fake {
    const char *target, *home;
    int error, home_error, home_calls, user_seen, calls;
    mode_t mode;
    uid_t uid_seen;
};
static uid_t uid(void *context) { (void)context; return 123; }
static int home(void *context, uid_t real, const char **path) {
    struct fake *f = context;
    f->uid_seen = real; f->home_calls++;
    *path = f->home;
    return f->home_error;
}
static int metadata(void *context, const char *path, struct stat *st) {
    struct fake *f = context;
    f->calls++;
    if (!strcmp(path, "/private-user-marker/.ppprc")) f->user_seen++;
    CHECK(strncmp(path, "/etc/", 5) && !strstr(path, "wrong-HOME-marker"));
    if (f->target && !strcmp(path, f->target)) {
        st->st_mode = f->mode;
        return f->error;
    }
    if (!strcmp(path, "/usr/sbin/pppd")) { st->st_mode = S_IFREG | 0755; return 0; }
    if (strstr(path, "/ppp/") || strstr(path, "/.ppprc")) return ENOENT;
    st->st_mode = S_IFDIR | 0755;
    return 0;
}
static void inspect(struct fake *f, enum ppp_launch_item item, enum ppp_metadata_state expected, int clear) {
    struct ppp_metadata_backend b = {f, uid, home, metadata};
    struct ppp_launch_preflight r;
    CHECK(ppp_launch_preflight(&r, &b) == 0 && r.clear == clear && r.items[item] == expected);
    CHECK(f->uid_seen == 123 && f->home_calls > 0);
    char diagnostic[1024], tiny[2];
    int count = ppp_launch_diagnostic(&r, diagnostic, sizeof(diagnostic));
    CHECK(count > 0 && strstr(diagnostic, "execution=unapproved") &&
          strstr(diagnostic, "auth-policy=unverified") &&
          strstr(diagnostic, "interface-cleanup=unverified") &&
          strstr(diagnostic, "sensitive-log=unsuppressed") &&
          strstr(diagnostic, "installed-source=unmatched"));
    CHECK(!strstr(diagnostic, "private-user-marker") && !strstr(diagnostic, "wrong-HOME-marker") &&
          !strstr(diagnostic, "/usr") && !strstr(diagnostic, "/private"));
    CHECK(ppp_launch_diagnostic(&r, tiny, sizeof(tiny)) == count && tiny[1] == '\0');
    CHECK(ppp_launch_diagnostic(&r, NULL, 0) == count);
}
int main(void) {
    struct ppp_launch_policy p;
    const char *const expected[] = {"/usr/sbin/pppd", "notty", "nodetach", "nopersist", "noauth",
        "noipdefault", "nodefaultroute", "noipv6", "nolog", "refuse-pap", "refuse-chap",
        "refuse-mschap", "refuse-mschap-v2", "refuse-eap", "noacsp"};
    CHECK(ppp_launch_prepare(NULL) == -1 && ppp_launch_prepare(&p) == 0);
    CHECK(!strcmp(p.path, expected[0]) && !strcmp(p.cwd, "/") && !p.envp[0] && !p.argv[15]);
    CHECK(sizeof(p.argv) / sizeof(p.argv[0]) == 16);
    for (unsigned i = 0; i < 15; i++) CHECK(!strcmp(p.argv[i], expected[i]));
    /* Connection-keeping policy: its own fixed argv, the single-shot one above unchanged. */
    struct ppp_launch_up_policy up;
    const char *const expected_up[] = {"/usr/sbin/pppd", "notty", "nodetach", "nopersist", "noauth",
        "noipdefault", "defaultroute", "usepeerdns", "noipv6", "nolog", "refuse-pap", "refuse-chap",
        "refuse-mschap", "refuse-mschap-v2", "refuse-eap", "noacsp", "serviceid", PPP_LTE_UP_SERVICE_ID,
        "lcp-echo-interval", "10", "lcp-echo-failure", "3"};
    const unsigned up_count = sizeof(expected_up) / sizeof(expected_up[0]);
    CHECK(ppp_launch_prepare_up(NULL) == -1 && ppp_launch_prepare_up(&up) == 0);
    CHECK(sizeof(up.argv) / sizeof(up.argv[0]) == 24 && up_count == 22);
    CHECK(!strcmp(up.path, "/usr/sbin/pppd") && !strcmp(up.cwd, "/") && !up.envp[0]);
    for (unsigned i = 0; i < up_count; i++) CHECK(up.argv[i] && !strcmp(up.argv[i], expected_up[i]));
    for (unsigned i = up_count; i < 24; i++) CHECK(!up.argv[i]);
    for (unsigned i = 0; i < up_count; i++)
        CHECK(strcmp(up.argv[i], "nodefaultroute") && strcmp(up.argv[i], "noipv6override") &&
              strcmp(up.argv[i], "persist") && strcmp(up.argv[i], "demand"));
    /* Fixed upper-case 8-4-4-4-12 UUID. */
    const char *id = PPP_LTE_UP_SERVICE_ID;
    CHECK(strlen(id) == 36);
    for (unsigned i = 0; i < 36 && id[i]; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) CHECK(id[i] == '-');
        else CHECK((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'F'));
    }
    /* The single-shot policy never gains the connection-keeping options. */
    for (unsigned i = 0; i < 15; i++)
        CHECK(strcmp(p.argv[i], "defaultroute") && strcmp(p.argv[i], "usepeerdns") &&
              strcmp(p.argv[i], "serviceid") && strncmp(p.argv[i], "lcp-echo", 8));
    CHECK(setenv("HOME", "/wrong-HOME-marker", 1) == 0);
    struct fake f = {.home = "/private-user-marker"};
    inspect(&f, PPP_LAUNCH_USER_OPTIONS, PPP_METADATA_ABSENT, 1);
    CHECK(f.user_seen == 1);
    const char *const paths[] = {"/usr/sbin/pppd", "/private/etc/ppp/options", "/private/etc/ppp/postoptions",
        "/private-user-marker/.ppprc", "/private/etc/ppp/pap-secrets", "/private/etc/ppp/chap-secrets",
        "/private/etc/ppp/srp-secrets", "/private/etc/ppp/ip-up", "/private/etc/ppp/ip-down",
        "/private/etc/ppp/auth-up", "/private/etc/ppp/auth-down", "/private/etc/ppp/ipv6-up", "/private/etc/ppp/ipv6-down"};
    for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++) {
        f.target = paths[i]; f.error = 0; f.mode = S_IFREG | 0755;
        inspect(&f, (enum ppp_launch_item)i, PPP_METADATA_PRESENT, i == 0);
        f.mode = S_IFLNK | 0777;
        inspect(&f, (enum ppp_launch_item)i, PPP_METADATA_SYMLINK, 0);
        f.mode = S_IFDIR | 0755;
        inspect(&f, (enum ppp_launch_item)i, PPP_METADATA_TYPE, 0);
        const int errors[] = {EACCES, EPERM, ENOTDIR, EIO};
        for (unsigned j = 0; j < sizeof(errors) / sizeof(errors[0]); j++) {
            f.error = errors[j]; inspect(&f, (enum ppp_launch_item)i, PPP_METADATA_ACCESS, 0);
        }
    }
    f.target = paths[0]; f.error = ENOENT;
    inspect(&f, PPP_LAUNCH_EXECUTABLE, PPP_METADATA_ABSENT, 0);
    f.error = 0; f.mode = S_IFREG | 0644;
    inspect(&f, PPP_LAUNCH_EXECUTABLE, PPP_METADATA_ACCESS, 0);
    f.target = "/private/etc/ppp"; f.mode = S_IFLNK | 0777;
    inspect(&f, PPP_LAUNCH_OPTIONS, PPP_METADATA_SYMLINK, 0);
    f.mode = S_IFREG | 0755;
    inspect(&f, PPP_LAUNCH_OPTIONS, PPP_METADATA_TYPE, 0);
    f.error = EACCES;
    inspect(&f, PPP_LAUNCH_OPTIONS, PPP_METADATA_ACCESS, 0);
    f.error = ENOENT;
    inspect(&f, PPP_LAUNCH_OPTIONS, PPP_METADATA_ABSENT, 1);
    f.target = NULL; f.error = 0; f.home_error = -1;
    inspect(&f, PPP_LAUNCH_USER_OPTIONS, PPP_METADATA_USER, 0);
    f.home_error = 0; f.home = "relative";
    inspect(&f, PPP_LAUNCH_USER_OPTIONS, PPP_METADATA_USER, 0);
    f.home = NULL;
    inspect(&f, PPP_LAUNCH_USER_OPTIONS, PPP_METADATA_USER, 0);
    char large[8192]; memset(large, 'a', sizeof(large)); large[0] = '/'; large[sizeof(large) - 1] = 0;
    f.home = large;
    inspect(&f, PPP_LAUNCH_USER_OPTIONS, PPP_METADATA_USER, 0);
    struct ppp_metadata_backend invalid = {0};
    struct ppp_launch_preflight r = {0};
    CHECK(ppp_launch_preflight(&r, &invalid) == -1 && ppp_launch_preflight(NULL, &invalid) == -1);
    r.items[0] = (enum ppp_metadata_state)99;
    CHECK(ppp_launch_diagnostic(&r, large, sizeof(large)) == -1);
    printf("ppp_launch fixed policy/fake metadata checks=%u failures=%u (no host contents or spawn)\n", checks, failures);
    return failures ? 1 : 0;
}
