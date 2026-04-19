/**
 * @file  a7670_https_uploader.h
 * @brief Weather data upload via the A7670E HTTP(S) service (AT+HTTP* commands).
 *
 * ── Service architecture ───────────────────────────────────────────────────
 *
 * Uses the A7670E standard HTTP(S) service (Chapter 16 of the AT command manual):
 *   AT+HTTPINIT  — start the HTTP service (activates PDP context)
 *   AT+HTTPPARA  — set URL, SSL context, content-type
 *   AT+HTTPDATA=<size>,<timeout> — prime the POST body buffer; modem replies
 *                                  "DOWNLOAD" (not ">"); then send binary body; OK
 *   AT+HTTPACTION=1 — issue POST; returns OK immediately; +HTTPACTION URC follows
 *   AT+HTTPTERM  — stop the HTTP service
 *
 * SSL is configured via AT+CSSLCFG (context 0) once in Modem_Module_Init()
 * and referenced here with AT+HTTPPARA="SSLCFG",0.
 *
 * ── Calling convention ────────────────────────────────────────────────────
 *
 *   https_uploader_start(urc_queue);
 *
 *   // One call per batch (≤ 512 B blob):
 *   https_uploader_post("https://host/upload.php", my_fetch_cb, ctx, blob_len);
 *
 *   https_uploader_stop();
 *
 * ── Fetch callback ────────────────────────────────────────────────────────
 *
 * The caller supplies a HttpsUlFetchCb_t that fills the upload buffer in one
 * shot.  There is no offset parameter — the entire blob (≤ HTTPS_UL_FETCH_WINDOW)
 * is assembled by the callback into the uploader's static s_fetch_buf[].
 *
 *   uint16_t my_cb(void *ctx, uint8_t *buf, uint16_t max_len)
 *   {
 *       // write header + records into buf[0..max_len-1]
 *       return bytes_written;   // 0 on error
 *   }
 *
 * The value returned by the callback MUST equal the @p len argument passed
 * to https_uploader_post().  A mismatch causes https_uploader_post() to
 * return HTTPS_UL_ERR_PARAM.
 *
 * ── Prerequisites ─────────────────────────────────────────────────────────
 *   modem_init() completed successfully.
 *   Certificates injected and AT+CSSLCFG context 0 configured by Modem_Module_Init().
 *
 * ── RAM layout ────────────────────────────────────────────────────────────
 *   s_fetch_buf[512]  512 B  assembled POST body (header + packed records)
 *   s_cmd_buf[256]    256 B  AT command assembly (HTTPPARA URL, HTTPDATA)
 *   Total static:     768 B
 *
 * ── Thread safety ─────────────────────────────────────────────────────────
 *   Not re-entrant.  Only SslUploadTask may call these functions.
 */

#ifndef A7670_HTTPS_UPLOADER_H
#define A7670_HTTPS_UPLOADER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ Tuning ══════════════════════════════════════════*/

/** Maximum POST body size in bytes.  Blob = header(5B) + N×18B packed records. */
#define HTTPS_UL_FETCH_WINDOW   512u

/** SSL context index configured by Modem_Module_Init() via AT+CSSLCFG. */
#define HTTPS_UL_SSL_CTX        0u

/** Maximum HTTPS URL length including scheme, host, path, query, and null. */
#define HTTPS_UL_URL_MAX_LEN    192u

/** Timeout for AT+HTTPINIT (activates PDP context, ms). */
#define HTTPS_UL_INIT_MS        10000u

/** Timeout for AT+HTTPPARA parameter commands (ms). */
#define HTTPS_UL_PARAM_MS       2000u

/**
 * Timeout for AT+HTTPDATA command response (ms).
 * The modem must reply "DOWNLOAD" within this window before binary body is sent.
 */
#define HTTPS_UL_DATA_MS        5000u

/**
 * Timeout waiting for +HTTPACTION: 1,<status>,<datalen> URC after AT+HTTPACTION=1.
 * Includes network round-trip + TLS handshake + server processing time.
 */
#define HTTPS_UL_ACTION_MS      60000u

/** Timeout for AT+HTTPTERM (ms). */
#define HTTPS_UL_STOP_MS        10000u

/* ══════════════════════════ Return codes ════════════════════════════════════*/

