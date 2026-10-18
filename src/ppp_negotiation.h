#ifndef DJI_PPP_NEGOTIATION_H
#define DJI_PPP_NEGOTIATION_H

#include <stddef.h>
#include <stdint.h>

/* Passive, observe-only RFC 1662 HDLC-like frame classifier for diagnostics.
 * It is fed copies of bytes the relay already accepted and never changes,
 * answers, injects or blocks anything; its state never feeds relay decisions.
 * Only fixed-size counters and labels are kept: no payload, option value,
 * address, DNS, authentication name/challenge/response, raw or hex bytes. */
enum ppp_nego_direction {
    PPP_NEGO_PEER,  /* USB to pppd: frames the modem side sent */
    PPP_NEGO_LOCAL, /* pppd to USB: frames the local daemon sent */
    PPP_NEGO_DIRECTIONS
};
enum ppp_nego_protocol {
    PPP_NEGO_LCP, PPP_NEGO_PAP, PPP_NEGO_CHAP, PPP_NEGO_EAP, PPP_NEGO_IPCP,
    PPP_NEGO_IPV6CP, PPP_NEGO_IPV4, PPP_NEGO_IPV6, PPP_NEGO_OTHER, PPP_NEGO_PROTOCOLS
};
/* Index 0 counts frames without a counted code (data protocols, other
 * protocols and out-of-range codes); 1..11 are the RFC 1661 code values. */
#define PPP_NEGO_CODES 12u
enum ppp_nego_lcp { PPP_NEGO_LCP_NONE, PPP_NEGO_LCP_CONFIGURING, PPP_NEGO_LCP_OPENED };
enum ppp_nego_ncp {
    PPP_NEGO_NCP_NONE, PPP_NEGO_NCP_CONFIGURING, PPP_NEGO_NCP_NAK, PPP_NEGO_NCP_REJECTED,
    PPP_NEGO_NCP_OPENED
};
/* Protocol named by the peer's latest LCP Configure-Request option 3. Only the
 * two protocol bytes are read; the rest of the option is never inspected. */
enum ppp_nego_auth {
    PPP_NEGO_AUTH_UNKNOWN, PPP_NEGO_AUTH_NONE, PPP_NEGO_AUTH_PAP, PPP_NEGO_AUTH_CHAP,
    PPP_NEGO_AUTH_EAP, PPP_NEGO_AUTH_OTHER
};
enum ppp_nego_auth_result {
    PPP_NEGO_RESULT_NONE, PPP_NEGO_RESULT_PENDING, PPP_NEGO_RESULT_SUCCESS,
    PPP_NEGO_RESULT_FAILURE
};
#define PPP_NEGO_TERMINATE_LOCAL 1u
#define PPP_NEGO_TERMINATE_PEER 2u
/* Diagnostic-only order of classified frames; never a decision input. No time
 * is stored (the observer has no clock) and no payload or option value: only
 * direction, protocol and the counted code cell. Control frames (LCP, PAP,
 * CHAP, EAP, IPCP, IPV6CP) take one event each. Data-like frames (IPv4, IPv6
 * and other protocols, which have no counted code) fold into one event per
 * direction and protocol between two control events and count repeats, so
 * data traffic cannot crowd control events out; their relative order inside
 * one such gap is first-occurrence order only.
 * Overflow policy: the first PPP_NEGO_TRACE_EVENTS events are kept and every
 * later frame only adds to dropped (no ring). A once trial lasts at most 60 s
 * and the question it answers (what happened between IPCP Opened and the
 * address loss) lies in its first few dozen frames, so a gap-free prefix of
 * the true order is more useful than a tail behind a hole; totals such as
 * terminate and echo counts stay in codes[]. */
#define PPP_NEGO_TRACE_EVENTS 48u
struct ppp_nego_event {
    uint32_t count;          /* frames folded into this event, >= 1, saturating */
    unsigned char direction; /* enum ppp_nego_direction */
    unsigned char protocol;  /* enum ppp_nego_protocol */
    unsigned char code;      /* counted code cell; 0 for data-like protocols */
};
struct ppp_nego_trace {
    struct ppp_nego_event events[PPP_NEGO_TRACE_EVENTS];
    uint32_t dropped;        /* frames not recorded after the trace filled, saturating */
    unsigned char length;    /* events in use */
    unsigned char gap;       /* first event after the latest control event */
};
/* Plain value type: copied by value into observations. Counters saturate.
 * lcp/ncp OPENED means a Configure-Ack was seen in both directions at some
 * point, not that the link is still open. NAK/REJECTED count only frames the
 * peer sent. */
struct ppp_nego_summary {
    uint64_t bytes[PPP_NEGO_DIRECTIONS];
    uint32_t frames[PPP_NEGO_DIRECTIONS], fcs_errors[PPP_NEGO_DIRECTIONS];
    uint32_t too_long[PPP_NEGO_DIRECTIONS];
    uint32_t codes[PPP_NEGO_DIRECTIONS][PPP_NEGO_PROTOCOLS][PPP_NEGO_CODES];
    enum ppp_nego_lcp lcp;
    enum ppp_nego_ncp ipcp, ipv6cp;
    enum ppp_nego_auth auth;
    enum ppp_nego_auth_result auth_result;
    unsigned terminate;
    struct ppp_nego_trace trace;
};
/* Only a short prefix of each frame is held, and it is zeroed as soon as the
 * frame is classified. Longer frames keep a length count and running FCS. */
#define PPP_NEGO_PREFIX 64u
#define PPP_NEGO_FRAME_LIMIT 4096u
struct ppp_nego_variant {
    unsigned char prefix[PPP_NEGO_PREFIX];
    uint32_t length;
    uint16_t fcs;
    unsigned char escaped, overflow;
};
/* Two decodings per direction: "strip" deletes unescaped control bytes per
 * the initial all-ones ACCM, "raw" keeps them for a link that negotiated a
 * smaller ACCM. ACCM option values are not read. */
struct ppp_nego_lane {
    struct ppp_nego_variant strip, raw;
    int synced;
};
struct ppp_negotiation {
    struct ppp_nego_lane lanes[PPP_NEGO_DIRECTIONS];
    struct ppp_nego_summary summary;
};
void ppp_negotiation_init(struct ppp_negotiation *negotiation);
/* Clears the partial frame prefixes (which may hold credential bytes of an
 * unfinished local authentication frame) through a volatile store, so the
 * clear is not elided before free. The summary counters and trace are kept. */
void ppp_negotiation_wipe(struct ppp_negotiation *negotiation);
/* Never fails and has no result: invalid arguments are ignored. Bytes before
 * a direction's first flag are not classified. */
void ppp_negotiation_input(struct ppp_negotiation *negotiation, enum ppp_nego_direction direction,
                           const unsigned char *data, size_t length);

#endif
