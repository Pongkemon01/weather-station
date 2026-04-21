/**
 * @file  a7670_ssl_downloader.h
 * @brief OTA firmware download via the A7670E HTTP(S) service (AT+HTTP* commands).
 *
 * ── Service architecture ───────────────────────────────────────────────────
 *
 * Uses the A7670E standard HTTP(S) service (Chapter 16 of the AT command manual):
 *   AT+HTTPINIT  — start the HTTP service (activates PDP context)
 *   AT+HTTPPARA  — set URL, SSL context, content-type, custom headers
 *   AT+HTTPACTION=0 — issue GET; returns OK immediately; +HTTPACTION URC follows
 *   AT+HTTPREAD  — drain the modem's response body buffer in chunks
 *   AT+HTTPTERM  — stop the HTTP service
 *
 * SSL is configured via AT+CSSLCFG (context 0) once in Modem_Module_Init()
 * and referenced here with AT+HTTPPARA="SSLCFG",0.
 *
 * ── Calling convention ────────────────────────────────────────────────────
 *
 *   ssl_downloader_start(urc_queue);
 *
 *   // Version check
 *   ssl_downloader_get("https://host/path/", buf, sizeof(buf), &n);
 *   buf[n] = '\0';   // null-terminate for string parsing
 *
 *   // Firmware chunk (repeat for each chunk)
 *   ssl_downloader_get("https://host/path/get_firmware?offset=0&length=512",
 *                       chunk_buf, HTTPS_DL_CHUNK_SIZE, &n);
 *
 *   ssl_downloader_stop();
 *
 * ── Prerequisites ─────────────────────────────────────────────────────────
 *   modem_init() completed successfully.
 *   Certificates injected and AT+CSSLCFG context 0 configured by Modem_Module_Init().
 *
 * ── RAM layout ────────────────────────────────────────────────────────────
 *   s_cmd_buf[224]         224 B  AT command assembly (fits HTTPPARA URL up to 191 chars)
 *   s_chunk_read_buf[516]  516 B  internal buffer for ssl_downloader_get_chunk()
 *                                 (chunk data 512 B + CRC-32 trailer 4 B)
 *   Total static:          740 B
 *   Data buffer: caller-supplied (HTTPS_DL_CHUNK_SIZE = 512 B for chunk downloads)
 *
 * ── Thread safety ─────────────────────────────────────────────────────────
 *   Not re-entrant.  Only OtaManagerTask may call these functions.
 */

#ifndef A7670_SSL_DOWNLOADER_H
#define A7670_SSL_DOWNLOADER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ Tuning ══════════════════════════════════════════*/

/** Bytes of firmware data per chunk — must match the server chunk size. */
#define HTTPS_DL_CHUNK_SIZE     512u

/** Total bytes per get_firmware response = chunk data + 4-byte CRC trailer. */
#define HTTPS_DL_CHUNK_WITH_CRC_SIZE  (HTTPS_DL_CHUNK_SIZE + 4u)

/** SSL context index configured by Modem_Module_Init() via AT+CSSLCFG. */
#define HTTPS_DL_SSL_CTX        0u

/** Maximum HTTPS URL length including scheme, host, path, query, and null. */
#define HTTPS_DL_URL_MAX_LEN    192u

/** Timeout for AT+HTTPINIT (activates PDP context, ms). */
#define HTTPS_DL_INIT_MS        10000u

/** Timeout for AT+HTTPPARA parameter commands (ms). */
#define HTTPS_DL_PARAM_MS       2000u

/** Timeout for AT+HTTPACTION=0 + +HTTPACTION URC — includes network + TLS (ms). */
#define HTTPS_DL_ACTION_MS      60000u

/** Timeout per AT+HTTPREAD call (ms). */
#define HTTPS_DL_READ_MS        30000u

/** Timeout for AT+HTTPTERM (ms). */
#define HTTPS_DL_STOP_MS        10000u

/* ══════════════════════════ Return codes ════════════════════════════════════*/

