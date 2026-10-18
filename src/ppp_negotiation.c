#include "ppp_negotiation.h"
#include "ppp_auth_probe.h"
#include <string.h>

/* RFC 1662 framing values and the FCS-16 good residue. */
#define NEGO_FLAG 0x7eu
#define NEGO_ESCAPE 0x7du
#define NEGO_GOOD_FCS 0xf0b8u

static void nego_bump(uint32_t *counter) {
    if (*counter != UINT32_MAX) (*counter)++;
}
static void nego_reset(struct ppp_nego_variant *v) {
    memset(v->prefix, 0, sizeof(v->prefix));
    v->length = 0; v->fcs = 0xffff; v->escaped = v->overflow = 0;
}
void ppp_negotiation_init(struct ppp_negotiation *negotiation) {
    if (!negotiation) return;
    memset(negotiation, 0, sizeof(*negotiation));
    for (unsigned d = 0; d < PPP_NEGO_DIRECTIONS; d++) {
        nego_reset(&negotiation->lanes[d].strip);
        nego_reset(&negotiation->lanes[d].raw);
    }
}
void ppp_negotiation_wipe(struct ppp_negotiation *negotiation) {
    if (!negotiation) return;
    volatile unsigned char *lanes = (volatile unsigned char *)negotiation->lanes;
    for (size_t i = 0; i < sizeof(negotiation->lanes); i++) lanes[i] = 0;
}
static void nego_byte(struct ppp_nego_variant *v, unsigned char byte, int strip) {
    /* RFC 1662 4.2: unescaped ACCM characters are deleted before unescaping. */
    if (strip && byte < 0x20) return;
    if (byte == NEGO_ESCAPE) { v->escaped = 1; return; }
    if (v->escaped) { byte ^= 0x20; v->escaped = 0; }
    v->fcs = ppp_auth_fcs(v->fcs, &byte, 1);
    if (v->length < PPP_NEGO_PREFIX) v->prefix[v->length] = byte;
    if (v->length < PPP_NEGO_FRAME_LIMIT) v->length++;
    else v->overflow = 1;
}
static int nego_valid(const struct ppp_nego_variant *v) {
    /* Smallest frame: a compressed one-byte protocol plus the two FCS bytes. */
    return !v->escaped && !v->overflow && v->length >= 3 && v->fcs == NEGO_GOOD_FCS;
}
static enum ppp_nego_protocol nego_protocol(unsigned value) {
    switch (value) {
    case 0xc021: return PPP_NEGO_LCP;
    case 0xc023: return PPP_NEGO_PAP;
    case 0xc223: return PPP_NEGO_CHAP;
    case 0xc227: return PPP_NEGO_EAP;
    case 0x8021: return PPP_NEGO_IPCP;
    case 0x8057: return PPP_NEGO_IPV6CP;
    case 0x0021: return PPP_NEGO_IPV4;
    case 0x0057: return PPP_NEGO_IPV6;
    default: return PPP_NEGO_OTHER;
    }
}
/* p[at] is the LCP code. Walks option headers inside the held prefix and
 * reads only option 3's two protocol bytes; anything not fully inside the
 * prefix, or malformed, is UNKNOWN rather than NONE. */
static enum ppp_nego_auth nego_auth(const unsigned char *p, size_t at, size_t have, size_t total) {
    if (at + 4 > have) return PPP_NEGO_AUTH_UNKNOWN;
    size_t length = (size_t)p[at + 2] << 8 | p[at + 3];
    if (length < 4 || length > total - at) return PPP_NEGO_AUTH_UNKNOWN;
    size_t end = at + length, i = at + 4;
    while (i < end) {
        if (i + 2 > have || i + 2 > end) return PPP_NEGO_AUTH_UNKNOWN;
        size_t option = p[i + 1];
        if (option < 2 || option > end - i) return PPP_NEGO_AUTH_UNKNOWN;
        if (p[i] == 3) {
            if (option < 4 || i + 4 > have) return PPP_NEGO_AUTH_UNKNOWN;
            switch ((unsigned)p[i + 2] << 8 | p[i + 3]) {
            case 0xc023: return PPP_NEGO_AUTH_PAP;
            case 0xc223: return PPP_NEGO_AUTH_CHAP;
            case 0xc227: return PPP_NEGO_AUTH_EAP;
            default: return PPP_NEGO_AUTH_OTHER;
            }
        }
        i += option;
    }
    return PPP_NEGO_AUTH_NONE;
}
static enum ppp_nego_ncp nego_ncp(const struct ppp_nego_summary *s, enum ppp_nego_protocol p) {
    const uint32_t *peer = s->codes[PPP_NEGO_PEER][p], *local = s->codes[PPP_NEGO_LOCAL][p];
    if (peer[2] && local[2]) return PPP_NEGO_NCP_OPENED;
    if (peer[4]) return PPP_NEGO_NCP_REJECTED;
    if (peer[3]) return PPP_NEGO_NCP_NAK;
    for (unsigned c = 0; c < PPP_NEGO_CODES; c++)
        if (peer[c] || local[c]) return PPP_NEGO_NCP_CONFIGURING;
    return PPP_NEGO_NCP_NONE;
}
/* Appends one event, or folds a data-like frame into the matching event of the
 * current gap. Once anything was dropped, every later frame is dropped too, so
 * the kept events stay an exact prefix of the observed order. */
