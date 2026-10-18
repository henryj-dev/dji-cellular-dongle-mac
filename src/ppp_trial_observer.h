#ifndef DJI_PPP_TRIAL_OBSERVER_H
#define DJI_PPP_TRIAL_OBSERVER_H

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <stdint.h>
#include <sys/types.h>

/* One trial only, read-only. Keys and addresses remain private CF objects.
 * No general observer backend, route collector or repair operations. */
struct ppp_trial_observer {
    SCDynamicStoreRef store;
    CFDictionaryRef before_services, before_globals, after_globals;
    CFStringRef service;
    char interface_name[64];
    pid_t pid;
    uint64_t start_sec, start_usec;
    int identity, ambiguous, attributed, snapshot_error;
    int service_removed, interface_removed, finished;
    unsigned interface_index;
    int current_valid;
};
void ppp_trial_observer_begin(struct ppp_trial_observer *observer);
/* Immediately after start, BEFORE any child poll/reap. Never call after reap. */
void ppp_trial_observer_child(struct ppp_trial_observer *observer, pid_t pid);
/* Only while direct child is still owned and not reaped. */
void ppp_trial_observer_sample(struct ppp_trial_observer *observer);
/* Copy last sample's current validity, never historical attribution alone.
 * Caller must sample while child remains owned before each readiness check. */
struct ppp_trial_interface { char name[64]; unsigned index; };
int ppp_trial_observer_current(const struct ppp_trial_observer *observer,
                               struct ppp_trial_interface *interface);
void ppp_trial_observer_finish(struct ppp_trial_observer *observer);
int ppp_trial_observer_diagnostic(const struct ppp_trial_observer *observer,
                                  char *buffer, size_t capacity);
/* 1 known changed/residual, 0 all observed clean, -1 insufficient evidence. */
int ppp_trial_observer_result(const struct ppp_trial_observer *observer);
void ppp_trial_observer_dispose(struct ppp_trial_observer *observer);
#endif
