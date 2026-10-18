#define _DARWIN_C_SOURCE
#include "ppp_launch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Private to this command; tests include this implementation, not a public ABI. */
struct local_check_backend {
    void *context;
    int (*identity)(void *context); /* 0 nonroot, 1 root, 2 elevated */
    int (*metadata)(void *context, const char *path, struct stat *st);
    int (*preflight)(void *context, struct ppp_launch_preflight *result);
};
static int local_identity(void *context) {
    (void)context;
    uid_t uid = getuid(), euid = geteuid();
    gid_t gid = getgid(), egid = getegid();
    if (!uid || !euid || !gid || !egid) return 1;
    return uid != euid || gid != egid ? 2 : 0;
}
static int local_metadata(void *context, const char *path, struct stat *st) {
    (void)context;
    return lstat(path, st) ? errno : 0;
}
static int local_preflight(void *context, struct ppp_launch_preflight *result) {
    (void)context;
    return ppp_launch_preflight(result, NULL);
}
static const char *local_executable(const struct local_check_backend *b) {
    static const char *const paths[] = {"/", "/usr", "/usr/sbin", "/usr/sbin/pppd"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat st = {0};
        int error = b->metadata(b->context, paths[i], &st);
        if (error) return error == ENOENT ? "absent" : "access-error";
        if (S_ISLNK(st.st_mode)) return "symlink";
        if (i == 3 ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)) return "wrong-type";
        if (st.st_uid) return "owner-error";
        if (st.st_mode & (S_IWGRP | S_IWOTH)) return "mode-error";
        if (i == 3 && ((st.st_mode & (S_ISUID | S_ISGID)) ||
                      !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))) return "mode-error";
    }
    return "clear";
}
static int local_check(int argc, char **argv, FILE *output,
                       const struct local_check_backend *backend) {
    static const char usage[] =
        "Usage: ppp_local_check --help | --check-host\n"
        "Metadata only; no pppd execution, USB or network changes.\n"
        "Host checks always leave execution blocked (exit 3).\n";
    if (!output) return 1;
    int help = argc == 2 && argv && argv[1] && !strcmp(argv[1], "--help");
    if (help || argc != 2 || !argv || !argv[1] || strcmp(argv[1], "--check-host")) {
        if (fputs(usage, output) == EOF || fflush(output)) return 1;
        return help ? 0 : 2;
    }
    const struct local_check_backend defaults = {NULL, local_identity, local_metadata, local_preflight};
    const struct local_check_backend *b = backend ? backend : &defaults;
    const char *identity = "not-checked", *executable = "not-checked", *metadata = "blocked";
    const char *preflight = "not-checked";
    char diagnostic[1024] = {0};
    int status = 3;
    if (!b->identity || !b->metadata || !b->preflight) status = 1;
    else {
        int id = b->identity(b->context);
        identity = id == 0 ? "nonroot" : id == 1 ? "root" : id == 2 ? "elevated" : "error";
        if (id < 0 || id > 2) status = 1;
        else if (!id) {
            executable = local_executable(b);
            if (!strcmp(executable, "clear")) {
                struct ppp_launch_preflight result = {0};
                if (b->preflight(b->context, &result)) { preflight = "internal-error"; status = 1; }
                else {
                    int count = ppp_launch_diagnostic(&result, diagnostic, sizeof(diagnostic));
                    if (count < 0 || (size_t)count >= sizeof(diagnostic)) {
                        diagnostic[0] = '\0'; preflight = "internal-error"; status = 1;
                    } else {
                        preflight = result.clear ? "clear" : "blocked";
                        metadata = preflight;
                    }
                }
            }
        }
    }
    /* The unchanged policy diagnostic is descriptive; the final local line is
     * this command's blocked decision, never an execution permit. */
    if ((diagnostic[0] && fprintf(output, "%s\n", diagnostic) < 0) ||
        fprintf(output, "local metadata=%s identity=%s executable=%s preflight=%s"
                " privilege=%s execution=blocked system-impact=unproven"
                " installed-source=unmatched auth-policy=unverified interface-cleanup=unverified\n",
                metadata, identity, executable, preflight,
                !strcmp(executable, "clear") ? "unavailable" : "not-checked") < 0 || fflush(output)) return 1;
    return status;
}
int main(int argc, char **argv) {
    return local_check(argc, argv, stdout, NULL);
}
