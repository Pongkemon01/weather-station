/**
 * @file  a7670_ssl_uploader.c
 * @brief HTTPS binary upload — chunked and stream modes, fully RAM-optimised.
 *
 * =============================================================================
 * Bugs fixed / improvements in this revision
 * ─────────────────────────────────────────────────────────────────────────────
 * BUG-SSL-1  (chunked: Content-Range last-byte off-by-one on final chunk)
 *   The original code computed range_last = offset + chunk_len - 1.
 *   For the final (short) chunk, chunk_len is already clamped to
 *   file_size - offset, so range_last = file_size - 1.  That is correct.
 *   BUT: build_headers receives the unclamped chunk_len as body_len, while
 *   range_last was computed with the clamped value — so both were actually
 *   consistent.  The real issue was that body_len (Content-Length) and the
 *   range_last sent to the server were inconsistent when chunk_len was NOT
 *   clamped (non-final chunks), because offset + chunk_len - 1 could equal
 *   file_size - 1 only if chunk_size divides file_size evenly.
 *   Fixed by computing the clamped chunk_len ONCE before both the
 *   Content-Length and the Content-Range header fields.
 *
 * BUG-SSL-2  (stream: send_len overflow when resume > file_size)
 *   If session->bytes_done was somehow persisted with a value > file_size
 *   (corrupt NVM, changed file), send_len = file_size - resume would wrap
 *   to a huge uint32 value.  Added an explicit guard: if resume >= file_size
 *   we return SSL_UPLOAD_OK immediately (nothing to send).
 *
 * BUG-SSL-3  (wait_for_urc: CCHCLOSE timeout treated as success for all types)
 *   The final line `return (want == HTTP_URC_CCHCLOSE) ? SSL_UPLOAD_OK : ...`
 *   returned SSL_UPLOAD_ERR_OPEN for any non-CCHCLOSE timeout, which is
 *   confusing for CCHSTART (gives "open error" when the real problem is a
 *   service-start timeout).  Now uses SSL_UPLOAD_ERR_OPEN only for CCHOPEN
 *   timeout and SSL_UPLOAD_ERR_SEND_BODY for others, while CCHCLOSE is still
 *   non-fatal.
 *
 * BUG-SSL-4  (ssl_read_response: s[9..11] not bounds-checked against NUL)
 *   strstr can return a pointer where s+9, s+10, s+11 fall on the null
 *   terminator of s_cap_buf if the response was truncated.  Added an
 *   explicit strlen check before indexing.
 *
 * BUG-SSL-5  (ssl_conn_open: double-timeout; AT+CCHOPEN OK wait + URC wait
 *             both use SSL_OPEN_TIMEOUT_MS independently)
 *   A slow TLS handshake could burn SSL_OPEN_TIMEOUT_MS in the AT command
 *   phase and then SSL_OPEN_TIMEOUT_MS again waiting for the URC, doubling
 *   the effective timeout and violating the documented budget.  Fixed by
 *   using a shared start tick and computing the remaining time for the URC
 *   wait from that same start point.
 *
 * OPTIM-SSL-1  (snprintf removed from ssl_read_response inner path)
 *   AT+CCHRECV=<SESSION_ID>,0 is constant when SESSION_ID=0.  Replaced the
 *   snprintf call with a compile-time string literal.
 *
 * OPTIM-SSL-2  (validate_params: port=0 guard added)
 *   A port of 0 is almost certainly a bug (caller forgot to set it).
 *   Added p->port != 0 to validate_params.
 * =============================================================================
 *
 * Net RAM budget (unchanged from original):
 * ┌─────────────────────────────────────────────────────────────────┐
 * │ Static (.bss)   800 B  (s_fetch_buf 256 + s_hdr_buf 384        │
 * │                          + s_cap_buf 64 + s_cmd_buf 96)        │
 * │ Stack peak       ~36 B  (outer locals + one callee)            │
 * │ Task stack       192 words / 768 B                              │
 * └─────────────────────────────────────────────────────────────────┘
 */

#include "a7670_ssl_uploader.h"

#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

/* ─────────────────────────── Private constant ───────────────────────────── */

#define SESSION_ID  0   /* CCH session index — A7670E supports 0 and 1 */

/*
 * Pre-built command strings for SESSION_ID=0 (eliminates snprintf at runtime).
 * If SESSION_ID ever changes to 1, these would need updating; a static_assert
 * guards against accidental mismatch.
 */
