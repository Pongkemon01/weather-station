#ifndef __A7670_SSL_UPLOADER_H
#define __A7670_SSL_UPLOADER_H

/**
 * @file  a7670_ssl_uploader.h
 * @brief HTTPS binary upload over the A7670E SSL/CCH service — two modes.
 *
 * ── Modes ────────────────────────────────────────────────────────────────────
 *
 *   ssl_upload_chunked()   One TLS connection per HTTP POST.  Each chunk is
 *                          atomic; failed chunks retry independently.
 *                          Content-Range on every POST (RFC 9110 §14.4).
 *                          Best for: large files, unreliable links.
 *
 *   ssl_upload_stream()    One TLS connection for the whole file.  Headers
 *                          sent once; body streamed window-by-window.
 *                          Retry resumes from session->bytes_done via
 *                          Content-Range when use_range_on_retry = true.
 *                          Best for: small/medium files, stable LTE links.
 *
 * ── Wire protocol ────────────────────────────────────────────────────────────
 *
 *   AT+CCHOPEN=0,"host",port,2  → OK → +CCHOPEN:0,0 (URC)
 *   AT+CCHSEND=0,<hdr_len>      → >  → <HTTP headers> → OK
 *   AT+CCHSEND=0,<window> × N   → >  → <body slice>   → OK
 *   AT+CCHRECV=0,0              → OK → captured HTTP status
 *   AT+CCHCLOSE=0               → OK → +CCHCLOSE:0,0 (URC)
 *
 *   Chunked (every POST — Content-Range always present):
 *     POST <path> HTTP/1.1\r\n
 *     Host: <host>\r\n
 *     Content-Type: application/octet-stream\r\n
 *     Content-Length: <chunk_len>\r\n
 *     Content-Range: bytes <first>-<last>/<total>\r\n
 *     Connection: close\r\n\r\n
 *
 *   Stream, first attempt (no Content-Range unless resuming pre-confirmed bytes):
 *     POST <path> HTTP/1.1\r\n
 *     Host: <host>\r\n
 *     Content-Type: application/octet-stream\r\n
 *     Content-Length: <send_len>\r\n
 *     Connection: close\r\n\r\n
 *
 *   Stream, retry with use_range_on_retry = true:
 *     POST <path> HTTP/1.1\r\n
 *     Host: <host>\r\n
 *     Content-Type: application/octet-stream\r\n
 *     Content-Length: <remaining>\r\n
 *     Content-Range: bytes <resume>-<file_size-1>/<file_size>\r\n
 *     Connection: close\r\n\r\n
 *
 * ── AT+CCHRECV capture ───────────────────────────────────────────────────────
 *   "+CCHRECV" is absent from the URC recogniser (a7670_at_channel.c), so
 *   "+CCHRECV:DATA,0,N" and the HTTP response lines arrive as informational
 *   text in at_channel_set_capture(). Status code is parsed from "HTTP/1.1 ".
 *
 * ── RAM layout ───────────────────────────────────────────────────────────────
 *
 *   Static (.bss) — all module buffers, no stack spill:
 *     s_fetch_buf[256]   256 B  SPI read + AT+CCHSEND payload
 *     s_hdr_buf[384]     384 B  HTTP header assembly
 *     s_cap_buf[64]       64 B  AT+CCHRECV response capture
 *     s_cmd_buf[96]       96 B  AT command string (all helpers share this)
 *     Total static:      800 B
 *
 *   Stack peaks (all call frames sequential, not nested):
 *     Public functions (locals only):        ~20 B
 *     ssl_stream_body (locals + urc):        ~16 B
 *     wait_for_urc (locals + ev):            ~12 B
 *     Peak simultaneous (outer + callee):    ~36 B
 *
 *   Task stack recommendation: 192 words (768 B) including FreeRTOS overhead.
 *   (Down from 512 words / 2048 B before any optimisation.)
 *
 * ── Prerequisites ─────────────────────────────────────────────────────────────
 *   UART_Sys_Init() + UART_Sys_Register()
 *   at_channel_init()
 *   at_channel_wait_ready() == AT_READY_OK
 */

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "a7670_at_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════ Tuning ══════════════════════════════════════*/

/** Bytes per AT+CCHSEND call and SPI read.  A7670E max: 2048 (§19.2.14). */
#define SSL_FETCH_WINDOW        256u

/** Default chunk size for ssl_upload_chunked(). chunk_size=0 → this value. */
#define SSL_CHUNK_SIZE_DEFAULT  4096u

/** Maximum hostname length including null terminator. */
#define SSL_HOST_MAX_LEN        64u

/** Maximum URL path length including null terminator. */
#define SSL_PATH_MAX_LEN        128u

