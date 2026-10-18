#define _DARWIN_C_SOURCE
#include "ppp_trial_preflight.h"
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int trial_identity(void *context) {
    (void)context;
    return getuid() == 0 && geteuid() == 0 && getgid() == getegid() ? 0 : -1;
}
int trial_metadata(void *context, const char *path, struct stat *st) {
    (void)context;
    return lstat(path, st) ? errno : 0;
}
static uid_t trial_real_uid(void *context) { (void)context; return getuid(); }
static int trial_checked_home(void *context, uid_t uid, const char **path) {
    const struct trial_home *home = context;
    if (uid != home->uid) return -1;
    *path = home->path;
    return 0;
}
static const char *trial_home_directory(const char *path) {
    struct stat st = {0};
    if (lstat(path, &st)) return "home-metadata-error";
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return "home-type-error";
    if (st.st_uid || (st.st_mode & (S_IWGRP | S_IWOTH))) return "home-owner-mode-error";
    return NULL;
}
int trial_preflight(void *context, struct trial_preflight *result) {
    (void)context;
    *result = (struct trial_preflight){.reason = "home-lookup-error"};
    struct trial_home *saved_home = &result->home;
    struct trial_home home = {.uid = getuid()};
    struct passwd *entry = getpwuid(home.uid);
    if (!entry || !entry->pw_dir) return -1;
    result->reason = "home-path-error";
    const char *source = entry->pw_dir;
    size_t length = strnlen(source, sizeof(home.path));
    if (!length || length >= sizeof(home.path) || source[0] != '/') return -1;
    /* Reject ambiguous spelling rather than normalize it or follow aliases. */
    for (size_t start = 1; start < length;) {
        size_t end = start;
        while (end < length && source[end] != '/') {
            unsigned char byte = (unsigned char)source[end];
            if (byte < 32 || byte == 127) return -1;
            end++;
        }
        size_t part = end - start;
        if (!part || part > NAME_MAX || (part == 1 && source[start] == '.') ||
            (part == 2 && source[start] == '.' && source[start + 1] == '.') ||
            (end < length && end + 1 == length)) return -1;
        start = end + 1;
    }
    const char *reason = trial_home_directory("/");
    if (reason) { result->reason = reason; return -1; }
    int alias = length >= 4 && !memcmp(source, "/var", 4) &&
        (length == 4 || source[4] == '/');
    if (alias) {
        struct stat st = {0};
        result->reason = "alias-metadata-error";
        if (lstat("/var", &st)) return -1;
        result->reason = "alias-type-error";
        if (!S_ISLNK(st.st_mode)) return -1;
        result->reason = "alias-owner-error";
        if (st.st_uid) return -1;
        char target[sizeof("/private/var")];
        ssize_t count = readlink("/var", target, sizeof(target));
        result->reason = "alias-readlink-error";
        if (count < 0) return -1;
        result->reason = "alias-target-error";
        /* readlink does not NUL-terminate. A full buffer may be truncated. */
        if ((size_t)count >= sizeof(target) ||
            !((count == (ssize_t)sizeof("private/var") - 1 &&
               !memcmp(target, "private/var", sizeof("private/var") - 1)) ||
              (count == (ssize_t)sizeof("/private/var") - 1 &&
               !memcmp(target, "/private/var", sizeof("/private/var") - 1)))) return -1;
        result->reason = "home-path-error";
        if (length > sizeof(home.path) - sizeof("/private")) return -1;
        memcpy(home.path, "/private", sizeof("/private") - 1);
        memcpy(home.path + sizeof("/private") - 1, source, length + 1);
        length += sizeof("/private") - 1;
    } else memcpy(home.path, source, length + 1);
    result->reason = "home-path-error";
    if (length + (length == 1 ? sizeof(".ppprc") : sizeof("/.ppprc")) > sizeof(home.path)) return -1;
    /* Unlike the common leaf checker, every physical home parent must exist.
     * Owner/write restrictions here are an additional trial-only gate. */
    for (size_t i = 1; i <= length; i++) {
        if (home.path[i] != '/' && i != length) continue;
        if (length == 1) break; /* / was already checked. */
        char saved = home.path[i]; home.path[i] = '\0';
        reason = trial_home_directory(home.path);
        home.path[i] = saved;
        if (reason) { result->reason = reason; return -1; }
    }
    *saved_home = home;
    result->backend = (struct ppp_metadata_backend){
        saved_home, trial_real_uid, trial_checked_home, trial_metadata
    };
    result->reason = "ppp-preflight-error";
    result->status = TRIAL_PREFLIGHT_ERROR;
    if (ppp_launch_preflight(&result->metadata, &result->backend)) return -1;
    result->status = TRIAL_PREFLIGHT_INVALID;
    result->reason = "ppp-preflight-invalid";
    int clear = 1;
    for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++) {
        if ((unsigned)result->metadata.items[i] > PPP_METADATA_USER) return -1;
        if (result->metadata.items[i] !=
            (i == PPP_LAUNCH_EXECUTABLE ? PPP_METADATA_PRESENT : PPP_METADATA_ABSENT)) clear = 0;
    }
    if (result->metadata.clear != clear) return -1;
    result->status = TRIAL_PREFLIGHT_COMPLETE;
    result->reason = clear ? "clear" : "ppp-preflight-blocked";
    return 0;
}
int trial_preflight_diagnostic(const struct trial_preflight *result, char *buffer, size_t capacity) {
    static const char *const labels[] = {"executable", "options", "postoptions", "user-options",
        "pap-secrets", "chap-secrets", "srp-secrets", "ip-up", "ip-down", "auth-up", "auth-down",
        "ipv6-up", "ipv6-down"};
    static const char *const states[] = {"absent", "present", "access-error", "symlink", "wrong-type", "user-error"};
    static const char *const validity[] = {"not-run", "complete", "error", "invalid"};
    if (!result || (!buffer && capacity) || (unsigned)result->status >= 4) return -1;
    int used = snprintf(buffer, capacity, "preflight=%s", validity[result->status]);
    if (result->status != TRIAL_PREFLIGHT_COMPLETE) return used;
    int clear = 1;
    for (unsigned i = 0; i < PPP_LAUNCH_ITEMS; i++) {
        if ((unsigned)result->metadata.items[i] >= sizeof(states) / sizeof(states[0])) return -1;
        if (result->metadata.items[i] !=
            (i == PPP_LAUNCH_EXECUTABLE ? PPP_METADATA_PRESENT : PPP_METADATA_ABSENT)) clear = 0;
        if (used < 0) return -1;
        size_t offset = (size_t)used < capacity ? (size_t)used : capacity;
        int added = snprintf(buffer ? buffer + offset : NULL, capacity - offset,
                             " %s=%s", labels[i], states[result->metadata.items[i]]);
        if (added < 0 || added > INT_MAX - used) return -1;
        used += added;
    }
    return result->metadata.clear == clear ? used : -1;
}
const char *trial_executable(const struct trial_backend *b) {
    static const char *const paths[] = {"/", "/usr", "/usr/sbin", "/usr/sbin/pppd"};
    for (unsigned i = 0; i < 4; i++) {
        struct stat st = {0};
        if (b->metadata(b->context, paths[i], &st)) return "metadata-error";
        if (S_ISLNK(st.st_mode) || (i == 3 ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode)))
            return "type-error";
        if (st.st_uid || (st.st_mode & (S_IWGRP | S_IWOTH))) return "owner-mode-error";
        if (i == 3 && ((st.st_mode & (S_ISUID | S_ISGID)) ||
            !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))) return "execute-mode-error";
    }
    return "clear";
}