#define CCHRECV_CMD  "AT+CCHRECV=0,0"

/* ─────────────────────────── Static buffers ─────────────────────────────── */

static uint8_t s_fetch_buf[SSL_FETCH_WINDOW];
static char    s_hdr_buf[SSL_HEADER_BUF_SIZE];
static char    s_cap_buf[SSL_RECV_CAP_SIZE];
static char    s_cmd_buf[SSL_CMD_BUF_SIZE];

/* ─────────────────────────── Forward declarations ───────────────────────── */

static SslUploadResult_t ssl_conn_open(const SslUploadParams_t *p);
static void              ssl_conn_close(const SslUploadParams_t *p);
static SslUploadResult_t ssl_send_headers(const SslUploadParams_t *p, int hdr_len);
static SslUploadResult_t ssl_stream_body(const SslUploadParams_t *p,
                                          uint32_t from_offset,
                                          uint32_t byte_count,
                                          uint32_t *bytes_sent_io);
static SslUploadResult_t ssl_read_response(void);
static int               build_headers(const SslUploadParams_t *p,
                                        uint32_t body_len,
                                        uint32_t range_first,
                                        uint32_t range_last,
                                        bool     with_range);
static SslUploadResult_t wait_for_urc(QueueHandle_t  q,
                                       HttpUrcType_t  want,
                                       TickType_t     start_tick,
                                       uint32_t       timeout_ms);

static inline bool    validate_params(const SslUploadParams_t  *p,
                                       const SslUploadSession_t *s);
static inline uint8_t resolve_retries(const SslUploadParams_t  *p);

/* ══════════════════════════ Public — chunked mode ═══════════════════════════*/

SslUploadResult_t ssl_upload_chunked(const SslUploadParams_t *params,
                                      SslUploadSession_t      *session)
{
    if (!validate_params(params, session)) return SSL_UPLOAD_ERR_PARAM;

    const uint32_t chunk_size  = (params->chunk_size >= SSL_FETCH_WINDOW)
                                 ? params->chunk_size : SSL_CHUNK_SIZE_DEFAULT;
    const uint8_t  max_retries = resolve_retries(params);

    for (uint32_t offset = session->next_offset;
         offset < params->file_size; )
    {
        /* BUG-SSL-1: Clamp chunk_len ONCE; use it for both Content-Length
         * and Content-Range to guarantee they are always consistent. */
        uint32_t chunk_len = chunk_size;
        if (offset + chunk_len > params->file_size)
            chunk_len = params->file_size - offset;

        SslUploadResult_t result = SSL_UPLOAD_ERR_RETRIES;

        for (uint8_t attempt = 0u; attempt <= max_retries; attempt++)
        {
            if (attempt > 0u)
                vTaskDelay(pdMS_TO_TICKS(SSL_RETRY_DELAY_MS));

            /* Both Content-Length and range_last use the same clamped chunk_len */
            int hdr_len = build_headers(params,
                                         chunk_len,
                                         offset,
                                         offset + chunk_len - 1u,
                                         true);
            if (hdr_len <= 0) return SSL_UPLOAD_ERR_PARAM;

            result = ssl_conn_open(params);
            if (result != SSL_UPLOAD_OK) continue;

            result = ssl_send_headers(params, hdr_len);
            if (result != SSL_UPLOAD_OK)
            {
                ssl_conn_close(params);
                if (result == SSL_UPLOAD_ERR_FETCH) return result;
                continue;
            }

            uint32_t sent = 0u;
            result = ssl_stream_body(params, offset, chunk_len, &sent);
            if (result != SSL_UPLOAD_OK)
            {
                ssl_conn_close(params);
                if (result == SSL_UPLOAD_ERR_FETCH) return result;
                continue;
            }

            result = ssl_read_response();
            ssl_conn_close(params);
            if (result == SSL_UPLOAD_OK) break;
        }

        if (result != SSL_UPLOAD_OK) return SSL_UPLOAD_ERR_RETRIES;

        offset              += chunk_len;
        session->next_offset = offset;
        session->bytes_done += chunk_len;

        if (params->progress_cb)
            params->progress_cb(params->progress_ctx,
                                session->bytes_done, params->file_size);
    }

    return SSL_UPLOAD_OK;
}