static void nego_trace(struct ppp_nego_trace *t, enum ppp_nego_direction d,
                       enum ppp_nego_protocol protocol, unsigned code) {
    int data = protocol == PPP_NEGO_IPV4 || protocol == PPP_NEGO_IPV6 || protocol == PPP_NEGO_OTHER;
    if (t->dropped) { nego_bump(&t->dropped); return; }
    if (data) {
        for (unsigned i = t->gap; i < t->length; i++) {
            struct ppp_nego_event *e = &t->events[i];
            if (e->direction == (unsigned char)d && e->protocol == (unsigned char)protocol) {
                nego_bump(&e->count); return;
            }
        }
    }
    if (t->length >= PPP_NEGO_TRACE_EVENTS) { nego_bump(&t->dropped); return; }
    t->events[t->length] = (struct ppp_nego_event){.count = 1, .direction = (unsigned char)d,
        .protocol = (unsigned char)protocol, .code = (unsigned char)code};
    t->length++;
    if (!data) t->gap = t->length;
}
static void nego_classify(struct ppp_nego_summary *s, enum ppp_nego_direction d,
                          const unsigned char *p, uint32_t frame_length) {
    /* Without the FCS; only the held prefix of it is readable. */
    size_t total = frame_length - 2, have = total < PPP_NEGO_PREFIX ? total : PPP_NEGO_PREFIX, at = 0;
    unsigned value = 0;
    int known = 0;
    if (have >= 2 && p[0] == 0xff && p[1] == 0x03) at = 2; /* ACFC when absent */
    if (at < have && (p[at] & 1)) { value = p[at]; at += 1; known = 1; } /* PFC */
    else if (at + 1 < have) { value = (unsigned)p[at] << 8 | p[at + 1]; at += 2; known = 1; }
    enum ppp_nego_protocol protocol = known ? nego_protocol(value) : PPP_NEGO_OTHER;
    unsigned code = at < have ? p[at] : 0, limit = 0;
    switch (protocol) {
    case PPP_NEGO_LCP: limit = 11; break;
    case PPP_NEGO_IPCP: case PPP_NEGO_IPV6CP: limit = 7; break;
    case PPP_NEGO_PAP: limit = 3; break;
    case PPP_NEGO_CHAP: case PPP_NEGO_EAP: limit = 4; break;
    default: limit = 0; break;
    }
    if (code > limit) code = 0;
    nego_bump(&s->codes[d][protocol][code]);
    nego_trace(&s->trace, d, protocol, code);
    if (protocol == PPP_NEGO_LCP) {
        if (code == 1 && d == PPP_NEGO_PEER) s->auth = nego_auth(p, at, have, total);
        if (code == 5) s->terminate |= d == PPP_NEGO_LOCAL ? PPP_NEGO_TERMINATE_LOCAL : PPP_NEGO_TERMINATE_PEER;
        s->lcp = s->codes[PPP_NEGO_PEER][PPP_NEGO_LCP][2] && s->codes[PPP_NEGO_LOCAL][PPP_NEGO_LCP][2] ?
            PPP_NEGO_LCP_OPENED : PPP_NEGO_LCP_CONFIGURING;
    } else if (protocol == PPP_NEGO_IPCP) s->ipcp = nego_ncp(s, PPP_NEGO_IPCP);
    else if (protocol == PPP_NEGO_IPV6CP) s->ipv6cp = nego_ncp(s, PPP_NEGO_IPV6CP);
    else if (protocol == PPP_NEGO_PAP || protocol == PPP_NEGO_CHAP || protocol == PPP_NEGO_EAP) {
        /* Results count only when the peer (the authenticator side) sends them. */
        unsigned success = protocol == PPP_NEGO_PAP ? 2 : 3, failure = success + 1;
        if (d == PPP_NEGO_PEER && code == success) s->auth_result = PPP_NEGO_RESULT_SUCCESS;
        else if (d == PPP_NEGO_PEER && code == failure) s->auth_result = PPP_NEGO_RESULT_FAILURE;
        else if (s->auth_result == PPP_NEGO_RESULT_NONE) s->auth_result = PPP_NEGO_RESULT_PENDING;
    }
}
static void nego_frame(struct ppp_negotiation *n, enum ppp_nego_direction d) {
    struct ppp_nego_lane *l = &n->lanes[d];
    const struct ppp_nego_variant *v = nego_valid(&l->strip) ? &l->strip :
        nego_valid(&l->raw) ? &l->raw : NULL;
    if (v) {
        nego_bump(&n->summary.frames[d]);
        nego_classify(&n->summary, d, v->prefix, v->length);
    } else if (l->strip.overflow || l->raw.overflow) nego_bump(&n->summary.too_long[d]);
    /* Bad FCS, an aborted (escape then flag) or a runt frame. */
    else if (l->strip.length || l->raw.length || l->strip.escaped || l->raw.escaped)
        nego_bump(&n->summary.fcs_errors[d]);
    nego_reset(&l->strip);
    nego_reset(&l->raw);
}
void ppp_negotiation_input(struct ppp_negotiation *negotiation, enum ppp_nego_direction direction,
                           const unsigned char *data, size_t length) {
    if (!negotiation || (unsigned)direction >= PPP_NEGO_DIRECTIONS || !data || !length) return;
    struct ppp_nego_lane *l = &negotiation->lanes[direction];
    uint64_t *bytes = &negotiation->summary.bytes[direction];
    *bytes = *bytes > UINT64_MAX - length ? UINT64_MAX : *bytes + length;
    for (size_t i = 0; i < length; i++) {
        if (data[i] == NEGO_FLAG) {
            if (l->synced) nego_frame(negotiation, direction);
            else { nego_reset(&l->strip); nego_reset(&l->raw); l->synced = 1; }
            continue;
        }
        if (!l->synced) continue;
        nego_byte(&l->strip, data[i], 1);
        nego_byte(&l->raw, data[i], 0);
    }
}
