#ifndef DJI_PPP_AUTH_PROBE_H
#define DJI_PPP_AUTH_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define PPP_AUTH_FRAME_MAX 4096u
#define PPP_AUTH_BYTE_MAX 65536u
#define PPP_AUTH_SLICE 4096u
/* Fixed, deliberately incomplete diagnostic peer. Never answers the daemon's
 * Configure-Request (RFC 1661 section 5.1 MUST reply is NOT implemented).
 * No LCP Opened, authentication payload, IPCP or IP traffic. */
struct ppp_auth_probe {
    unsigned char frame[PPP_AUTH_FRAME_MAX], output[64];
    size_t used, output_length, output_offset;
    uint64_t received, sent;
    unsigned requests, rejects, peer_requests;
    int framed, escaped, ready, failed;
    const char *reason;
};
void ppp_auth_probe_init(struct ppp_auth_probe *probe);
void ppp_auth_probe_fail(struct ppp_auth_probe *probe, const char *reason);
/* Feed only after confirmed exec success. Limits count actual wire bytes. */
int ppp_auth_probe_input(struct ppp_auth_probe *probe, const unsigned char *data, size_t length);
const unsigned char *ppp_auth_probe_output(struct ppp_auth_probe *probe, size_t *length);
int ppp_auth_probe_written(struct ppp_auth_probe *probe, size_t count);
uint16_t ppp_auth_fcs(uint16_t fcs, const unsigned char *data, size_t length);
#endif
