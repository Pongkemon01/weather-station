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

/** Line-reassembly accumulator — must fit the longest modem response line. */
#define AT_LINE_BUF_SIZE        128u

/** Recommended depth of the caller-supplied URC queue. */
#define AT_URC_QUEUE_DEPTH      8u

/** Poll interval when waiting for network registration (ms). */
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
 * Source: A76XX AT Command Manual V1.09, Section 19.4.
 */
typedef enum {
    /* ── Deferred command results (arrive after the immediate OK) ─────── */
    HTTP_URC_CCHSTART         = 0,  /* +CCHSTART:<err>             */
    HTTP_URC_CCHSTOP          = 1,  /* +CCHSTOP:<err>              */
    HTTP_URC_CCHOPEN          = 2,  /* +CCHOPEN:<id>,<err>         */
    HTTP_URC_CCHCLOSE         = 3,  /* +CCHCLOSE:<id>,<err>        */

    /* ── True spontaneous URCs (section 19.4) ─────────────────────────── */
    HTTP_URC_CCHEVENT         = 4,  /* +CCHEVENT:<id>,RECV EVENT   */
    HTTP_URC_CCH_RECV_CLOSED  = 5,  /* +CCH_RECV_CLOSED:<id>,<err> */
    HTTP_URC_CCHSEND_RESULT   = 6,  /* +CCHSEND:<id>,<err>         */
    HTTP_URC_CCH_PEER_CLOSED  = 7,  /* +CCH_PEER_CLOSED:<id>       */
    HTTP_URC_CCH_STOP         = 8,  /* +CCH: CCH STOP              */
} HttpUrcType_t;

/**
 * URC event delivered via the queue.
 * client_idx: session_id (0 or 1), or -1 when not applicable.
 * param:      error code for result URCs; 0 for CCHEVENT/PEER_CLOSED/CCH_STOP.
 */
typedef struct {
    HttpUrcType_t type;
    int8_t        client_idx;
    int8_t        param;
} HttpUrcEvent_t;   /* 3 bytes — fits in one queue slot */

/* ─────────────────────────── Readiness result ───────────────────────────── */

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

bool            at_channel_init(UART_Ctx_t *uart_ctx, QueueHandle_t urc_queue);
AtResult_t      at_channel_ping_modem(uint32_t total_timeout_ms, uint8_t at_alive_retries);
AtReadyResult_t at_channel_wait_ready(uint32_t total_timeout_ms, uint8_t at_alive_retries);
void            at_channel_set_capture(char *buf, uint16_t size);
AtResult_t      at_channel_send_cmd(const char *cmd, uint32_t timeout_ms);
AtResult_t      at_channel_send_binary(const char *cmd, const uint8_t *data,
                                        size_t len, uint32_t timeout_ms);
void            at_channel_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __A7670_AT_CHANNEL_H */