typedef enum {
    HTTPS_UL_OK          = 0,  /**< Success.                                         */
    HTTPS_UL_ERR_PARAM   = 1,  /**< NULL/invalid parameter, or callback length mismatch. */
    HTTPS_UL_ERR_INIT    = 2,  /**< AT+HTTPINIT failed.                              */
    HTTPS_UL_ERR_URL     = 3,  /**< AT+HTTPPARA="URL" failed.                        */
    HTTPS_UL_ERR_POST    = 4,  /**< AT+HTTPDATA or AT+HTTPACTION=1 failed, or
                                     +HTTPACTION URC timed out.                       */
    HTTPS_UL_ERR_HTTP    = 5,  /**< Server returned a non-2xx HTTP status.           */
    HTTPS_UL_ERR_STOP    = 6,  /**< AT+HTTPTERM failed (non-fatal; modem cleans up). */
} HttpsUlResult_t;

/* ══════════════════════════ Callback type ═══════════════════════════════════*/

/**
 * @brief  Fetch callback — fills the upload buffer with the complete POST body.
 *
 * Called once by https_uploader_post() before the AT+HTTPDATA command is sent.
 * The callback must write the entire blob into @p buf in a single call.
 *
 * @param  ctx      Caller-supplied context pointer (passed through unchanged).
 * @param  buf      Output buffer; capacity is HTTPS_UL_FETCH_WINDOW bytes.
 * @param  max_len  Buffer capacity (always HTTPS_UL_FETCH_WINDOW).
 * @return Number of bytes written into @p buf, or 0 on error.
 *         Must equal the @p len argument passed to https_uploader_post().
 */
typedef uint16_t (*HttpsUlFetchCb_t)(void *ctx, uint8_t *buf, uint16_t max_len);

/* ══════════════════════════ Public API ══════════════════════════════════════*/

/**
 * @brief  Start the HTTP service and configure SSL.
 *
 * Issues AT+HTTPINIT then AT+HTTPPARA="SSLCFG",HTTPS_UL_SSL_CTX.
 * Must be called once before any https_uploader_post() calls.
 *
 * @param  urc_queue  URC queue from modem_get_urc_queue() — stored internally
 *                    for the lifetime of the HTTP session.
 * @return HTTPS_UL_OK on success, or an error code.
 */
HttpsUlResult_t https_uploader_start(QueueHandle_t urc_queue);

/**
 * @brief  POST one blob to the server.
 *
 * Flow:
 *   1. AT+HTTPPARA="URL","<full_url>"
 *   2. AT+HTTPPARA="CONTENT","application/octet-stream"
 *   3. Invoke @p fetch_cb to fill s_fetch_buf[]; verify returned length == @p len.
 *   4. AT+HTTPDATA=<len>,30  →  wait for "DOWNLOAD" prompt (AT_PROMPT)
 *   5. Send @p len bytes of s_fetch_buf[] via at_channel_send_binary()
 *   6. AT+HTTPACTION=1  →  wait for +HTTPACTION: 1,<status>,<datalen> URC
 *   7. Verify status is 2xx; discard response body (datalen ignored).
 *
 * @param  full_url  Complete HTTPS URL ("https://host/path?query").
 *                   Must be ≤ HTTPS_UL_URL_MAX_LEN-1 characters.
 * @param  fetch_cb  Callback that assembles the POST body into the internal buffer.
 * @param  ctx       Opaque context forwarded to @p fetch_cb.
 * @param  len       Expected POST body size in bytes (must be ≤ HTTPS_UL_FETCH_WINDOW
 *                   and must match the value returned by @p fetch_cb).
 * @return HTTPS_UL_OK on success, or an error code.
 */
HttpsUlResult_t https_uploader_post(const char     *full_url,
                                     HttpsUlFetchCb_t fetch_cb,
                                     void           *ctx,
                                     uint16_t        len);

/**
 * @brief  Stop the HTTP service.
 *
 * Issues AT+HTTPTERM.  Must be called once after all https_uploader_post()
 * calls complete (or on error abort).
 *
 * @return HTTPS_UL_OK on success, HTTPS_UL_ERR_STOP if the command times out
 *         (treated as non-fatal by the upload task — modem will clean up).
 */
HttpsUlResult_t https_uploader_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* A7670_HTTPS_UPLOADER_H */