/**
 * HTTP header assembly buffer (static s_hdr_buf).
 * Worst case with Content-Range (all fields at max):
 *   "POST /<127> HTTP/1.1\r\n"                                  144 B
 *   "Host: <63>\r\n"                                             71 B
 *   "Content-Type: application/octet-stream\r\n"                 40 B
 *   "Content-Length: 4294967295\r\n"                             28 B
 *   "Content-Range: bytes 4294967168-4294967295/4294967295\r\n"  55 B
 *   "Connection: close\r\n"                                      19 B
 *   "\r\n" + null                                                 3 B
 *   Total: 360 B → rounded to 384 B.
 */
#define SSL_HEADER_BUF_SIZE     384u

/**
 * AT+CCHRECV response capture buffer (static s_cap_buf).
 * Must hold "+CCHRECV:DATA,0,4096\n" (21 B) + "HTTP/1.1 NNN <reason>\n" (38 B).
 * 64 B is sufficient with margin.
 */
#define SSL_RECV_CAP_SIZE       64u

/**
 * AT command string buffer (static s_cmd_buf, shared by all helpers).
 * Largest command: AT+CCHOPEN=0,"<63 chars>",65535,2 = 88 B + null.
 * 96 B covers all commands with margin.
 */
#define SSL_CMD_BUF_SIZE        96u

/** Timeout: AT+CCHOPEN command + deferred +CCHOPEN:0,0 URC (ms). */
#define SSL_OPEN_TIMEOUT_MS     20000u

/** Per-phase timeout for at_channel_send_binary() — prompt wait and OK wait (ms). */
#define SSL_SEND_TIMEOUT_MS     5000u

/** Timeout: AT+CCHRECV + inline HTTP response (ms). */
#define SSL_RECV_TIMEOUT_MS     15000u

/** Timeout: AT+CCHCLOSE + deferred +CCHCLOSE URC (ms). */
#define SSL_CLOSE_TIMEOUT_MS    8000u

/** Default retry limit.  Chunked: per-chunk.  Stream: per full-file attempt. */
#define SSL_MAX_RETRIES         3u

/** Delay between retries (ms). */
#define SSL_RETRY_DELAY_MS      5000u

/**
 * Wall-clock timeout for ssl_upload_stream() across ALL attempts (ms).
 * Override at compile time: -DSSL_STREAM_TIMEOUT_MS=<value>
 * Not a struct field — all uploads on this MCU share the same bound.
 */
#ifndef SSL_STREAM_TIMEOUT_MS
#define SSL_STREAM_TIMEOUT_MS   120000u
#endif

/* ══════════════════════════ Return codes ════════════════════════════════════*/

typedef enum {
    SSL_UPLOAD_OK            =  0,
    SSL_UPLOAD_ERR_PARAM     =  1, /**< NULL or invalid parameter.                  */
    SSL_UPLOAD_ERR_OPEN      =  2, /**< AT+CCHOPEN failed or +CCHOPEN URC err ≠ 0. */
    SSL_UPLOAD_ERR_SEND_HDR  =  3, /**< AT+CCHSEND failed on HTTP headers.         */
    SSL_UPLOAD_ERR_FETCH     =  4, /**< SPI fetch callback returned false.          */
    SSL_UPLOAD_ERR_SEND_BODY =  5, /**< AT+CCHSEND failed on body data.            */
    SSL_UPLOAD_ERR_RECV      =  6, /**< AT+CCHRECV failed or no HTTP status found. */
    SSL_UPLOAD_ERR_HTTP      =  7, /**< Server returned non-2xx HTTP status.       */
    SSL_UPLOAD_ERR_PEER      =  8, /**< +CCH_PEER_CLOSED or +CCH:CCH STOP.        */
    SSL_UPLOAD_ERR_RETRIES   =  9, /**< Exhausted retry budget.                    */
    SSL_UPLOAD_ERR_TIMEOUT   = 10, /**< SSL_STREAM_TIMEOUT_MS elapsed (stream).    */
} SslUploadResult_t;

/* ══════════════════════════ Callbacks ═══════════════════════════════════════*/

/**
 * SPI flash fetch callback.
 * Read @p len bytes at file offset @p offset into @p buf.
 * @p buf points to the module's internal s_fetch_buf (SSL_FETCH_WINDOW bytes).
 * @return true on success; false on SPI error or out-of-bounds offset.
 */
typedef bool (*SslFetchCb_t)(void     *ctx,
                              uint32_t  offset,
                              uint8_t  *buf,
                              uint16_t  len);