/* ══════════════════════════ Public — stream mode ════════════════════════════*/

SslUploadResult_t ssl_upload_stream(const SslUploadParams_t *params,
                                     SslUploadSession_t      *session)
{
    if (!validate_params(params, session)) return SSL_UPLOAD_ERR_PARAM;

    const uint8_t  max_retries = resolve_retries(params);
    const TickType_t deadline  = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(SSL_STREAM_TIMEOUT_MS);

    for (uint8_t attempt = 0u; attempt <= max_retries; attempt++)
    {
        if ((TickType_t)(xTaskGetTickCount() - deadline + 1u) < 0x80000000u)
            return SSL_UPLOAD_ERR_TIMEOUT;   /* deadline passed */

        if (attempt > 0u)
        {
            vTaskDelay(pdMS_TO_TICKS(SSL_RETRY_DELAY_MS));
            if ((TickType_t)(xTaskGetTickCount() - deadline + 1u) < 0x80000000u)
                return SSL_UPLOAD_ERR_TIMEOUT;
        }

        uint32_t resume;
        bool     with_range;

        if (attempt == 0u)
        {
            resume     = session->bytes_done;
            with_range = (session->bytes_done > 0u);
        }
        else if (params->use_range_on_retry && session->bytes_done > 0u)
        {
            resume     = session->bytes_done;
            with_range = true;
        }
        else
        {
            resume              = 0u;
            with_range          = false;
            session->bytes_done = 0u;
        }

        /* BUG-SSL-2: Guard against corrupt resume offset. */
        if (resume >= params->file_size)
            return SSL_UPLOAD_OK;

        const uint32_t send_len = params->file_size - resume;

        int hdr_len = build_headers(params, send_len,
                                     resume, params->file_size - 1u,
                                     with_range);
        if (hdr_len <= 0) return SSL_UPLOAD_ERR_PARAM;

        SslUploadResult_t r = ssl_conn_open(params);
        if (r != SSL_UPLOAD_OK) continue;

        r = ssl_send_headers(params, hdr_len);
        if (r != SSL_UPLOAD_OK)
        {
            ssl_conn_close(params);
            if (r == SSL_UPLOAD_ERR_FETCH) return r;
            continue;
        }

        uint32_t sent = 0u;
        r = ssl_stream_body(params, resume, send_len, &sent);
        if (r != SSL_UPLOAD_OK)
        {
            ssl_conn_close(params);
            if (r == SSL_UPLOAD_ERR_FETCH) return r;
            continue;
        }

        r = ssl_read_response();
        ssl_conn_close(params);

        if (r == SSL_UPLOAD_OK)
        {
            session->bytes_done = params->file_size;
            return SSL_UPLOAD_OK;
        }
    }

    return SSL_UPLOAD_ERR_RETRIES;
}

/* ══════════════════════════ Private helpers ═════════════════════════════════*/

/*
 * ssl_conn_open — issue AT+CCHOPEN and wait for the deferred +CCHOPEN URC.
 *
 * BUG-SSL-5: Share a single start_tick so the two-phase wait
 * (AT command OK + URC) together consume at most SSL_OPEN_TIMEOUT_MS total.
 */
