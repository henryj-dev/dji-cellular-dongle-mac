#ifndef DJI_PPP_TRIAL_PREFLIGHT_H
#define DJI_PPP_TRIAL_PREFLIGHT_H
#include "ppp_launch.h"
#include <limits.h>

/* Internal trial-only gates; checked home/backend stay alive through session start. */
struct trial_home { uid_t uid; char path[PATH_MAX]; };
struct trial_preflight {
    struct ppp_launch_preflight metadata;
    const char *reason;
    enum { TRIAL_PREFLIGHT_NOT_RUN, TRIAL_PREFLIGHT_COMPLETE,
           TRIAL_PREFLIGHT_ERROR, TRIAL_PREFLIGHT_INVALID } status;
    struct trial_home home;
    struct ppp_metadata_backend backend;
};
struct trial_backend {
    void *context;
    int (*identity)(void *context);
    int (*metadata)(void *context, const char *path, struct stat *st);
    int (*preflight)(void *context, struct trial_preflight *result);
};
int trial_identity(void *context);
int trial_metadata(void *context, const char *path, struct stat *st);
int trial_preflight(void *context, struct trial_preflight *result);
const char *trial_executable(const struct trial_backend *backend);
int trial_preflight_diagnostic(const struct trial_preflight *result, char *buffer, size_t capacity);
#endif