typedef enum {
    SSL_DL_OK          = 0,  /**< Success.                                        */
    SSL_DL_ERR_PARAM   = 1,  /**< NULL or invalid parameter.                      */
    SSL_DL_ERR_INIT    = 2,  /**< AT+HTTPINIT failed.                             */
    SSL_DL_ERR_SSL_CFG = 3,  /**< AT+HTTPPARA="SSLCFG" failed.                   */
    SSL_DL_ERR_URL     = 4,  /**< AT+HTTPPARA="URL" failed.                       */
    SSL_DL_ERR_GET     = 5,  /**< AT+HTTPACTION=0 failed or +HTTPACTION timed out.*/
    SSL_DL_ERR_HTTP    = 6,  /**< Server returned a non-2xx HTTP status.          */
    SSL_DL_ERR_READ    = 7,  /**< AT+HTTPREAD failed.                             */
    SSL_DL_ERR_STOP    = 8,  /**< AT+HTTPTERM failed (non-fatal; modem cleans up).*/
    SSL_DL_ERR_CRC     = 9,  /**< Per-chunk CRC-32 mismatch — retransmit chunk.  */
} SslDlResult_t;

/* ══════════════════════════ Public API ══════════════════════════════════════*/

/**
 * @brief  Start the HTTP service and configure SSL.
 *
 * Issues AT+HTTPINIT then AT+HTTPPARA="SSLCFG",HTTPS_DL_SSL_CTX.
 * Must be called once before any ssl_downloader_get() calls.
 *
 * @param  urc_queue  URC queue from modem_get_urc_queue() — stored internally
 *                    for the lifetime of the HTTP session.
 * @return SSL_DL_OK on success, or an error code.
 */
SslDlResult_t ssl_downloader_start(QueueHandle_t urc_queue);

/**
 * @brief  Perform one HTTPS GET and receive the full response body.
 *
 * Issues AT+HTTPPARA="URL" then AT+HTTPACTION=0, waits for the +HTTPACTION
 * URC to confirm a 2xx HTTP status, then calls AT+HTTPREAD to retrieve the
 * body.  Total bytes written to @p buf are reported in @p received.
 *
 * For version check: pass a 512-byte buffer and null-terminate at buf[received].
 * For firmware chunk: pass a HTTPS_DL_CHUNK_SIZE buffer; received will be the
 * number of bytes in this chunk (≤ HTTPS_DL_CHUNK_SIZE).
 *
 * @param  full_url  Complete HTTPS URL ("https://host/path?query").
 *                   Must be ≤ HTTPS_DL_URL_MAX_LEN-1 characters.
 * @param  buf       Caller-supplied output buffer.
 * @param  max_len   Buffer capacity in bytes.
 * @param  received  Total bytes written to @p buf.
 * @return SSL_DL_OK on success, or an error code.
 */
SslDlResult_t ssl_downloader_get(const char *full_url,
                                  uint8_t    *buf,
                                  uint16_t    max_len,
                                  uint16_t   *received);

/**
 * @brief  Perform one HTTPS GET for a firmware chunk and validate its CRC-32.
 *
 * The server response is: <chunk_data> (up to HTTPS_DL_CHUNK_SIZE bytes)
 * followed immediately by a 4-byte little-endian CRC-32/MPEG-2 of that chunk.
 * Total server response = actual_chunk_len + 4.
 *
 * This function reads the full response into an internal buffer, verifies the
 * trailing CRC-32 against the chunk data, then copies only the chunk data to
 * @p chunk_buf.  Returns SSL_DL_ERR_CRC if the CRC does not match — the OTA
 * manager should retry the chunk URL.
 *
 * @param  full_url       Complete HTTPS URL for the chunk
 *                        ("https://host/path/get_firmware?offset=X&length=512").
 *                        Must be ≤ HTTPS_DL_URL_MAX_LEN-1 characters.
 * @param  chunk_buf      Caller-supplied buffer for chunk data only (no CRC).
 *                        Must be at least HTTPS_DL_CHUNK_SIZE bytes.
 * @param  max_chunk_len  Capacity of @p chunk_buf in bytes.
 * @param  chunk_received Actual chunk data bytes written to @p chunk_buf
 *                        (= response length − 4; ≤ HTTPS_DL_CHUNK_SIZE).
 * @return SSL_DL_OK, SSL_DL_ERR_CRC, or any other SslDlResult_t error code.
 */
SslDlResult_t ssl_downloader_get_chunk(const char *full_url,
                                        uint8_t    *chunk_buf,
                                        uint16_t    max_chunk_len,
                                        uint16_t   *chunk_received);

/**
 * @brief  Stop the HTTP service.
 *
 * Issues AT+HTTPTERM.  Must be called once after all ssl_downloader_get()
 * and ssl_downloader_get_chunk() calls complete (or on error abort).
 *
 * @return SSL_DL_OK on success, SSL_DL_ERR_STOP if the command times out
 *         (treated as non-fatal by the OTA manager — modem will clean up).
 */
SslDlResult_t ssl_downloader_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* A7670_SSL_DOWNLOADER_H */