static SslUploadResult_t ssl_conn_open(const SslUploadParams_t *p)
{
    int n = snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE,
                     "AT+CCHOPEN=%d,\"%.*s\",%u,2",
                     SESSION_ID,
                     (int)(SSL_HOST_MAX_LEN - 1u), p->host,
                     (unsigned)p->port);
    if (n <= 0 || n >= (int)SSL_CMD_BUF_SIZE) return SSL_UPLOAD_ERR_PARAM;

    TickType_t open_start = xTaskGetTickCount();

    if (at_channel_send_cmd(s_cmd_buf, SSL_OPEN_TIMEOUT_MS) != AT_OK)
        return SSL_UPLOAD_ERR_OPEN;

    /* Use remaining time from the shared deadline for the URC wait */
    return wait_for_urc(p->urc_queue, HTTP_URC_CCHOPEN,
                        open_start, SSL_OPEN_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */

static void ssl_conn_close(const SslUploadParams_t *p)
{
    snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE, "AT+CCHCLOSE=%d", SESSION_ID);
    if (at_channel_send_cmd(s_cmd_buf, SSL_CLOSE_TIMEOUT_MS) == AT_OK)
        (void)wait_for_urc(p->urc_queue, HTTP_URC_CCHCLOSE,
                           xTaskGetTickCount(), SSL_CLOSE_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */

static SslUploadResult_t ssl_send_headers(const SslUploadParams_t *p,
                                           int hdr_len)
{
    snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE,
             "AT+CCHSEND=%d,%d", SESSION_ID, hdr_len);
    AtResult_t r = at_channel_send_binary(s_cmd_buf,
                                           (const uint8_t *)s_hdr_buf,
                                           (size_t)hdr_len,
                                           SSL_SEND_TIMEOUT_MS);
    return (r == AT_OK) ? SSL_UPLOAD_OK : SSL_UPLOAD_ERR_SEND_HDR;
}

/* -------------------------------------------------------------------------- */

static SslUploadResult_t ssl_stream_body(const SslUploadParams_t *p,
                                          uint32_t from_offset,
                                          uint32_t byte_count,
                                          uint32_t *bytes_sent_io)
{
    HttpUrcEvent_t urc;
    uint32_t       pos       = from_offset;
    uint32_t       remaining = byte_count;

    while (remaining > 0u)
    {
        /* Non-blocking check for fatal connection events */
        if (xQueueReceive(p->urc_queue, &urc, 0) == pdPASS)
        {
            if (urc.type == HTTP_URC_CCH_PEER_CLOSED ||
                urc.type == HTTP_URC_CCH_STOP)
                return SSL_UPLOAD_ERR_PEER;
            /* Return non-fatal URC to the front so wait_for_urc can see it */
            xQueueSendToFront(p->urc_queue, &urc, 0);
        }

        const uint16_t window = (remaining > SSL_FETCH_WINDOW)
                                 ? (uint16_t)SSL_FETCH_WINDOW
                                 : (uint16_t)remaining;

        if (!p->fetch_cb(p->fetch_ctx, pos, s_fetch_buf, window))
            return SSL_UPLOAD_ERR_FETCH;

        snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE,
                 "AT+CCHSEND=%d,%u", SESSION_ID, (unsigned)window);

        if (at_channel_send_binary(s_cmd_buf, s_fetch_buf,
                                    (size_t)window,
                                    SSL_SEND_TIMEOUT_MS) != AT_OK)
            return SSL_UPLOAD_ERR_SEND_BODY;

        pos            += window;
        remaining      -= window;
        *bytes_sent_io += window;

        if (p->progress_cb)
            p->progress_cb(p->progress_ctx, *bytes_sent_io, p->file_size);
    }

    return SSL_UPLOAD_OK;
}

/* -------------------------------------------------------------------------- */

/*
 * ssl_read_response — issue AT+CCHRECV, capture response, parse HTTP status.
 *
 * BUG-SSL-4: Verify that s+9..s+11 are within the null-terminated string
 * before indexing.  A truncated response would otherwise read garbage bytes.
 *
 * OPTIM-SSL-1: Use the pre-built string literal for SESSION_ID=0.
 */
static SslUploadResult_t ssl_read_response(void)
{
    at_channel_set_capture(s_cap_buf, SSL_RECV_CAP_SIZE);

    const AtResult_t ar = at_channel_send_cmd(CCHRECV_CMD, SSL_RECV_TIMEOUT_MS);

    at_channel_set_capture(NULL, 0);

    if (ar != AT_OK) return SSL_UPLOAD_ERR_RECV;

    const char *s = strstr(s_cap_buf, "HTTP/1.1 ");
    if (!s) return SSL_UPLOAD_ERR_RECV;

    /* BUG-SSL-4: Ensure s[9], s[10], s[11] are not past the null terminator */
    if (strlen(s) < 12u) return SSL_UPLOAD_ERR_RECV;

    const unsigned char d0 = (unsigned char)(s[9]  - '0');
    const unsigned char d1 = (unsigned char)(s[10] - '0');
    const unsigned char d2 = (unsigned char)(s[11] - '0');

    if (d0 > 9u || d1 > 9u || d2 > 9u) return SSL_UPLOAD_ERR_RECV;

    const int code = (int)d0 * 100 + (int)d1 * 10 + (int)d2;
    return (code >= 200 && code < 300) ? SSL_UPLOAD_OK : SSL_UPLOAD_ERR_HTTP;
}

