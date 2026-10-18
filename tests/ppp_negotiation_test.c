#include "ppp_negotiation.h"
#include "ppp_auth_probe.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL negotiation line=%d\n", __LINE__); } } while (0)

/* Synthetic RFC 1662 generator. RAW leaves control bytes unescaped (a link
 * with ACCM 0), STRAY inserts unescaped XON/XOFF that an all-ones ACCM
 * receiver deletes, BAD_FCS corrupts the check sequence. */
enum { ESCAPED = 0, RAW = 1, STRAY = 2, BAD_FCS = 4 };
static unsigned char wire[16384], frame[8192];
static size_t encode(const unsigned char *data, size_t length, unsigned mode) {
    uint16_t fcs = (uint16_t)~ppp_auth_fcs(0xffff, data, length);
    if (mode & BAD_FCS) fcs ^= 0x0100;
    size_t n = 0;
    wire[n++] = 0x7e;
    for (size_t i = 0; i < length + 2; i++) {
        unsigned char b = i < length ? data[i] : i == length ? (unsigned char)fcs : (unsigned char)(fcs >> 8);
        if (b == 0x7e || b == 0x7d || (b < 0x20 && !(mode & RAW))) { wire[n++] = 0x7d; b ^= 0x20; }
        wire[n++] = b;
        if ((mode & STRAY) && i == 1) { wire[n++] = 0x11; wire[n++] = 0x13; }
    }
    wire[n++] = 0x7e;
    return n;
}
/* Address/control unless acfc, protocol compressed when pfc and < 0x100. */
static size_t packet(unsigned protocol, int acfc, int pfc, unsigned code,
                     const unsigned char *body, size_t body_length) {
    size_t n = 0, length = 4 + body_length;
    if (!acfc) { frame[n++] = 0xff; frame[n++] = 0x03; }
    if (pfc && protocol < 0x100) frame[n++] = (unsigned char)protocol;
    else { frame[n++] = (unsigned char)(protocol >> 8); frame[n++] = (unsigned char)protocol; }
    frame[n++] = (unsigned char)code; frame[n++] = 1;
    frame[n++] = (unsigned char)(length >> 8); frame[n++] = (unsigned char)length;
    if (body_length) memcpy(frame + n, body, body_length);
    return n + body_length;
}
static void feed(struct ppp_negotiation *n, enum ppp_nego_direction d, const unsigned char *data,
                 size_t length, size_t chunk) {
    for (size_t i = 0; i < length; i += chunk)
        ppp_negotiation_input(n, d, data + i, length - i < chunk ? length - i : chunk);
}
static void emit(struct ppp_negotiation *n, enum ppp_nego_direction d, unsigned protocol,
                 unsigned code, const unsigned char *body, size_t body_length, unsigned mode, size_t chunk) {
    size_t length = encode(frame, packet(protocol, 0, 0, code, body, body_length), mode);
    feed(n, d, wire, length, chunk);
}
static int prefixes_clear(const struct ppp_negotiation *n) {
    for (unsigned d = 0; d < PPP_NEGO_DIRECTIONS; d++)
        for (unsigned i = 0; i < PPP_NEGO_PREFIX; i++)
            if (n->lanes[d].strip.prefix[i] || n->lanes[d].raw.prefix[i]) return 0;
    return 1;
}
static int contains(const void *haystack, size_t length, const unsigned char *needle, size_t size) {
    const unsigned char *h = haystack;
    for (size_t i = 0; i + size <= length; i++) if (!memcmp(h + i, needle, size)) return 1;
    return 0;
}
#define PEER PPP_NEGO_PEER
#define LOCAL PPP_NEGO_LOCAL
static const unsigned char marker[] = {0xde, 0xad, 0xbe, 0xef};

