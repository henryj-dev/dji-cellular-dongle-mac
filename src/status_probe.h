#ifndef DJI_STATUS_PROBE_H
#define DJI_STATUS_PROBE_H

#include "at_probe.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Approved --probe-status: AT+CPIN?, AT+CSQ, AT+COPS?, AT+CEREG? on interface 2,
 * in this order, each at most once. Only per-command fixed fields are printed. */
#define STATUS_PROBE_COMMANDS 4u
/* Slot 4 is the --probe-pdp AT+CGDCONT? query; never sent by --probe-status. */
#define STATUS_PROBE_PDP_INDEX 4u
/* Slot 5 is the --probe-data-support capability test; never a data connection. */
#define STATUS_PROBE_DATA_SUPPORT_INDEX 5u
/* Slot 6 is AT+CESQ, sent only by the signal query before a PPP session. */
#define STATUS_PROBE_CESQ_INDEX 6u
#define STATUS_PROBE_SLOTS 7u
/* Numbers kept from one parsed CSQ (2) or CESQ (6) value line. */
#define STATUS_PROBE_VALUES 6u
#define STATUS_PROBE_APN_MAX 63u
#define STATUS_PROBE_MAX_LINES 16u
#define STATUS_PROBE_CME_MAX 40u
#define STATUS_PROBE_CPIN_MAX 20u
#define STATUS_PROBE_OPER_MAX 32u
/* A text field holding this many digits in total is treated as a possible identifier. */
#define STATUS_PROBE_DIGIT_RUN 7u

enum status_result {
    STATUS_RESULT_NONE,
    STATUS_RESULT_OK,
    STATUS_RESULT_ERROR,
    STATUS_RESULT_CME_ERROR
};

/* Reply attribution. MATCHED means only that the reply has the expected shape (own echo
 * first, one final line last, nothing foreign). It does not prove the reply belongs to the
 * request just sent: an identical leftover reply cannot be ruled out. MISMATCH means the
 * shape contradicts the request; it does not identify a SIM, network or APN cause. */
enum status_attribution {
    STATUS_ATTRIBUTION_UNPROVEN,
    STATUS_ATTRIBUTION_MISMATCH,
    STATUS_ATTRIBUTION_MATCHED
};

/* Mismatch reasons come first in priority order, then unproven reasons in priority order.
 * Any mismatch reason wins over any unproven reason. */
enum status_attribution_reason {
    STATUS_REASON_NOT_ANALYZED,
    STATUS_REASON_FORMAT_ONLY,        /* matched */
    STATUS_REASON_FOREIGN_ECHO,       /* another command's echo, or any other AT... line */
    STATUS_REASON_DUP_ECHO,           /* own echo more than once */
    STATUS_REASON_FINAL_BEFORE_ECHO,  /* a final line before the own echo */
    STATUS_REASON_STALE_BEFORE_ECHO,  /* own/known prefix, CMS error or plain line before own echo */
    STATUS_REASON_FOREIGN_PREFIX,     /* another listed command's response prefix */
    STATUS_REASON_MULTI_FINAL,        /* more than one final line */
    STATUS_REASON_IO_ERROR,           /* read stopped by an error, cancellation or disconnect */
    STATUS_REASON_TRUNCATED,          /* joined buffer overflowed */
    STATUS_REASON_OMITTED,            /* more lines than the line cap */
    STATUS_REASON_NO_ECHO,
    STATUS_REASON_PARTIAL,            /* unterminated tail */
    STATUS_REASON_NO_FINAL,
    STATUS_REASON_TRAILING,           /* lines after the final line */
    STATUS_REASON_CMS_ERROR,          /* +CMS ERROR: line; not a final line for this reader */
    STATUS_REASON_UNLISTED_PREFIX,    /* + line with no listed prefix; cause not asserted */
    STATUS_REASON_OTHER_LINE          /* any other unrecognized line */
};

/* Why a run stopped sending. Every reason except NONE blocks all later slots of the run.
 * TIMEOUT is final for the run: after a read timeout the SDK may need a data-toggle
 * resynchronization, so no later read, write or validate happens (close/release only).
 * Logical stops (timeout, time-budget, read-cap, zero-count, unconfirmed, final-error)
 * keep the DJI_OK return; the stop reason tells them apart from a full run. */
