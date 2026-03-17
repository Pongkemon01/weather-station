#ifndef __A7670_AT_CHANNEL_H
#define __A7670_AT_CHANNEL_H

/**
 * @file    a7670_at_channel.h
 * @brief   AT channel public API — line reassembly, URC dispatch,
 *          command sequencing, and modem/network readiness.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "uart_subsystem.h"
#include "FreeRTOS.h"
#include "queue.h"

/* ─────────────────────────── Tuning ─────────────────────────────────────── */

/** Line-reassembly accumulator — must fit the longest modem response line.   */
#define AT_LINE_BUF_SIZE        128u

/** Depth of the caller-supplied URC queue (recommendation).                  */
#define AT_URC_QUEUE_DEPTH      8u

/**
 * Readiness poll interval when waiting for network registration.
 * AT+CGREG / AT+CEREG are polled on this cadence.
 */
#define AT_REG_POLL_MS          2000u

/* ─────────────────────────── AT result codes ────────────────────────────── */

typedef enum {
    AT_OK        = 0,
    AT_ERROR     = 1,
    AT_CME_ERROR = 2,
    AT_TIMEOUT   = 3,
    AT_PROMPT    = 4,   /* '>' received — binary payload may now be sent */
} AtResult_t;

/* ─────────────────────────── URC event types ────────────────────────────── */

/**
 * Every URC the modem can emit on the SSL/CCH channel.
 *
 * Source: A76XX AT Command Manual V1.09, Section 19.4, plus deferred
 * results for AT+CCHSTART and AT+CCHSTOP which use the same post-OK
 * notification pattern as AT+CCHOPEN.
 *
 * What changed from the previous version
 * ───────────────────────────────────────
 * ADDED   (were missing, section 19.4):
 *   HTTP_URC_CCH_RECV_CLOSED  — +CCH_RECV_CLOSED:<id>,<err>
 *   HTTP_URC_CCHSEND_RESULT   — +CCHSEND:<id>,<err>
 *   HTTP_URC_CCH_PEER_CLOSED  — +CCH_PEER_CLOSED:<id>
 *   HTTP_URC_CCH_STOP         — +CCH: CCH STOP
 *   HTTP_URC_CCHSTART         — +CCHSTART:<err>  (deferred result)
 *   HTTP_URC_CCHSTOP          — +CCHSTOP:<err>   (deferred result)
 *
 * REMOVED (were wrong):
 *   HTTP_URC_CCHRECV  — +CCHRECV:DATA is an inline AT response,
 *                        not a spontaneous URC.
 *   HTTP_URC_NO_CARRIER — circuit-switched; unrelated to CCH/SSL.
 *
 * KEPT:
 *   HTTP_URC_CCHOPEN  — +CCHOPEN:<id>,<err>  (deferred result of
 *                        AT+CCHOPEN; forwarded to the URC queue so
 *                        http_subsystem can poll for it)
 *   HTTP_URC_CCHCLOSE — +CCHCLOSE:<id>,<err> (deferred result of
 *                        AT+CCHCLOSE; same pattern)
 *   HTTP_URC_CCHEVENT — +CCHEVENT:<id>,RECV EVENT  (section 19.4 #1)
 */
typedef enum {
    /* ── Deferred command results (arrive after the immediate OK) ─────── */
    HTTP_URC_CCHSTART         = 0,  /* +CCHSTART:<err>           */
    HTTP_URC_CCHSTOP          = 1,  /* +CCHSTOP:<err>            */
    HTTP_URC_CCHOPEN          = 2,  /* +CCHOPEN:<id>,<err>       */
    HTTP_URC_CCHCLOSE         = 3,  /* +CCHCLOSE:<id>,<err>      */

    /* ── True spontaneous URCs (section 19.4) ─────────────────────────── */
    HTTP_URC_CCHEVENT         = 4,  /* +CCHEVENT:<id>,RECV EVENT */
    HTTP_URC_CCH_RECV_CLOSED  = 5,  /* +CCH_RECV_CLOSED:<id>,<err> */
    HTTP_URC_CCHSEND_RESULT   = 6,  /* +CCHSEND:<id>,<err>       */
    HTTP_URC_CCH_PEER_CLOSED  = 7,  /* +CCH_PEER_CLOSED:<id>     */
    HTTP_URC_CCH_STOP         = 8,  /* +CCH: CCH STOP            */
} HttpUrcType_t;

/**
 * URC event delivered via the queue.
 *
 * client_idx: session_id (0 or 1), or -1 when not applicable
 *             (e.g. HTTP_URC_CCH_STOP).
 * param:      error code for result URCs; 0 for CCHEVENT/PEER_CLOSED/CCH_STOP.
 */