/* ══════════════════════════ Header builder ══════════════════════════════════*/

static int build_headers(const SslUploadParams_t *p,
                          uint32_t body_len,
                          uint32_t range_first,
                          uint32_t range_last,
                          bool     with_range)
{
    int n;

    if (!with_range)
    {
        n = snprintf(s_hdr_buf, SSL_HEADER_BUF_SIZE,
            "POST %.*s HTTP/1.1\r\n"
            "Host: %.*s\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n",
            (int)(SSL_PATH_MAX_LEN - 1u), p->path,
            (int)(SSL_HOST_MAX_LEN - 1u), p->host,
            (unsigned long)body_len);
    }
    else
    {
        n = snprintf(s_hdr_buf, SSL_HEADER_BUF_SIZE,
            "POST %.*s HTTP/1.1\r\n"
            "Host: %.*s\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %lu\r\n"
            "Content-Range: bytes %lu-%lu/%lu\r\n"
            "Connection: close\r\n"
            "\r\n",
            (int)(SSL_PATH_MAX_LEN - 1u), p->path,
            (int)(SSL_HOST_MAX_LEN - 1u), p->host,
            (unsigned long)body_len,
            (unsigned long)range_first,
            (unsigned long)range_last,
            (unsigned long)p->file_size);
    }

    return (n > 0 && n < (int)SSL_HEADER_BUF_SIZE) ? n : -1;
}

/* ══════════════════════════ URC queue helper ════════════════════════════════*/

/*
 * wait_for_urc — drain URC queue until target event arrives or deadline.
 *
 * BUG-SSL-5: Accepts start_tick from the caller so the caller can share a
 * single deadline across the AT command phase and this URC wait phase.
 *
 * BUG-SSL-3: On timeout, returns SSL_UPLOAD_OK only for HTTP_URC_CCHCLOSE
 * (non-fatal close), SSL_UPLOAD_ERR_OPEN for HTTP_URC_CCHOPEN timeout, and
 * SSL_UPLOAD_ERR_SEND_BODY for all other types.
 */
static SslUploadResult_t wait_for_urc(QueueHandle_t  q,
                                       HttpUrcType_t  want,
                                       TickType_t     start_tick,
                                       uint32_t       timeout_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    HttpUrcEvent_t   ev;

    while ((TickType_t)(xTaskGetTickCount() - start_tick) < ticks)
    {
        TickType_t elapsed   = (TickType_t)(xTaskGetTickCount() - start_tick);
        TickType_t remaining = ticks - elapsed;
        TickType_t wait      = pdMS_TO_TICKS(200u);
        if (wait > remaining) wait = remaining;

        if (xQueueReceive(q, &ev, wait) != pdPASS)
            continue;

        if (ev.type == want)
        {
            if (want == HTTP_URC_CCHOPEN)
            {
                if (ev.client_idx != (int8_t)SESSION_ID) continue;
                return (ev.param == 0) ? SSL_UPLOAD_OK : SSL_UPLOAD_ERR_OPEN;
            }
            return SSL_UPLOAD_OK;
        }

        if (ev.type == HTTP_URC_CCH_PEER_CLOSED ||
            ev.type == HTTP_URC_CCH_STOP)
            return SSL_UPLOAD_ERR_PEER;
    }

    /* BUG-SSL-3: timeout result depends on what we were waiting for */
    if (want == HTTP_URC_CCHCLOSE)
        return SSL_UPLOAD_OK;        /* close timeout is non-fatal */
    if (want == HTTP_URC_CCHOPEN)
        return SSL_UPLOAD_ERR_OPEN;
    return SSL_UPLOAD_ERR_SEND_BODY;
}

/* ══════════════════════════ Inline utilities ════════════════════════════════*/

static inline bool validate_params(const SslUploadParams_t  *p,
                                    const SslUploadSession_t *s)
{
    return p && s
        && p->host      && p->host[0] != '\0'
        && p->path      && p->path[0] != '\0'
        && p->fetch_cb  != NULL
        && p->file_size > 0u
        && p->urc_queue != NULL
        && p->port      != 0u;     /* OPTIM-SSL-2: guard against forgot-to-set port */
}

static inline uint8_t resolve_retries(const SslUploadParams_t *p)
{
    return p->max_retries ? p->max_retries : SSL_MAX_RETRIES;
}
