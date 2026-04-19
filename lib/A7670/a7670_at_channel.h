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
    AT_PROMPT    = 4,   /* '>' or "DOWNLOAD" received — payload may now be sent */
} AtResult_t;

/* ─────────────────────────── URC event types ────────────────────────────── */

/**
 * URC types emitted by the A7670E HTTP(S) service.
 * Source: A76XX AT Command Manual V1.09, Chapter 16.
 */
typedef enum {
    /* +HTTPACTION: <method>,<statuscode>,<datalen> — result of GET or POST */
    HTTP_URC_HTTPACTION    = 0,
    /* +HTTP_PEER_CLOSED — server closed the connection                     */
    HTTP_URC_PEER_CLOSED   = 1,
    /* +HTTP_NONET_EVENT — network became unavailable                       */
    HTTP_URC_NONET         = 2,
} HttpUrcType_t;

/**
 * URC event delivered via the queue.
 *
 * For HTTP_URC_HTTPACTION:
 *   method     — 0=GET, 1=POST, 2=HEAD, 3=DELETE, 4=PUT
 *   statuscode — HTTP status (200, 206, 404, …) or modem errcode (700–719)
 *   datalen    — response body length in bytes
 *
 * For HTTP_URC_PEER_CLOSED / HTTP_URC_NONET:
 *   method, statuscode, datalen are zero.
 */
typedef struct {
    HttpUrcType_t type;
    uint8_t       method;      /* GET=0, POST=1 (HTTPACTION only) */
    uint16_t      statuscode;  /* HTTP status or modem errcode    */
    uint32_t      datalen;     /* response body length            */
} HttpUrcEvent_t;

/* ─────────────────────────── Readiness result ───────────────────────────── */

typedef enum {
    AT_READY_OK          = 0,  /* modem alive, network registered, data attached */
    AT_READY_NO_MODEM    = 1,  /* AT echo test timed out                         */
    AT_READY_NO_NETWORK  = 2,  /* registration poll timed out                    */
    AT_READY_NO_ATTACH   = 3,  /* GPRS attach poll timed out                     */
    AT_READY_TIMEOUT     = 4,  /* overall deadline exceeded                      */
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

/**
 * @brief  Issue AT+HTTPREAD=<offset>,<size> and receive the response body.
 *
 * Must be called after a successful +HTTPACTION URC to drain the modem's
 * receive buffer.  The modem response sequence is:
 *
 *   OK\r\n                     (immediate — suppressed internally)
 *   +HTTPREAD: <actual_len>\r\n
 *   <actual_len bytes of data>
 *   \r\n
 *   +HTTPREAD: 0\r\n           (end marker — triggers AT_OK signal)
 *
 * Binary data bypasses line reassembly so firmware images with null bytes
 * and embedded \r\n sequences are handled correctly.
 *
 * @param[in]  offset      Byte offset into the response body (usually 0).
 * @param[in]  size        Number of bytes to request.
 * @param[out] buf         Output buffer; must be at least @p max_len bytes.
 * @param[in]  max_len     Buffer capacity (must be >= @p size).
 * @param[out] received    Actual bytes written to @p buf.
 * @param[in]  timeout_ms  Total operation timeout.
 * @return AT_OK on success, AT_ERROR or AT_TIMEOUT on failure.
 */
AtResult_t      at_channel_http_read(uint32_t  offset,
                                      uint16_t  size,
                                      uint8_t  *buf,
                                      uint16_t  max_len,
                                      uint16_t *received,
                                      uint32_t  timeout_ms);

void            at_channel_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __A7670_AT_CHANNEL_H */
