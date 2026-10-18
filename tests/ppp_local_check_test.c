#define _DARWIN_C_SOURCE
#include "../src/ppp_launch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL local-check line=%d\n", __LINE__); failures++; } } while (0)
struct fake {
    uid_t uid, euid, owner;
    gid_t gid, egid;
    const char *target, *config;
    mode_t mode;
    int error, home_error, identity_error, preflight_error, bad_diagnostic;
    unsigned identity_calls, metadata_calls, preflight_calls, null_backend_calls, home_calls;
};
static struct fake *host;
static uid_t fake_getuid(void) { host->identity_calls++; return host->uid; }
static uid_t fake_geteuid(void) { host->identity_calls++; return host->euid; }
static gid_t fake_getgid(void) { host->identity_calls++; return host->gid; }
static gid_t fake_getegid(void) { host->identity_calls++; return host->egid; }
static int fake_metadata(void *context, const char *path, struct stat *st) {
    struct fake *f = context;
    f->metadata_calls++;
    if (f->target && !strcmp(path, f->target)) {
        st->st_mode = f->mode; st->st_uid = f->owner;
        return f->error;
    }
    st->st_uid = 0;
    st->st_mode = !strcmp(path, "/usr/sbin/pppd") ? S_IFREG | 0755 : S_IFDIR | 0755;
    if (f->config && !strcmp(path, f->config)) { st->st_mode = S_IFREG | 0600; return 0; }
    return strstr(path, "/ppp/") || strstr(path, "/.ppprc") ? ENOENT : 0;
}
static int fake_lstat(const char *path, struct stat *st) {
    int error = fake_metadata(host, path, st);
    if (error) { errno = error; return -1; }
    return 0;
}
static uid_t fake_real_uid(void *context) { return ((struct fake *)context)->uid; }
static int fake_home(void *context, uid_t uid, const char **path) {
    struct fake *f = context;
    CHECK(uid == f->uid); f->home_calls++;
    *path = "/home-private-account-marker";
    return f->home_error;
}
static int fake_preflight(void *context, struct ppp_launch_preflight *result) {
    struct fake *f = context;
    f->preflight_calls++;
    if (f->preflight_error) return -1;
    const struct ppp_metadata_backend b = {f, fake_real_uid, fake_home, fake_metadata};
    int error = ppp_launch_preflight(result, &b);
    if (f->bad_diagnostic) result->items[0] = (enum ppp_metadata_state)99;
    return error;
}
static int fake_default_preflight(struct ppp_launch_preflight *result,
                                  const struct ppp_metadata_backend *backend) {
    CHECK(backend == NULL);
    host->null_backend_calls++;
    return fake_preflight(host, result);
}
/* OS calls and default preflight are replaced only in this translation unit.
 * Even the NULL/default path cannot inspect the actual host. */
#define getuid fake_getuid
#define geteuid fake_geteuid
#define getgid fake_getgid
#define getegid fake_getegid
#define lstat fake_lstat
#define ppp_launch_preflight(...) fake_default_preflight(__VA_ARGS__)
#define main local_check_main
#include "../src/ppp_local_check.c"
#undef main
#undef ppp_launch_preflight
#undef lstat
#undef getegid
#undef getgid
#undef geteuid
#undef getuid