/* Bidirectional LCP open, one byte per read, escaped option bytes. */
static void lcp_open_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    const unsigned char peer_options[] = {1, 4, 0x05, 0xdc, 3, 5, 0xc2, 0x23, 0x05,
        5, 6, 0xde, 0xad, 0xbe, 0xef};
    const unsigned char local_options[] = {2, 6, 0, 0, 0, 0, 5, 6, 0x7e, 0x7d, 0x11, 0x00};
    emit(&n, PEER, 0xc021, 1, peer_options, sizeof(peer_options), ESCAPED, 1);
    CHECK(n.summary.lcp == PPP_NEGO_LCP_CONFIGURING && n.summary.auth == PPP_NEGO_AUTH_CHAP);
    emit(&n, LOCAL, 0xc021, 1, local_options, sizeof(local_options), ESCAPED, 1);
    emit(&n, LOCAL, 0xc021, 2, peer_options, sizeof(peer_options), ESCAPED, 3);
    CHECK(n.summary.lcp == PPP_NEGO_LCP_CONFIGURING);
    emit(&n, PEER, 0xc021, 2, local_options, sizeof(local_options), ESCAPED, 7);
    CHECK(n.summary.lcp == PPP_NEGO_LCP_OPENED);
    CHECK(n.summary.frames[PEER] == 2 && n.summary.frames[LOCAL] == 2);
    CHECK(n.summary.codes[PEER][PPP_NEGO_LCP][1] == 1 && n.summary.codes[PEER][PPP_NEGO_LCP][2] == 1);
    CHECK(n.summary.codes[LOCAL][PPP_NEGO_LCP][1] == 1 && n.summary.codes[LOCAL][PPP_NEGO_LCP][2] == 1);
    CHECK(!n.summary.fcs_errors[PEER] && !n.summary.fcs_errors[LOCAL] && !n.summary.too_long[PEER]);
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_NONE && n.summary.terminate == 0);
    /* Echo, discard and code-reject codes are counted per direction. */
    emit(&n, PEER, 0xc021, 9, marker, sizeof(marker), ESCAPED, 64);
    emit(&n, LOCAL, 0xc021, 10, marker, sizeof(marker), ESCAPED, 64);
    emit(&n, LOCAL, 0xc021, 11, marker, sizeof(marker), ESCAPED, 64);
    emit(&n, PEER, 0xc021, 7, NULL, 0, ESCAPED, 64);
    emit(&n, PEER, 0xc021, 12, NULL, 0, ESCAPED, 64);
    CHECK(n.summary.codes[PEER][PPP_NEGO_LCP][9] == 1 && n.summary.codes[LOCAL][PPP_NEGO_LCP][10] == 1);
    CHECK(n.summary.codes[LOCAL][PPP_NEGO_LCP][11] == 1 && n.summary.codes[PEER][PPP_NEGO_LCP][7] == 1);
    CHECK(n.summary.codes[PEER][PPP_NEGO_LCP][0] == 1); /* out-of-range code */
    CHECK(prefixes_clear(&n) && !contains(&n, sizeof(n), marker, sizeof(marker)));
}
/* ACFC and PFC, data protocols, other protocol, frames sharing one flag. */
static void compression_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    unsigned char stream[1024]; size_t used = 0, length;
    length = encode(frame, packet(0x8021, 1, 1, 1, NULL, 0), ESCAPED);
    memcpy(stream + used, wire, length); used += length - 1; /* shared closing/opening flag */
    length = encode(frame, packet(0x21, 1, 1, 0x45, marker, sizeof(marker)), ESCAPED);
    memcpy(stream + used, wire, length); used += length;
    length = encode(frame, packet(0x57, 0, 1, 0x60, marker, sizeof(marker)), ESCAPED);
    memcpy(stream + used, wire, length); used += length;
    length = encode(frame, packet(0x0021, 0, 0, 0x45, marker, sizeof(marker)), ESCAPED);
    memcpy(stream + used, wire, length); used += length;
    length = encode(frame, packet(0x002b, 1, 0, 1, NULL, 0), ESCAPED);
    memcpy(stream + used, wire, length); used += length;
    feed(&n, PEER, stream, used, 5);
    CHECK(n.summary.frames[PEER] == 5 && !n.summary.fcs_errors[PEER]);
    CHECK(n.summary.codes[PEER][PPP_NEGO_IPCP][1] == 1 && n.summary.ipcp == PPP_NEGO_NCP_CONFIGURING);
    CHECK(n.summary.codes[PEER][PPP_NEGO_IPV4][0] == 2 && n.summary.codes[PEER][PPP_NEGO_IPV6][0] == 1);
    CHECK(n.summary.codes[PEER][PPP_NEGO_OTHER][0] == 1);
    CHECK(n.summary.bytes[PEER] == used && !n.summary.bytes[LOCAL] && !n.summary.frames[LOCAL]);
    CHECK(prefixes_clear(&n) && !contains(&n, sizeof(n), marker, sizeof(marker)));
}
/* Bad FCS, abort, runt, control text before the first flag, long frames. */
static void error_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    const unsigned char text[] = "\r\nCONNECT 150000000\r\nRDY";
    feed(&n, LOCAL, text, sizeof(text) - 1, 4);
    CHECK(!n.summary.frames[LOCAL] && !n.summary.fcs_errors[LOCAL] && n.summary.bytes[LOCAL] == sizeof(text) - 1);
    emit(&n, LOCAL, 0xc021, 1, NULL, 0, BAD_FCS, 2);
    CHECK(!n.summary.frames[LOCAL] && n.summary.fcs_errors[LOCAL] == 1 && n.summary.lcp == PPP_NEGO_LCP_NONE);
    const unsigned char aborted[] = {0x7e, 0xff, 0x03, 0xc0, 0x21, 0x7d, 0x7e};
    feed(&n, LOCAL, aborted, sizeof(aborted), 1);
    const unsigned char runt[] = {0x21, 0x7e, 0x7e};
    feed(&n, LOCAL, runt, sizeof(runt), 1);
    CHECK(n.summary.fcs_errors[LOCAL] == 3 && !n.summary.frames[LOCAL]);
    static unsigned char body[5000];
    memset(body, 0x5a, sizeof(body));
    emit(&n, PEER, 0x0021, 0x45, body, sizeof(body), ESCAPED, 997);
    CHECK(n.summary.too_long[PEER] == 1 && !n.summary.frames[PEER] && !n.summary.fcs_errors[PEER]);
    /* Longer than the held prefix but within the limit: still classified. */
    emit(&n, PEER, 0x0021, 0x45, body, 200, ESCAPED, 31);
    CHECK(n.summary.frames[PEER] == 1 && n.summary.codes[PEER][PPP_NEGO_IPV4][0] == 1);
    /* Recovery after the errors. */
    emit(&n, LOCAL, 0xc021, 1, NULL, 0, ESCAPED, 2);
    CHECK(n.summary.frames[LOCAL] == 1 && n.summary.lcp == PPP_NEGO_LCP_CONFIGURING);
    CHECK(prefixes_clear(&n));
    /* Counters saturate instead of wrapping. */
    n.summary.frames[LOCAL] = UINT32_MAX;
    emit(&n, LOCAL, 0xc021, 1, NULL, 0, ESCAPED, 2);
    CHECK(n.summary.frames[LOCAL] == UINT32_MAX);
}
/* Unescaped control bytes: deleted under the initial ACCM, kept for ACCM 0. */
static void accm_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    const unsigned char option[] = {3, 6, 0x0a, 0x00, 0x00, 0x01};
    emit(&n, PEER, 0x8021, 1, option, sizeof(option), RAW, 3);
    CHECK(n.summary.frames[PEER] == 1 && !n.summary.fcs_errors[PEER]);
    CHECK(n.summary.codes[PEER][PPP_NEGO_IPCP][1] == 1);
    emit(&n, LOCAL, 0x8021, 1, option, sizeof(option), STRAY, 1);
    CHECK(n.summary.frames[LOCAL] == 1 && !n.summary.fcs_errors[LOCAL]);
    CHECK(n.summary.codes[LOCAL][PPP_NEGO_IPCP][1] == 1);
    CHECK(prefixes_clear(&n));
}
/* Option 3 types from the peer's latest Configure-Request only. */
static void auth_option_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_UNKNOWN && n.summary.auth_result == PPP_NEGO_RESULT_NONE);
    const unsigned char pap[] = {1, 4, 0x05, 0xdc, 3, 4, 0xc0, 0x23};
    const unsigned char eap[] = {3, 4, 0xc2, 0x27};
    const unsigned char other[] = {3, 4, 0x12, 0x34};
    const unsigned char none[] = {1, 4, 0x05, 0xdc, 5, 6, 0xde, 0xad, 0xbe, 0xef};
    const unsigned char malformed[] = {1, 1, 3, 4, 0xc0, 0x23};
    const unsigned char truncated[] = {3, 5, 0xc2, 0x23};
    emit(&n, PEER, 0xc021, 1, pap, sizeof(pap), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_PAP);
    emit(&n, LOCAL, 0xc021, 1, eap, sizeof(eap), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_PAP); /* local requests never change it */
    emit(&n, PEER, 0xc021, 1, eap, sizeof(eap), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_EAP);
    emit(&n, PEER, 0xc021, 1, other, sizeof(other), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_OTHER);
    emit(&n, PEER, 0xc021, 1, none, sizeof(none), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_NONE);
    emit(&n, PEER, 0xc021, 1, malformed, sizeof(malformed), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_UNKNOWN);
    emit(&n, PEER, 0xc021, 1, truncated, sizeof(truncated), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_UNKNOWN);
    /* Option 3 past the held prefix is unknown, not none. */
    unsigned char beyond[74] = {0x10, 70};
    beyond[70] = 3; beyond[71] = 4; beyond[72] = 0xc0; beyond[73] = 0x23;
    emit(&n, PEER, 0xc021, 1, none, sizeof(none), ESCAPED, 2);
    emit(&n, PEER, 0xc021, 1, beyond, sizeof(beyond), ESCAPED, 9);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_UNKNOWN && n.summary.frames[PEER] == 8);
    /* Configure-Ack/Nak options are not the peer's own request. */
    emit(&n, PEER, 0xc021, 3, pap, sizeof(pap), ESCAPED, 2);
    CHECK(n.summary.auth == PPP_NEGO_AUTH_UNKNOWN);
    CHECK(prefixes_clear(&n) && !contains(&n, sizeof(n), marker, sizeof(marker)));
}
/* Authentication codes only; results only from the peer side. */
static void auth_result_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    emit(&n, PEER, 0xc223, 1, marker, sizeof(marker), ESCAPED, 3);
    CHECK(n.summary.auth_result == PPP_NEGO_RESULT_PENDING);
    emit(&n, LOCAL, 0xc223, 2, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, LOCAL, 0xc223, 3, NULL, 0, ESCAPED, 3); /* not the authenticator side */
    CHECK(n.summary.auth_result == PPP_NEGO_RESULT_PENDING);
    emit(&n, PEER, 0xc223, 3, NULL, 0, ESCAPED, 3);
    CHECK(n.summary.auth_result == PPP_NEGO_RESULT_SUCCESS);
    CHECK(n.summary.codes[PEER][PPP_NEGO_CHAP][1] == 1 && n.summary.codes[LOCAL][PPP_NEGO_CHAP][2] == 1);
    CHECK(n.summary.codes[PEER][PPP_NEGO_CHAP][3] == 1 && n.summary.codes[LOCAL][PPP_NEGO_CHAP][3] == 1);
    struct ppp_negotiation p; ppp_negotiation_init(&p);
    emit(&p, LOCAL, 0xc023, 1, marker, sizeof(marker), ESCAPED, 64);
    emit(&p, PEER, 0xc023, 3, NULL, 0, ESCAPED, 64);
    CHECK(p.summary.auth_result == PPP_NEGO_RESULT_FAILURE && p.summary.codes[PEER][PPP_NEGO_PAP][3] == 1);
    emit(&p, PEER, 0xc023, 2, NULL, 0, ESCAPED, 64);
    CHECK(p.summary.auth_result == PPP_NEGO_RESULT_SUCCESS);
    struct ppp_negotiation e; ppp_negotiation_init(&e);
    emit(&e, PEER, 0xc227, 1, marker, sizeof(marker), ESCAPED, 64);
    emit(&e, PEER, 0xc227, 4, NULL, 0, ESCAPED, 64);
    CHECK(e.summary.auth_result == PPP_NEGO_RESULT_FAILURE && e.summary.codes[PEER][PPP_NEGO_EAP][4] == 1);
    CHECK(prefixes_clear(&n) && !contains(&n, sizeof(n), marker, sizeof(marker)));
    CHECK(!contains(&p, sizeof(p), marker, sizeof(marker)) && !contains(&e, sizeof(e), marker, sizeof(marker)));
}
/* IPCP Nak/Reject/Ack stages, IPV6CP Protocol-Reject, Terminate direction. */
static void ncp_terminate_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    const unsigned char address[] = {3, 6, 0, 0, 0, 0};
    emit(&n, LOCAL, 0x8021, 1, address, sizeof(address), ESCAPED, 4);
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_CONFIGURING);
    emit(&n, PEER, 0x8021, 3, address, sizeof(address), ESCAPED, 4);
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_NAK);
    emit(&n, PEER, 0x8021, 4, address, sizeof(address), ESCAPED, 4);
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_REJECTED);
    emit(&n, LOCAL, 0x8021, 3, address, sizeof(address), ESCAPED, 4); /* local Nak does not count */
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_REJECTED);
    emit(&n, PEER, 0x8021, 2, address, sizeof(address), ESCAPED, 4);
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_REJECTED);
    emit(&n, LOCAL, 0x8021, 2, address, sizeof(address), ESCAPED, 4);
    CHECK(n.summary.ipcp == PPP_NEGO_NCP_OPENED);
    CHECK(n.summary.codes[PEER][PPP_NEGO_IPCP][3] == 1 && n.summary.codes[PEER][PPP_NEGO_IPCP][4] == 1);
    const unsigned char identifier[] = {1, 10, 0, 0, 0, 0, 0, 0, 0, 1};
    emit(&n, LOCAL, 0x8057, 1, identifier, sizeof(identifier), ESCAPED, 4);
    const unsigned char rejected[] = {0x80, 0x57};
    emit(&n, PEER, 0xc021, 8, rejected, sizeof(rejected), ESCAPED, 4);
    CHECK(n.summary.ipv6cp == PPP_NEGO_NCP_CONFIGURING && n.summary.codes[PEER][PPP_NEGO_LCP][8] == 1);
    CHECK(n.summary.terminate == 0);
    emit(&n, LOCAL, 0xc021, 5, NULL, 0, ESCAPED, 4);
    CHECK(n.summary.terminate == PPP_NEGO_TERMINATE_LOCAL);
    emit(&n, PEER, 0xc021, 6, NULL, 0, ESCAPED, 4);
    CHECK(n.summary.terminate == PPP_NEGO_TERMINATE_LOCAL);
    emit(&n, PEER, 0xc021, 5, NULL, 0, ESCAPED, 4);
    CHECK(n.summary.terminate == (PPP_NEGO_TERMINATE_LOCAL | PPP_NEGO_TERMINATE_PEER));
    struct ppp_negotiation v; ppp_negotiation_init(&v);
    emit(&v, LOCAL, 0x8057, 1, identifier, sizeof(identifier), ESCAPED, 64);
    emit(&v, PEER, 0x8057, 1, identifier, sizeof(identifier), ESCAPED, 64);
    emit(&v, PEER, 0x8057, 2, identifier, sizeof(identifier), ESCAPED, 64);
    CHECK(v.summary.ipv6cp == PPP_NEGO_NCP_CONFIGURING);
    emit(&v, LOCAL, 0x8057, 2, identifier, sizeof(identifier), ESCAPED, 64);
    CHECK(v.summary.ipv6cp == PPP_NEGO_NCP_OPENED && v.summary.ipcp == PPP_NEGO_NCP_NONE);
    emit(&v, PEER, 0xc021, 5, NULL, 0, ESCAPED, 64);
    CHECK(v.summary.terminate == PPP_NEGO_TERMINATE_PEER && v.summary.lcp == PPP_NEGO_LCP_CONFIGURING);
    CHECK(prefixes_clear(&n) && prefixes_clear(&v));
}
/* Interleaved split reads keep each direction's decoder independent. */
static void interleave_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    unsigned char a[256], b[256];
    size_t al = encode(frame, packet(0xc021, 0, 0, 1, marker, sizeof(marker)), ESCAPED);
    memcpy(a, wire, al);
    size_t bl = encode(frame, packet(0xc021, 0, 0, 2, marker, sizeof(marker)), ESCAPED);
    memcpy(b, wire, bl);
    for (size_t i = 0; i < al || i < bl; i++) {
        if (i < al) ppp_negotiation_input(&n, PEER, a + i, 1);
        if (i < bl) ppp_negotiation_input(&n, LOCAL, b + i, 1);
    }
    CHECK(n.summary.frames[PEER] == 1 && n.summary.frames[LOCAL] == 1);
    CHECK(n.summary.codes[PEER][PPP_NEGO_LCP][1] == 1 && n.summary.codes[LOCAL][PPP_NEGO_LCP][2] == 1);
    CHECK(n.summary.bytes[PEER] == al && n.summary.bytes[LOCAL] == bl);
}
static int event_is(const struct ppp_nego_trace *t, unsigned i, enum ppp_nego_direction d,
                    enum ppp_nego_protocol p, unsigned code, uint32_t count) {
    return i < t->length && t->events[i].direction == (unsigned char)d &&
        t->events[i].protocol == (unsigned char)p && t->events[i].code == code && t->events[i].count == count;
}
/* Order across both directions; data folds per direction/protocol between
 * control events; bad frames are not recorded. */