enum status_stop_reason {
    STATUS_STOP_NONE,          /* every slot completed: MATCHED with a final OK */
    STATUS_STOP_TIMEOUT,       /* a read timed out */
    STATUS_STOP_IO_ERROR,      /* read/validate error or disconnect: existing error code */
    STATUS_STOP_CANCELLED,
    STATUS_STOP_CLOCK_ERROR,   /* sentinel, backwards, overflow or impossible remaining time */
    STATUS_STOP_TIME_BUDGET,   /* 2 s read budget used up before completion */
    STATUS_STOP_READ_CAP,      /* four reads without own echo followed by a final line */
    STATUS_STOP_ZERO_COUNT,    /* successful read of zero bytes */
    STATUS_STOP_OVERSIZE,      /* successful read reporting more than the buffer */
    STATUS_STOP_WRITE_ERROR,   /* write failed or timed out: delivery unknown */
    STATUS_STOP_UNCONFIRMED,   /* completed, but attribution is not MATCHED */
    STATUS_STOP_FINAL_ERROR    /* MATCHED ERROR or +CME ERROR */
};

struct status_command_result {
    unsigned sent;
    unsigned reads;
    unsigned lines;
    unsigned omitted;
    unsigned contexts;
    unsigned support_lists;
    /* Run path only: complete lines before the first own echo, skipped unread. */
    unsigned pre_echo_skipped;
    /* Run path only: own echo then a terminated final line were collected. */
    unsigned complete;
    /* CSQ and CESQ only: numbers of the last printed value line, in reply order
     * (CSQ rssi,ber; CESQ rxlev,ber,rscp,ecno,rsrq,rsrp). values is how many. */
    unsigned values;
    unsigned value[STATUS_PROBE_VALUES];
    /* Own-prefix lines met while printing (parsed or not); a single one is required
     * before the numbers are trusted. */
    unsigned value_lines;
    enum status_result result;
    enum status_attribution attribution;
    enum status_attribution_reason attribution_reason;
};

struct status_probe_result {
    unsigned commands_sent;
    enum status_stop_reason stop;
    unsigned stop_index;       /* slot that stopped the run; STATUS_PROBE_SLOTS when none */
    struct status_command_result commands[STATUS_PROBE_SLOTS];
};

/* Fixed send order; index 0..3 maps to CPIN, CSQ, COPS, CEREG. */
enum at_probe_command status_probe_command(unsigned index);
const char *status_probe_name(unsigned index);
/* True when the joined bytes hold a CR/LF-terminated OK, ERROR or +CME ERROR: line. */
int status_has_final_line(const uint8_t *bytes, size_t size);
/* index 0..3 are status commands; the PDP and DATA_SUPPORT indices are separate queries.
 * Direct parsing keeps the strict whole-reply meaning: a terminated line before the own
 * echo is analyzed and printed, and it blocks MATCHED (stale-before-echo, final-before-echo).
 * The run path differs: there those lines are skipped unread (length/count only) and only
 * an own prefix among them blocks MATCHED. Prints only the approved fields of one command's
 * joined reply. Lines without a trailing CR/LF are partial and never parsed. Updates
 * lines/omitted/result and decides attribution before printing; on MISMATCH the own-prefix
 * lines are withheld (length only). Ends with one fixed attribution line. Attribution is
 * set even when stream is NULL. */
void status_print_lines(unsigned index, const uint8_t *bytes, size_t size, FILE *stream,
                        struct status_command_result *command);
int status_probe_run(const struct at_probe_backend *backend, struct status_probe_result *result,
                     FILE *stream);
/* --probe-pdp: AT+CGDCONT? once. cid, PDP type and APN only; address presence/length only. */
int pdp_probe_run(const struct at_probe_backend *backend, struct status_probe_result *result,
                  FILE *stream);

/* --probe-data-support: AT+CGDATA=? once. Known L2P capability only, not a PPP connection. */
int data_support_probe_run(const struct at_probe_backend *backend, struct status_probe_result *result,
                           FILE *stream);

/* Signal query before a PPP session: AT+CSQ then AT+CESQ, each at most once, same
 * stop rules as --probe-status. A NULL stream still parses (output is discarded). */
int signal_probe_run(const struct at_probe_backend *backend, struct status_probe_result *result,
                     FILE *stream);

#endif