static int fake_identity(void *context) {
    struct fake *f = context;
    CHECK(f == host);
    return f->identity_error ? -1 : local_identity(context);
}
static struct fake fresh(void) {
    return (struct fake){.uid = 765432, .euid = 765432, .gid = 654321, .egid = 654321};
}
static void invoke(struct fake *f, int argc, char **argv, int defaults, int expected,
                   const char *label) {
    FILE *output = tmpfile();
    if (!output) { CHECK(output != NULL); exit(EXIT_FAILURE); }
    host = f;
    const struct local_check_backend backend = {f, fake_identity, fake_metadata, fake_preflight};
    CHECK(local_check(argc, argv, output, defaults ? NULL : &backend) == expected);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    char text[2048] = {0};
    size_t count = fread(text, 1, sizeof(text) - 1, output);
    CHECK(!ferror(output) && count < sizeof(text) - 1);
    CHECK(strstr(text, label) != NULL);
    const char *const private_markers[] = {"765432", "654321", "home-private-account-marker",
        "account-secret-marker", "config-content-marker", "password-secret-marker",
        "private-program-path-marker", "/usr/", "/private/"};
    for (unsigned i = 0; i < sizeof(private_markers) / sizeof(private_markers[0]); i++)
        CHECK(!strstr(text, private_markers[i]));
    if (expected == 3 || (argc == 2 && argv && argv[1] && !strcmp(argv[1], "--check-host"))) {
        CHECK(strstr(text, "execution=blocked system-impact=unproven"));
        CHECK(strstr(text, "executable=clear") ? strstr(text, "privilege=unavailable") != NULL :
              strstr(text, "privilege=not-checked") != NULL);
        CHECK(strstr(text, "installed-source=unmatched"));
        CHECK(strstr(text, "auth-policy=unverified interface-cleanup=unverified"));
        if (f->preflight_calls && !f->preflight_error && !f->bad_diagnostic) {
            const char *policy = strstr(text, "policy metadata=");
            const char *decision = strstr(text, "local metadata=");
            CHECK(policy && decision && policy < decision);
            CHECK(strstr(text, "execution=unapproved"));
            int blocked = f->config || f->target || f->home_error;
            CHECK(strstr(text, blocked ? "policy metadata=blocked" : "policy metadata=clear"));
            CHECK(strstr(text, blocked ?
                "local metadata=blocked identity=nonroot executable=clear preflight=blocked" :
                "local metadata=clear identity=nonroot executable=clear preflight=clear"));
        }
    }
    CHECK(fclose(output) == 0);
    host = NULL;
}
int main(void) {
    char *check[] = {"private-program-path-marker", "--check-host", NULL};
    char *help[] = {"private-program-path-marker", "--help", NULL};
    char *invalid[][4] = {
        {"checker", NULL}, {"checker", "--run", NULL}, {"checker", "--version", NULL},
        {"checker", "--unknown", NULL}, {"checker", "--help", "--check-host", NULL},
        {"checker", "--check-host", "--help", NULL}, {"checker", "--check-host", "--check-host", NULL},
        {"checker", "--help", "--help", NULL}, {"checker", "--check-host", "/private/path-marker", NULL},
        {"checker", "--path=account-secret-marker", NULL},
        {"checker", "--options=config-content-marker", NULL},
        {"checker", "--password=password-secret-marker", NULL}, {"checker", "", NULL}
    };
    for (int defaults = 0; defaults <= 1; defaults++) {
        struct fake f = fresh();
        invoke(&f, 2, help, defaults, 0, "Usage:");
        CHECK(!f.identity_calls && !f.metadata_calls && !f.preflight_calls && !f.home_calls);
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            int argc = invalid[i][2] ? 3 : invalid[i][1] ? 2 : 1;
            invoke(&f, argc, invalid[i], defaults, 2, "Usage:");
            CHECK(!f.identity_calls && !f.metadata_calls && !f.preflight_calls && !f.home_calls);
        }
        invoke(&f, 0, NULL, defaults, 2, "Usage:");
        CHECK(!f.identity_calls && !f.metadata_calls && !f.preflight_calls);
        invoke(&f, 2, check, defaults, 3, "metadata=clear identity=nonroot executable=clear preflight=clear");
        CHECK(f.identity_calls == 4 && f.metadata_calls > 4 && f.preflight_calls == 1 && f.home_calls == 1);
        CHECK(f.null_backend_calls == (unsigned)defaults);
        f = fresh(); f.config = "/private/etc/ppp/options";
        invoke(&f, 2, check, defaults, 3, "metadata=blocked identity=nonroot executable=clear preflight=blocked");
        CHECK(f.preflight_calls == 1 && f.home_calls == 1);
        f = fresh(); f.config = "/home-private-account-marker/.ppprc";
        invoke(&f, 2, check, defaults, 3, "user-options=present");
        const struct { int error, home_error; mode_t mode; const char *label; } blocked[] = {
            {EACCES, 0, 0, " options=access-error"},
            {EPERM, 0, 0, " options=access-error"},
            {ENOTDIR, 0, 0, " options=access-error"},
            {0, 0, S_IFLNK | 0777, " options=symlink"},
            {0, -1, 0, " user-options=user-error"}
        };
        for (unsigned i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
            f = fresh(); f.error = blocked[i].error; f.home_error = blocked[i].home_error;
            f.mode = blocked[i].mode;
            f.target = f.home_error ? NULL : "/private/etc/ppp/options";
            invoke(&f, 2, check, defaults, 3, blocked[i].label);
            CHECK(f.preflight_calls == 1 && f.home_calls == 1);
        }
        f = fresh(); f.preflight_error = 1;
        invoke(&f, 2, check, defaults, 1, "preflight=internal-error");
        f = fresh(); f.bad_diagnostic = 1;
        invoke(&f, 2, check, defaults, 1, "preflight=internal-error");
        for (unsigned i = 0; i < 6; i++) {
            f = fresh();
            if (i == 0) f.uid = 0;
            if (i == 1) f.euid = 0;
            if (i == 2) f.gid = 0;
            if (i == 3) f.egid = 0;
            if (i == 4) f.euid++;
            if (i == 5) f.egid++;
            invoke(&f, 2, check, defaults, 3, i < 4 ? "identity=root" : "identity=elevated");
            CHECK(f.identity_calls == 4 && !f.metadata_calls && !f.preflight_calls && !f.home_calls);
        }
        const char *const paths[] = {"/", "/usr", "/usr/sbin", "/usr/sbin/pppd"};
        const int errors[] = {ENOENT, EACCES, EPERM, ENOTDIR, EIO};
        for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            for (unsigned j = 0; j < sizeof(errors) / sizeof(errors[0]); j++) {
                f = fresh(); f.target = paths[i]; f.error = errors[j];
                invoke(&f, 2, check, defaults, 3, j ? "executable=access-error" : "executable=absent");
                CHECK(f.metadata_calls == i + 1 && !f.preflight_calls && !f.home_calls);
            }
            const mode_t modes[] = {S_IFLNK | 0777, S_IFIFO | 0600,
                (i == 3 ? S_IFDIR : S_IFREG) | 0755,
                (i == 3 ? S_IFREG : S_IFDIR) | 0775,
                (i == 3 ? S_IFREG : S_IFDIR) | 0757};
            for (unsigned j = 0; j < sizeof(modes) / sizeof(modes[0]); j++) {
                f = fresh(); f.target = paths[i]; f.mode = modes[j];
                invoke(&f, 2, check, defaults, 3, j == 0 ? "executable=symlink" :
                       j < 3 ? "executable=wrong-type" : "executable=mode-error");
                CHECK(f.metadata_calls == i + 1 && !f.preflight_calls);
            }
            f = fresh(); f.target = paths[i]; f.owner = 765432;
            f.mode = (i == 3 ? S_IFREG : S_IFDIR) | 0755;
            invoke(&f, 2, check, defaults, 3, "executable=owner-error");
            CHECK(f.metadata_calls == i + 1 && !f.preflight_calls);
        }
        const mode_t modes[] = {S_IFREG | 0644, S_IFREG | 04755, S_IFREG | 02755};
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            f = fresh(); f.target = "/usr/sbin/pppd"; f.mode = modes[i];
            invoke(&f, 2, check, defaults, 3, "executable=mode-error");
            CHECK(!f.preflight_calls);
        }
    }
    struct fake f = fresh(); f.identity_error = 1;
    invoke(&f, 2, check, 0, 1, "identity=error");
    CHECK(!f.metadata_calls && !f.preflight_calls);
    FILE *output = tmpfile();
    if (!output) { CHECK(output != NULL); return EXIT_FAILURE; }
    const struct local_check_backend invalid_backend = {0};
    CHECK(local_check(2, check, output, &invalid_backend) == 1);
    CHECK(local_check(2, check, NULL, NULL) == 1);
    CHECK(fclose(output) == 0);
    printf("ppp_local_check fake-only checks=%u failures=%u (no host inspection or pppd execution)\n", checks, failures);
    return failures ? 1 : 0;
}