static void trace_order_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    const struct ppp_nego_trace *t = &n.summary.trace;
    CHECK(!t->length && !t->dropped);
    emit(&n, PEER, 0xc021, 1, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, LOCAL, 0xc021, 1, NULL, 0, ESCAPED, 3);
    emit(&n, LOCAL, 0xc021, 2, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, PEER, 0xc021, 2, NULL, 0, ESCAPED, 3);
    emit(&n, LOCAL, 0x8021, 1, NULL, 0, BAD_FCS, 3); /* not classified, not recorded */
    emit(&n, LOCAL, 0x8021, 1, NULL, 0, ESCAPED, 3);
    emit(&n, PEER, 0x8021, 2, NULL, 0, ESCAPED, 3);
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, PEER, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, LOCAL, 0x0057, 0x60, marker, sizeof(marker), ESCAPED, 3);
    /* A peer Configure-Request after Opened starts a new gap for data. */
    emit(&n, PEER, 0xc021, 1, NULL, 0, ESCAPED, 3);
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 3);
    emit(&n, PEER, 0xc021, 12, NULL, 0, ESCAPED, 3); /* out-of-range code: cell 0 */
    emit(&n, LOCAL, 0x002b, 1, NULL, 0, ESCAPED, 3);
    CHECK(t->length == 13 && !t->dropped && n.summary.fcs_errors[LOCAL] == 1);
    CHECK(event_is(t, 0, PEER, PPP_NEGO_LCP, 1, 1) && event_is(t, 1, LOCAL, PPP_NEGO_LCP, 1, 1));
    CHECK(event_is(t, 2, LOCAL, PPP_NEGO_LCP, 2, 1) && event_is(t, 3, PEER, PPP_NEGO_LCP, 2, 1));
    CHECK(event_is(t, 4, LOCAL, PPP_NEGO_IPCP, 1, 1) && event_is(t, 5, PEER, PPP_NEGO_IPCP, 2, 1));
    CHECK(event_is(t, 6, LOCAL, PPP_NEGO_IPV4, 0, 3) && event_is(t, 7, PEER, PPP_NEGO_IPV4, 0, 1));
    CHECK(event_is(t, 8, LOCAL, PPP_NEGO_IPV6, 0, 1) && event_is(t, 9, PEER, PPP_NEGO_LCP, 1, 1));
    CHECK(event_is(t, 10, LOCAL, PPP_NEGO_IPV4, 0, 1) && event_is(t, 11, PEER, PPP_NEGO_LCP, 0, 1));
    CHECK(t->gap == 12 && event_is(t, 12, LOCAL, PPP_NEGO_OTHER, 0, 1));
    /* Other protocols fold like data after the last control event. */
    emit(&n, LOCAL, 0x002b, 1, NULL, 0, ESCAPED, 3);
    CHECK(t->length == 13 && event_is(t, 12, LOCAL, PPP_NEGO_OTHER, 0, 2));
    /* Counts saturate. */
    n.summary.trace.events[12].count = UINT32_MAX;
    emit(&n, LOCAL, 0x002b, 1, NULL, 0, ESCAPED, 3);
    CHECK(t->length == 13 && t->events[12].count == UINT32_MAX);
    CHECK(prefixes_clear(&n) && !contains(&n, sizeof(n), marker, sizeof(marker)));
}
/* First PPP_NEGO_TRACE_EVENTS events kept; afterwards everything is dropped,
 * including data that would otherwise fold, so the prefix stays exact. */
