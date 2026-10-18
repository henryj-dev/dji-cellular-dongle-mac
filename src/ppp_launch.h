#ifndef DJI_PPP_LAUNCH_H
#define DJI_PPP_LAUNCH_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Preparation only: no spawn, authorization, installation check or path override.
 * Object strings are fixed and borrowed; caller must not modify them. */
struct ppp_launch_policy {
    const char *path, *cwd;
    char *argv[16];
    char *envp[1];
};
int ppp_launch_prepare(struct ppp_launch_policy *policy);
/* Fixed SystemConfiguration service ID used only by the connection-keeping
 * mode (build/ppp_lte_up). Stale State keys under this ID are the only ones
 * that mode may remove; never print it. */
#define PPP_LTE_UP_SERVICE_ID "247DA48E-8FCB-42B9-9A2D-01333DB983D0"
/* Connection-keeping policy: same fixed path/cwd/env, but pppd installs the
 * default route and peer DNS for the whole Mac, publishes under the fixed
 * service ID and ends itself after missed LCP echoes. Kept separate so the
 * single-shot/auth policies stay byte-for-byte unchanged. */
struct ppp_launch_up_policy {
    const char *path, *cwd;
    char *argv[24];
    char *envp[1];
};
int ppp_launch_prepare_up(struct ppp_launch_up_policy *policy);
enum ppp_launch_item {
    PPP_LAUNCH_EXECUTABLE, PPP_LAUNCH_OPTIONS, PPP_LAUNCH_POSTOPTIONS, PPP_LAUNCH_USER_OPTIONS,
    PPP_LAUNCH_PAP, PPP_LAUNCH_CHAP, PPP_LAUNCH_SRP, PPP_LAUNCH_IP_UP, PPP_LAUNCH_IP_DOWN,
    PPP_LAUNCH_AUTH_UP, PPP_LAUNCH_AUTH_DOWN, PPP_LAUNCH_IPV6_UP, PPP_LAUNCH_IPV6_DOWN,
    PPP_LAUNCH_ITEMS
};
enum ppp_metadata_state {
    PPP_METADATA_ABSENT, PPP_METADATA_PRESENT, PPP_METADATA_ACCESS,
    PPP_METADATA_SYMLINK, PPP_METADATA_TYPE, PPP_METADATA_USER
};
/* metadata returns 0 or an errno value; never read contents or follow symlinks.
 * home must use the given real UID, returning a borrowed absolute path, not HOME.
 * Test-only dependency injection, not a production policy override. */
struct ppp_metadata_backend {
    void *context;
    uid_t (*real_uid)(void *context);
    int (*home)(void *context, uid_t uid, const char **path);
    int (*metadata)(void *context, const char *path, struct stat *metadata);
};
struct ppp_launch_preflight {
    enum ppp_metadata_state items[PPP_LAUNCH_ITEMS];
    int clear;
};
/* NULL backend selects lstat/getuid/getpwuid inside the method. Only metadata is
 * inspected. Known /etc mapping is checked at /private/etc; all other symlinks
 * (including parent components) fail closed. No private path retained/reported.
 * clear is NOT an execution permit: TOCTOU, auth policy, interface cleanup,
 * installed behavior and logs remain unverified. Diagnostic auth-policy and
 * interface-cleanup labels are descriptive, not enforcement. sensitive-log=...
 * means the user accepted the public-pin user/uid and IP/DNS syslog lines and
 * this build does not suppress them. It is not a permit. */
int ppp_launch_preflight(struct ppp_launch_preflight *result, const struct ppp_metadata_backend *backend);
int ppp_launch_diagnostic(const struct ppp_launch_preflight *result, char *buffer, size_t capacity);

#endif
