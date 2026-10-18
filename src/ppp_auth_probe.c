#include "ppp_auth_probe.h"
#include <string.h>

static const unsigned char options[5][5] = {
    {3,4,0xc0,0x23,0}, {3,5,0xc2,0x23,5}, {3,5,0xc2,0x23,0x80},
    {3,5,0xc2,0x23,0x81}, {3,4,0xc2,0x27,0}
};
uint16_t ppp_auth_fcs(uint16_t fcs, const unsigned char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        fcs ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            fcs = (uint16_t)((fcs >> 1) ^ ((fcs & 1) ? 0x8408 : 0));
    }
    return fcs;
}
void ppp_auth_probe_init(struct ppp_auth_probe *p) {
    if (p) *p = (struct ppp_auth_probe){.reason = "none"};
}
void ppp_auth_probe_fail(struct ppp_auth_probe *p, const char *reason) {
    if (p && !p->failed) { p->failed = 1; p->reason = reason; }
}
static int fail(struct ppp_auth_probe *p, const char *reason) {
    ppp_auth_probe_fail(p, reason);
    return -1;
}
static void request(struct ppp_auth_probe *p) {
    if (!p->ready || p->failed || p->requests != p->rejects || p->requests == 5) return;
    unsigned i = p->requests;
    unsigned char packet[15] = {0xff,3,0xc0,0x21,1,(unsigned char)(0x51 + i),0,0};
    size_t n = options[i][1];
    packet[7] = (unsigned char)(4 + n);
    memcpy(packet + 8, options[i], n);
    uint16_t fcs = (uint16_t)~ppp_auth_fcs(0xffff, packet, 8 + n);
    packet[8 + n] = (unsigned char)fcs;
    packet[9 + n] = (unsigned char)(fcs >> 8);
    p->output_offset = 0; p->output_length = 0;
    p->output[p->output_length++] = 0x7e;
    for (size_t j = 0; j < 10 + n; j++) {
        unsigned char byte = packet[j];
        if (byte < 0x20 || byte == 0x7d || byte == 0x7e) {
            p->output[p->output_length++] = 0x7d; byte ^= 0x20;
        }
        p->output[p->output_length++] = byte;
    }
    p->output[p->output_length++] = 0x7e;
    p->requests++;
}
static int frame(struct ppp_auth_probe *p) {
    size_t n = p->used;
    unsigned char *b = p->frame;
    if (n < 10 || ppp_auth_fcs(0xffff, b, n) != 0xf0b8) return fail(p, "frame-fcs-length");
    if (b[0] != 0xff || b[1] != 3 || b[2] != 0xc0 || b[3] != 0x21)
        return fail(p, "unexpected-protocol");
    size_t length = (size_t)b[6] * 256 + b[7];
    if (length < 4 || length != n - 6) return fail(p, "lcp-length");
    if (b[4] == 1) {
        /* Validate CI structure, never acknowledge or echo its private bytes. */
        for (size_t i = 8; i < n - 2;) {
            if (n - 2 - i < 2 || b[i + 1] < 2 || b[i + 1] > n - 2 - i)
                return fail(p, "peer-option-length");
            i += b[i + 1];
        }
        p->peer_requests++; p->ready = 1;
        request(p);
        return 0;
    }
    if (b[4] != 4) return fail(p, b[4] == 2 ? "configure-ack" :
                                      b[4] == 3 ? "configure-nak" : "unexpected-lcp");
    if (!p->requests || p->rejects >= p->requests || p->output_offset != p->output_length)
        return fail(p, "unexpected-reject");
    unsigned i = p->rejects;
    if (b[5] != 0x51 + i) return fail(p, "reject-id");
    if (length != 4u + options[i][1] || memcmp(b + 8, options[i], options[i][1]))
        return fail(p, "reject-option");
    p->rejects++;
    request(p);
    return 0;
}
int ppp_auth_probe_input(struct ppp_auth_probe *p, const unsigned char *data, size_t length) {
    if (!p || (!data && length) || p->failed) return -1;
    if (length > PPP_AUTH_BYTE_MAX - p->received) return fail(p, "stdout-limit");
    p->received += length;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = data[i];
        if (byte == 0x7e) {
            if (p->escaped) return fail(p, "escape-abort");
            if (p->used && frame(p)) return -1;
            p->framed = 1; p->used = 0;
        } else if (byte < 0x20) {
            /* Initial receive ACCM: remove unescaped control bytes before FCS. */
            continue;
        } else {
            if (!p->framed) return fail(p, "unframed-input");
            if (!p->escaped && byte == 0x7d) { p->escaped = 1; continue; }
            if (p->escaped) { byte ^= 0x20; p->escaped = 0; }
            if (p->used == sizeof(p->frame)) return fail(p, "frame-limit");
            p->frame[p->used++] = byte;
        }
    }
    return 0;
}
const unsigned char *ppp_auth_probe_output(struct ppp_auth_probe *p, size_t *length) {
    if (length) *length = 0;
    if (!p || !length || p->failed || p->output_offset == p->output_length) return NULL;
    *length = p->output_length - p->output_offset;
    return p->output + p->output_offset;
}
int ppp_auth_probe_written(struct ppp_auth_probe *p, size_t count) {
    if (!p || p->failed) return -1;
    if (!count || count > p->output_length - p->output_offset) return fail(p, "write-count");
    if (count > PPP_AUTH_BYTE_MAX - p->sent) return fail(p, "stdin-limit");
    p->sent += count; p->output_offset += count;
    return 0;
}