/**
 * Progress callback — optional, both modes.
 *
 * Chunked: fired after each confirmed chunk.  @p bytes is session->bytes_done.
 *          Safe point to persist session to NVM.
 * Stream:  fired after each SSL_FETCH_WINDOW window is sent (unconfirmed).
 *          @p bytes is cumulative bytes sent in the current attempt.
 *          Persist session only after ssl_upload_stream() returns OK.
 *
 * @param ctx        Opaque pointer from SslUploadParams_t.progress_ctx.
 * @param bytes      Bytes confirmed (chunked) or sent this attempt (stream).
 * @param file_size  Total file size.
 */
typedef void (*SslProgressCb_t)(void    *ctx,
                                 uint32_t bytes,
                                 uint32_t file_size);

/* ══════════════════════════ Data types ══════════════════════════════════════*/

/**
 * Resumable session state — 8 bytes.
 *
 * Zero-initialise for a fresh upload.
 *
 * Chunked: persist after each progress_cb fires.  Both fields are valid and
 *          safe to store to NVM.  Pass saved struct back to resume.
 *
 * Stream:  bytes_done is meaningful only after SSL_UPLOAD_OK, or after a
 *          partial range has been server-confirmed on a previous attempt.
 *          next_offset is unused in stream mode.
 */
typedef struct {
    uint32_t next_offset; /**< Chunked: byte offset of next chunk to send. */
    uint32_t bytes_done;  /**< Bytes confirmed by server 2xx response.     */
} SslUploadSession_t;

/**
 * Upload parameters — 40 bytes, no internal padding.
 *
 * All pointer fields must remain valid until the upload call returns.
 */
typedef struct {
    /* Pointers (4 B each on 32-bit MCU) */
    const char      *host;         /**< Server hostname, e.g. "api.example.com". */
    const char      *path;         /**< URL path, e.g. "/v1/upload".             */
    SslFetchCb_t     fetch_cb;     /**< SPI flash read — must not be NULL.        */
    void            *fetch_ctx;    /**< Passed to fetch_cb unchanged.             */
    QueueHandle_t    urc_queue;    /**< Same queue passed to at_channel_init().   */
    SslProgressCb_t  progress_cb;  /**< NULL = disabled.                          */
    void            *progress_ctx; /**< Passed to progress_cb unchanged.          */
    /* 32-bit scalars */
    uint32_t         file_size;    /**< Total file size in bytes (> 0).           */
    uint32_t         chunk_size;   /**< Chunked: bytes per POST. 0 = default.     */
    /* Small scalars packed at end — no padding gaps */
    uint16_t         port;         /**< TCP port, typically 443.                  */
    uint8_t          max_retries;  /**< Per-chunk / per-attempt limit. 0=default. */
    /**
     * Stream mode only.
     * true:  retry resumes from session->bytes_done with a Content-Range header.
     *        Requires server-side range-upload support (RFC 9110 §14.4).
     * false: retry always re-sends from byte 0 (safe with any server).
     */
    bool             use_range_on_retry;
} SslUploadParams_t;

/* ══════════════════════════ Public API ══════════════════════════════════════*/

/**
 * ssl_upload_chunked — upload a file as independent per-chunk HTTP POSTs.
 *
 * Each chunk opens its own TLS connection, sends headers + body, reads the
 * server response, then closes.  Content-Range is sent on every POST so the
 * server can assemble the file and detect duplicate chunks on retry.
 *
 * Failed chunks are retried up to max_retries times (default SSL_MAX_RETRIES).
 * Fetch errors (SPI failure) return immediately without consuming a retry.
 *
 * Advances session->next_offset and session->bytes_done after each confirmed
 * chunk, then calls progress_cb.  This is the safe NVM persist point.
 *
 * Blocking.  Task stack: 192 words (768 B).
 */
SslUploadResult_t ssl_upload_chunked(const SslUploadParams_t *params,
                                      SslUploadSession_t      *session);

/**
 * ssl_upload_stream — upload the entire file as one HTTP POST.
 *
 * One TLS handshake per attempt.  Body streamed window-by-window.
 * On failure, retry starts from session->bytes_done (with Content-Range if
 * use_range_on_retry = true) or from byte 0 (use_range_on_retry = false).
 *
 * Aborts with ERR_TIMEOUT  if SSL_STREAM_TIMEOUT_MS elapses (all attempts).
 * Aborts with ERR_RETRIES  if attempt count exceeds max_retries.
 * Fetch errors return immediately.
 *
 * Blocking.  Task stack: 192 words (768 B).
 */
SslUploadResult_t ssl_upload_stream(const SslUploadParams_t *params,
                                     SslUploadSession_t      *session);

#ifdef __cplusplus
}
#endif

#endif /* __A7670_SSL_UPLOADER_H */
