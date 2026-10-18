#include "ppp_auth_probe.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL auth-probe line=%d\n", __LINE__); } } while (0)
static size_t encode(unsigned char *wire, const unsigned char *packet, size_t length) {
    uint16_t fcs = (uint16_t)~ppp_auth_fcs(0xffff, packet, length);
    size_t n = 0;
    wire[n++] = 0x7e;
    for (size_t i = 0; i < length + 2; i++) {
        unsigned char b = i < length ? packet[i] : i == length ? (unsigned char)fcs : (unsigned char)(fcs >> 8);
        if (b < 32 || b == 0x7e || b == 0x7d) { wire[n++] = 0x7d; b ^= 0x20; }
        wire[n++] = b;
    }
    wire[n++] = 0x7e;
    return n;
}
static size_t peer_request(unsigned char *wire) {
    const unsigned char packet[] = {255,3,0xc0,0x21,1,7,0,10,5,6,0x7d,0x7e,0,31};
    return encode(wire, packet, sizeof(packet));
}
static size_t decode(const unsigned char *wire, size_t length, unsigned char *packet) {
    size_t n = 0; int escaped = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char b = wire[i];
        if (b == 0x7e) continue;
        if (!escaped && b == 0x7d) { escaped = 1; continue; }
        packet[n++] = escaped ? b ^ 0x20 : b; escaped = 0;
    }
    return n;
}
static size_t response(struct ppp_auth_probe *p, unsigned char *wire, int code, int bad_id, int bad_option) {
    size_t length;
    const unsigned char *out = ppp_auth_probe_output(p, &length);
    CHECK(out != NULL);
    unsigned char packet[64];
    size_t n = decode(out, length, packet);
    CHECK(n >= 14 && ppp_auth_fcs(0xffff, packet, n) == 0xf0b8);
    packet[4] = (unsigned char)code;
    packet[5] += (unsigned char)bad_id;
    if (bad_option) packet[10] ^= 1;
    /* Exercise partial write: an early reply must not be accepted. */
    for (size_t i = 0; i < length; i++) CHECK(ppp_auth_probe_written(p, 1) == 0);
    return encode(wire, packet, n - 2);
}
int main(void) {
    CHECK(ppp_auth_fcs(0xffff, (const unsigned char *)"123456789", 9) == 0x6f91);
    unsigned char vector[] = {'1','2','3','4','5','6','7','8','9',0x6e,0x90};
    CHECK(ppp_auth_fcs(0xffff, vector, sizeof(vector)) == 0xf0b8);
    unsigned char wire[10000], peer[128];
    size_t n = peer_request(peer);
    for (size_t split = 0; split <= n; split++) {
        struct ppp_auth_probe p;
        ppp_auth_probe_init(&p);
        CHECK(ppp_auth_probe_input(&p, peer, split) == 0);
        CHECK(ppp_auth_probe_input(&p, peer + split, n - split) == 0);
        CHECK(p.ready && p.requests == 1 && p.peer_requests == 1);
        for (unsigned i = 0; i < 5; i++) {
            size_t r = response(&p, wire, 4, 0, 0);
            for (size_t j = 0; j < r; j++) CHECK(ppp_auth_probe_input(&p, wire + j, 1) == 0);
            CHECK(p.rejects == i + 1);
            if (i < 4) {
                CHECK(ppp_auth_probe_input(&p, peer, n) == 0);
                CHECK(p.requests == i + 2); /* retransmission cannot create a new probe */
            }
        }
        CHECK(p.rejects == 5 && p.requests == 5 && !p.failed);
        size_t length = 99;
        CHECK(ppp_auth_probe_output(&p, &length) == NULL && length == 0);
        CHECK(ppp_auth_probe_input(&p, wire, 0) == 0);
    }
    /* Multiple frames per read, raw receive-ACCM controls removed before FCS. */
    struct ppp_auth_probe p;
    ppp_auth_probe_init(&p);
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) { wire[pos++] = peer[i]; wire[pos++] = 0x11; }
    memcpy(wire + pos, peer, n); pos += n;
    CHECK(ppp_auth_probe_input(&p, wire, pos) == 0 && p.peer_requests == 2 && p.requests == 1);
    for (unsigned mode = 0; mode < 8; mode++) {
        ppp_auth_probe_init(&p);
        CHECK(ppp_auth_probe_input(&p, peer, n) == 0);
        size_t r = response(&p, wire, mode == 0 ? 2 : mode == 1 ? 3 : 4, mode == 2, mode == 3);
        if (mode == 4) wire[2] ^= 1;
        if (mode == 5) { unsigned char b[] = {0x7e,0x7d,0x7e}; memcpy(wire, b, sizeof(b)); r = sizeof(b); }
        if (mode == 6) { wire[0] = 0x55; }
        if (mode == 7) { /* exact reject is allowed once, duplicate is not */
            CHECK(ppp_auth_probe_input(&p, wire, r) == 0 && p.rejects == 1);
        }
        CHECK(ppp_auth_probe_input(&p, wire, r) == -1 && p.failed);
        unsigned requests = p.requests;
        CHECK(ppp_auth_probe_input(&p, peer, n) == -1 && p.requests == requests);
        size_t length;
        CHECK(ppp_auth_probe_output(&p, &length) == NULL);
    }
    const unsigned char bad_packets[][12] = {
        {255,3,0xc0,0x21,1,1,0,5,5,0,0,0}, /* mismatch */
        {255,3,0xc0,0x21,1,1,0,8,5,0,0,0}, /* CI zero */
        {255,3,0xc0,0x23,1,1,0,8,5,4,0,0}, /* auth protocol */
        {255,3,0xc0,0x21,1,1,0,8,5,9,0,0}, /* CI too long */
        {255,3,0xc0,0x21,1,1,0,3,5,4,0,0}  /* short LCP */
    };
    for (unsigned i = 0; i < sizeof(bad_packets) / sizeof(bad_packets[0]); i++) {
        ppp_auth_probe_init(&p);
        size_t length = encode(wire, bad_packets[i], sizeof(bad_packets[i]));
        CHECK(ppp_auth_probe_input(&p, wire, length) == -1);
    }
    ppp_auth_probe_init(&p);
    wire[0] = 0x7e; memset(wire + 1, 0x55, PPP_AUTH_FRAME_MAX + 1);
    CHECK(ppp_auth_probe_input(&p, wire, PPP_AUTH_FRAME_MAX + 2) == -1);
    CHECK(!strcmp(p.reason, "frame-limit"));
    ppp_auth_probe_init(&p);
    memset(wire, 0x7e, sizeof(wire));
    for (unsigned i = 0; i < 6; i++) CHECK(ppp_auth_probe_input(&p, wire, sizeof(wire)) == 0);
    CHECK(ppp_auth_probe_input(&p, wire, 5536) == 0);
    CHECK(ppp_auth_probe_input(&p, wire, 1) == -1 && p.received == PPP_AUTH_BYTE_MAX);
    ppp_auth_probe_init(&p);
    CHECK(ppp_auth_probe_input(&p, peer, n) == 0);
    CHECK(ppp_auth_probe_written(&p, 0) == -1);
    ppp_auth_probe_init(&p);
    CHECK(ppp_auth_probe_input(&p, peer, n) == 0);
    CHECK(ppp_auth_probe_written(&p, 65) == -1);
    ppp_auth_probe_init(&p);
    CHECK(ppp_auth_probe_input(&p, peer, n) == 0);
    /* Reject before the pending request has been completely written. */
    size_t length;
    const unsigned char *out = ppp_auth_probe_output(&p, &length);
    unsigned char packet[64];
    size_t plen = decode(out, length, packet); packet[4] = 4;
    size_t r = encode(wire, packet, plen - 2);
    CHECK(ppp_auth_probe_input(&p, wire, r) == -1);
    printf("ppp_auth_probe synthetic checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