typedef struct {
    HttpUrcType_t type;
    int8_t        client_idx;
    int8_t        param;
} HttpUrcEvent_t;   /* 3 bytes — fits in one queue slot */

/* ─────────────────────────── Readiness result ───────────────────────────── */

/**
 * Result of at_channel_wait_ready() — the modem startup / network
 * registration + CCH service activation sequence.
 */
typedef enum {
    AT_READY_OK              = 0,  /* modem alive, network registered, CCH started */
    AT_READY_NO_MODEM        = 1,  /* AT echo test timed out                       */
    AT_READY_NO_NETWORK      = 2,  /* registration poll timed out                  */
    AT_READY_NO_ATTACH       = 3,  /* GPRS attach poll timed out                   */
    AT_READY_CCHSTART_FAIL   = 4,  /* +CCHSTART returned non-zero error            */
    AT_READY_TIMEOUT         = 5,  /* overall deadline exceeded                    */
} AtReadyResult_t;

/* ─────────────────────────── Public API ─────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the AT channel.
 *
 * @param uart_ctx  UART context registered with UART_Sys_Register().
 * @param urc_queue Caller-created queue for HttpUrcEvent_t items.
 *                  Recommended depth: AT_URC_QUEUE_DEPTH.
 * @return true on success.
 */
bool at_channel_init(UART_Ctx_t *uart_ctx, QueueHandle_t urc_queue);

/**
 * Send AT (echo test) command to check for modem aliveness.
 *
 * It blocks until success or a deadline is reached.
 *
 * @param total_timeout_ms  Hard deadline for the entire sequence (ms).
 * @param at_alive_retries  How many times to retry the AT echo test.
 * @return AtResult_t
 */
AtResult_t at_channel_ping_modem(uint32_t total_timeout_ms,
                                  uint8_t  at_alive_retries);

/**
 * Wait for the modem and cellular network to be ready, then start the
 * CCH/SSL service (AT+CCHSTART).
 *
 * This must be called once after at_channel_init() and before any SSL
 * operations. It blocks until success or a deadline is reached.
 *
 * Sequence performed internally:
 *   1. AT               — modem echo test (up to at_alive_retries attempts)
 *   2. AT+CGREG? / AT+CEREG?   — wait for stat=1 or 5
 *   3. AT+CGATT?        — wait for state=1
 *   4. AT+CCHSTART      — activate PDP and start SSL service;
 *                          wait for deferred +CCHSTART:0 URC
 *
 * @param total_timeout_ms  Hard deadline for the entire sequence (ms).
 * @param at_alive_retries  How many times to retry the AT echo test.
 * @return AtReadyResult_t
 */
AtReadyResult_t at_channel_wait_ready(uint32_t total_timeout_ms,
                                       uint8_t  at_alive_retries);

/**
 * Register an optional caller-supplied text capture buffer.
 *
 * When registered, informational response lines (not OK/ERROR/URCs) are
 * appended here, separated by '\n'. The buffer is always null-terminated.
 * Set buf=NULL to disable capture.
 *
 * Must be called while cmd_mutex is not held (i.e. not inside a send_cmd
 * call). Capture is active for the duration of the next send_cmd call and
 * is automatically cleared when expecting_response goes false.
 */
void at_channel_set_capture(char *buf, uint16_t size);

/**
 * Send an AT command string and wait for OK / ERROR / timeout.
 *
 * @param cmd        Null-terminated AT command (without trailing CRLF).
 * @param timeout_ms Response wait timeout in milliseconds.
 * @return AtResult_t
 */
AtResult_t at_channel_send_cmd(const char *cmd, uint32_t timeout_ms);

/**
 * Two-phase binary send: send @p cmd, wait for '>' prompt, then stream
 * @p data bytes, then wait for OK / ERROR.
 *
 * Used for AT+CCERTDOWN and AT+CCHSEND.
 *
 * @param cmd        Null-terminated command string (e.g. "AT+CCHSEND=0,256").
 * @param data       Binary payload.
 * @param len        Payload length in bytes.
 * @param timeout_ms Per-phase timeout (applied to both the prompt wait and
 *                   the final OK wait).
 * @return AtResult_t
 */
AtResult_t at_channel_send_binary(const char    *cmd,
                                   const uint8_t *data,
                                   size_t         len,
                                   uint32_t       timeout_ms);

/** Tear down the AT channel (deletes rx_task and FreeRTOS objects). */
void at_channel_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __A7670_AT_CHANNEL_H */