static void trace_overflow_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    const struct ppp_nego_trace *t = &n.summary.trace;
    for (unsigned i = 0; i + 1 < PPP_NEGO_TRACE_EVENTS; i++)
        emit(&n, i % 2 ? LOCAL : PEER, 0xc021, 9 + i % 2, marker, sizeof(marker), ESCAPED, 64);
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 64);
    CHECK(t->length == PPP_NEGO_TRACE_EVENTS && !t->dropped);
    /* Full, nothing dropped yet: a repeat of the current gap still folds. */
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 64);
    CHECK(!t->dropped && event_is(t, PPP_NEGO_TRACE_EVENTS - 1, LOCAL, PPP_NEGO_IPV4, 0, 2));
    emit(&n, PEER, 0xc021, 5, NULL, 0, ESCAPED, 64);
    emit(&n, LOCAL, 0x0021, 0x45, marker, sizeof(marker), ESCAPED, 64);
    emit(&n, LOCAL, 0xc021, 6, NULL, 0, ESCAPED, 64);
    CHECK(t->length == PPP_NEGO_TRACE_EVENTS && t->dropped == 3);
    CHECK(event_is(t, PPP_NEGO_TRACE_EVENTS - 1, LOCAL, PPP_NEGO_IPV4, 0, 2));
    CHECK(event_is(t, 0, PEER, PPP_NEGO_LCP, 9, 1) && event_is(t, 1, LOCAL, PPP_NEGO_LCP, 10, 1));
    /* Counted totals keep the dropped frames. */
    CHECK(n.summary.codes[PEER][PPP_NEGO_LCP][5] == 1 && n.summary.terminate == PPP_NEGO_TERMINATE_PEER);
    n.summary.trace.dropped = UINT32_MAX;
    emit(&n, PEER, 0xc021, 1, NULL, 0, ESCAPED, 64);
    CHECK(t->dropped == UINT32_MAX && t->length == PPP_NEGO_TRACE_EVENTS);
    CHECK(prefixes_clear(&n) && !contains(&n, sizeof(n), marker, sizeof(marker)));
}
/* Invalid calls are ignored; a zeroed object behaves like an initialized one. */
static void argument_test(void) {
    struct ppp_negotiation n; ppp_negotiation_init(&n);
    struct ppp_negotiation copy; memcpy(&copy, &n, sizeof(n));
    const unsigned char flag = 0x7e;
    ppp_negotiation_input(NULL, PEER, &flag, 1);
    ppp_negotiation_input(&n, PPP_NEGO_DIRECTIONS, &flag, 1);
    ppp_negotiation_input(&n, PEER, NULL, 1);
    ppp_negotiation_input(&n, PEER, &flag, 0);
    ppp_negotiation_init(NULL);
    CHECK(!memcmp(&n, &copy, sizeof(n)));
    struct ppp_negotiation zero; memset(&zero, 0, sizeof(zero));
    emit(&zero, PEER, 0xc021, 1, NULL, 0, ESCAPED, 3);
    CHECK(zero.summary.frames[PEER] == 1 && zero.summary.auth == PPP_NEGO_AUTH_NONE);
}
/* An unfinished frame keeps its prefix until wipe; wipe clears it, keeps the summary. */
static void wipe_test(void) {
    struct ppp_negotiation n;
    ppp_negotiation_init(&n);
    emit(&n, LOCAL, 0xc021, 1, NULL, 0, ESCAPED, 64);
    size_t length = encode(frame, packet(0xc023, 0, 0, 1, marker, sizeof(marker)), ESCAPED);
    feed(&n, LOCAL, wire, length - 1, 64); /* no closing flag: still unfinished */
    CHECK(contains(n.lanes, sizeof(n.lanes), marker, sizeof(marker)));
    struct ppp_nego_summary before = n.summary;
    ppp_negotiation_wipe(&n);
    CHECK(!contains(&n, sizeof(n), marker, sizeof(marker)) && prefixes_clear(&n));
    CHECK(!memcmp(&before, &n.summary, sizeof(before)) && n.summary.frames[LOCAL] == 1);
    /* The order record is part of the kept summary, not of the wiped lanes. */
    CHECK(event_is(&n.summary.trace, 0, LOCAL, PPP_NEGO_LCP, 1, 1) && n.summary.trace.length == 1);
    ppp_negotiation_wipe(NULL);
}
int main(void) {
    lcp_open_test();
    wipe_test();
    compression_test();
    error_test();
    accm_test();
    auth_option_test();
    auth_result_test();
    ncp_terminate_test();
    interleave_test();
    trace_order_test();
    trace_overflow_test();
    argument_test();
    printf("ppp_negotiation synthetic checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
