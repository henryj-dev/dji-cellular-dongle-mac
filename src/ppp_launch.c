#define _DARWIN_C_SOURCE
#include "ppp_launch.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int ppp_launch_prepare(struct ppp_launch_policy *p) {
    if (!p) return -1;
    *p = (struct ppp_launch_policy){.path = "/usr/sbin/pppd", .cwd = "/",
        .argv = {"/usr/sbin/pppd", "notty", "nodetach", "nopersist", "noauth", "noipdefault",
                 "nodefaultroute", "noipv6", "nolog", "refuse-pap", "refuse-chap",
                 "refuse-mschap", "refuse-mschap-v2", "refuse-eap",
                 /* ACSCP is enabled by default and gates SC publish
                  * (check_protocols_ready, commit_publish_dict). */
                 "noacsp", NULL}, .envp = {NULL}};
    return 0;
}
int ppp_launch_prepare_up(struct ppp_launch_up_policy *p) {
    if (!p) return -1;
    /* No nodefaultroute: it clears defaultroute. No noipv6override: the IPv6
     * dummy primary pppd publishes with defaultroute keeps IPv6 off Wi-Fi. */
    *p = (struct ppp_launch_up_policy){.path = "/usr/sbin/pppd", .cwd = "/",
        .argv = {"/usr/sbin/pppd", "notty", "nodetach", "nopersist", "noauth", "noipdefault",
                 "defaultroute", "usepeerdns", "noipv6", "nolog", "refuse-pap", "refuse-chap",
                 "refuse-mschap", "refuse-mschap-v2", "refuse-eap", "noacsp",
                 "serviceid", PPP_LTE_UP_SERVICE_ID,
                 "lcp-echo-interval", "10", "lcp-echo-failure", "3", NULL}, .envp = {NULL}};
    return 0;
}
static uid_t real_uid(void *context) { (void)context; return getuid(); }
static int uid_home(void *context, uid_t uid, const char **path) {
    (void)context;
    struct passwd *entry = getpwuid(uid);
    if (!entry || !entry->pw_dir || entry->pw_dir[0] != '/') return -1;
    *path = entry->pw_dir;
    return 0;
}
static int metadata(void *context, const char *path, struct stat *st) {
    (void)context;
    return lstat(path, st) ? errno : 0;
}
static enum ppp_metadata_state inspect(const struct ppp_metadata_backend *b,
                                       const char *path, int executable) {
    char part[PATH_MAX];
    size_t length = strlen(path);
    if (!length || path[0] != '/' || length >= sizeof(part)) return PPP_METADATA_ACCESS;
    memcpy(part, path, length + 1);
    /* Check each parent without following an untrusted symlink. ENOTDIR and
     * permission errors are unknown, never absence. /private/etc is the fixed
     * spelling of the known system /etc mapping, not a general realpath bypass. */
    for (size_t i = 1; i <= length; i++) {
        if (part[i] != '/' && i != length) continue;
        char saved = part[i]; part[i] = '\0';
        struct stat st = {0};
        int error = b->metadata(b->context, part, &st);
        part[i] = saved;
        if (error) return error == ENOENT ? PPP_METADATA_ABSENT : PPP_METADATA_ACCESS;
        if (S_ISLNK(st.st_mode)) return PPP_METADATA_SYMLINK;
        if (i < length) {
            if (!S_ISDIR(st.st_mode)) return PPP_METADATA_TYPE;
        } else {
            if (!S_ISREG(st.st_mode)) return PPP_METADATA_TYPE;
            if (executable && !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) return PPP_METADATA_ACCESS;
        }
    }
    return PPP_METADATA_PRESENT;
}
int ppp_launch_preflight(struct ppp_launch_preflight *r, const struct ppp_metadata_backend *backend) {
    static const char *const paths[PPP_LAUNCH_ITEMS] = {
        "/usr/sbin/pppd", "/private/etc/ppp/options", "/private/etc/ppp/postoptions", NULL,
        "/private/etc/ppp/pap-secrets", "/private/etc/ppp/chap-secrets", "/private/etc/ppp/srp-secrets",
        "/private/etc/ppp/ip-up", "/private/etc/ppp/ip-down", "/private/etc/ppp/auth-up",
        "/private/etc/ppp/auth-down", "/private/etc/ppp/ipv6-up", "/private/etc/ppp/ipv6-down"};
    const struct ppp_metadata_backend defaults = {NULL, real_uid, uid_home, metadata};
    const struct ppp_metadata_backend *b = backend ? backend : &defaults;
    if (!r || !b->real_uid || !b->home || !b->metadata) return -1;
    *r = (struct ppp_launch_preflight){.clear = 1};
    const char *home = NULL;
    char user[PATH_MAX];
    int home_error = b->home(b->context, b->real_uid(b->context), &home);
    int user_valid = !home_error && home && home[0] == '/';
    if (user_valid) {
        size_t length = strlen(home);
        while (length > 1 && home[length - 1] == '/') length--;
        if (length + sizeof("/.ppprc") > sizeof(user)) user_valid = 0;
        else {
            memcpy(user, home, length);
            memcpy(user + length, length == 1 ? ".ppprc" : "/.ppprc",
                   length == 1 ? sizeof(".ppprc") : sizeof("/.ppprc"));
        }
    }
    for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++) {
        r->items[i] = i == PPP_LAUNCH_USER_OPTIONS ?
            (user_valid ? inspect(b, user, 0) : PPP_METADATA_USER) : inspect(b, paths[i], i == 0);
        if (r->items[i] != (i == PPP_LAUNCH_EXECUTABLE ? PPP_METADATA_PRESENT : PPP_METADATA_ABSENT))
            r->clear = 0;
    }
    return 0;
}
int ppp_launch_diagnostic(const struct ppp_launch_preflight *r, char *buffer, size_t capacity) {
    static const char *const labels[] = {"executable", "options", "postoptions", "user-options",
        "pap-secrets", "chap-secrets", "srp-secrets", "ip-up", "ip-down", "auth-up", "auth-down",
        "ipv6-up", "ipv6-down"};
    static const char *const states[] = {"absent", "present", "access-error", "symlink", "wrong-type", "user-error"};
    if (!r || (!buffer && capacity)) return -1;
    for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++)
        if ((unsigned)r->items[i] >= sizeof(states) / sizeof(states[0])) return -1;
    int used = snprintf(buffer, capacity, "policy metadata=%s execution=unapproved", r->clear ? "clear" : "blocked");
    for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++) {
        if (used < 0) return -1;
        size_t offset = (size_t)used < capacity ? (size_t)used : capacity;
        int added = snprintf(buffer ? buffer + offset : NULL, capacity - offset,
                             " %s=%s", labels[i], states[r->items[i]]);
        if (added < 0 || added > INT_MAX - used) return -1;
        used += added;
    }
    if (used < 0) return -1;
    {
        size_t offset = (size_t)used < capacity ? (size_t)used : capacity;
        int added = snprintf(buffer ? buffer + offset : NULL, capacity - offset,
                             " auth-policy=unverified interface-cleanup=unverified"
                             " sensitive-log=unsuppressed installed-source=unmatched");
        /* Accepted residual log, not an execution permit. No user/uid/path/IP/DNS. */
        if (added < 0 || added > INT_MAX - used) return -1;
        used += added;
    }
    return used;
}
