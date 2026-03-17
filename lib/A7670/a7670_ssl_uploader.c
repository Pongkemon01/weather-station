/**
 * @file  a7670_ssl_uploader.c
 * @brief HTTPS binary upload — chunked and stream modes, fully RAM-optimised.
 *
 * Net RAM budget:
 * ┌─────────────────────────────────────────────────────────────────┐
 * │ Static (.bss)   800 B  (s_fetch_buf 256 + s_hdr_buf 384        │
 * │                          + s_cap_buf 64 + s_cmd_buf 96)        │
 * │ Stack peak       ~36 B  (outer locals + one callee)            │
 * │ Task stack       192 words / 768 B  (was 512 words / 2048 B)   │
 * │ SslUploadParams_t  40 B  (was 56 B, no padding)                │
 * │ SslUploadSession_t  8 B  (was 16 B)                            │
 * └─────────────────────────────────────────────────────────────────┘
 */

#include "a7670_ssl_uploader.h"

#include <string.h>
#include <stdio.h>   /* snprintf — already needed for build_headers */
#include "FreeRTOS.h"
#include "task.h"

/* ─────────────────────────── Private constant ───────────────────────────── */

#define SESSION_ID  0   /* CCH session index — A7670E supports 0 and 1 */

/* ─────────────────────────── Static buffers ─────────────────────────────── */

/*
 * All module buffers are static so they never consume task stack.
 * This is safe because only one task may call ssl_upload_chunked /
 * ssl_upload_stream at a time (non-re-entrant by design).
 */

/** SPI fetch window and AT+CCHSEND payload. */
static uint8_t s_fetch_buf[SSL_FETCH_WINDOW];

/** HTTP header assembly buffer used by build_headers(). */
static char    s_hdr_buf[SSL_HEADER_BUF_SIZE];

/**
 * AT+CCHRECV response capture buffer.
 * Registered with at_channel_set_capture() before AT+CCHRECV, cleared after.
 * 64 B holds "+CCHRECV:DATA,0,4096\n" (21 B) + "HTTP/1.1 NNN reason\n" (38 B).
 */
static char    s_cap_buf[SSL_RECV_CAP_SIZE];

/**
 * AT command string buffer shared by all helpers.
 * 96 B covers the largest command:
 *   "AT+CCHOPEN=0,"<63 chars>",65535,2\0" = 88 B.
 * Helpers write here with snprintf before calling at_channel_send_*.
 */
static char    s_cmd_buf[SSL_CMD_BUF_SIZE];

/* ─────────────────────────── Forward declarations ───────────────────────── */

static SslUploadResult_t ssl_conn_open(const SslUploadParams_t *p);
static void              ssl_conn_close(const SslUploadParams_t *p);
static SslUploadResult_t ssl_send_headers(const SslUploadParams_t *p,
                                           int hdr_len);
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
static SslUploadResult_t wait_for_urc(QueueHandle_t q,
                                       HttpUrcType_t  want,
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
        /* Compute this chunk's length — last chunk may be shorter */
        uint32_t chunk_len = chunk_size;
        if (offset + chunk_len > params->file_size)
            chunk_len = params->file_size - offset;

        SslUploadResult_t result = SSL_UPLOAD_ERR_RETRIES;

        for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
        {
            if (attempt > 0)
                vTaskDelay(pdMS_TO_TICKS(SSL_RETRY_DELAY_MS));

            int hdr_len = build_headers(params, chunk_len,
                                         offset,
                                         offset + chunk_len - 1u,
                                         true);   /* Content-Range always */
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

        /* Chunk confirmed — advance session (safe NVM persist point) */
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

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        if (xTaskGetTickCount() >= deadline) return SSL_UPLOAD_ERR_TIMEOUT;

        if (attempt > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(SSL_RETRY_DELAY_MS));
            if (xTaskGetTickCount() >= deadline) return SSL_UPLOAD_ERR_TIMEOUT;
        }

        /*
         * Determine resume point and whether to include Content-Range.
         *
         * attempt == 0:
         *   Start from bytes_done (0 for a fresh upload; non-zero if the
         *   caller pre-loaded a session with a previously confirmed range).
         *   Include Content-Range only when resuming a partial file.
         *
         * attempt > 0, use_range_on_retry && bytes_done > 0:
         *   Resume from confirmed offset; include Content-Range.
         *
         * attempt > 0, otherwise:
         *   Full re-send from byte 0; clear bytes_done.
         */
        uint32_t resume;
        bool     with_range;

        if (attempt == 0)
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

        const uint32_t send_len = params->file_size - resume;
        if (send_len == 0u) return SSL_UPLOAD_OK;   /* nothing left */

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
            /* bytes_done not updated — server confirmation not received */
            continue;
        }

        r = ssl_read_response();
        ssl_conn_close(params);

        if (r == SSL_UPLOAD_OK)
        {
            session->bytes_done = params->file_size;
            return SSL_UPLOAD_OK;
        }
        /* Non-2xx: bytes_done unchanged; retry from bytes_done */
    }

    return SSL_UPLOAD_ERR_RETRIES;
}

/* ══════════════════════════ Private helpers ═════════════════════════════════*/

/*
 * ssl_conn_open — issue AT+CCHOPEN and wait for the deferred +CCHOPEN URC.
 *
 * A7670E (§19.2.12):
 *   → AT+CCHOPEN=0,"<host>",<port>,2    client_type 2 = SSL/TLS
 *   ← OK                                command accepted
 *   ← +CCHOPEN:0,0                      TLS handshake complete
 *   ← +CCHOPEN:0,<err>                  TLS handshake failed (err ≠ 0)
 *
 * Writes to s_cmd_buf (static shared).
 */
static SslUploadResult_t ssl_conn_open(const SslUploadParams_t *p)
{
    int n = snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE,
                     "AT+CCHOPEN=%d,\"%.*s\",%u,2",
                     SESSION_ID,
                     (int)(SSL_HOST_MAX_LEN - 1u), p->host,
                     (unsigned)p->port);
    if (n <= 0 || n >= (int)SSL_CMD_BUF_SIZE) return SSL_UPLOAD_ERR_PARAM;

    if (at_channel_send_cmd(s_cmd_buf, SSL_OPEN_TIMEOUT_MS) != AT_OK)
        return SSL_UPLOAD_ERR_OPEN;

    return wait_for_urc(p->urc_queue, HTTP_URC_CCHOPEN, SSL_OPEN_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */

/*
 * ssl_conn_close — issue AT+CCHCLOSE and drain the deferred +CCHCLOSE URC.
 * Best-effort: errors are swallowed; modem will time out the session itself.
 */
static void ssl_conn_close(const SslUploadParams_t *p)
{
    snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE, "AT+CCHCLOSE=%d", SESSION_ID);
    if (at_channel_send_cmd(s_cmd_buf, SSL_CLOSE_TIMEOUT_MS) == AT_OK)
        (void)wait_for_urc(p->urc_queue, HTTP_URC_CCHCLOSE, SSL_CLOSE_TIMEOUT_MS);
}

/* -------------------------------------------------------------------------- */

/*
 * ssl_send_headers — send s_hdr_buf (hdr_len bytes) as one AT+CCHSEND.
 * The byte count in the command string MUST equal hdr_len exactly, because
 * at_channel_send_binary() streams exactly hdr_len bytes after the '>' prompt.
 */
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

/*
 * ssl_stream_body — stream file bytes over the open TLS session via repeated
 * AT+CCHSEND calls (one per SSL_FETCH_WINDOW slice).
 *
 * Before each slice the URC queue is peeked non-blocking:
 *   +CCH_PEER_CLOSED or +CCH:CCH STOP → session dead; return ERR_PEER.
 *   Any other URC → returned to queue front (wait_for_urc may need it).
 *
 * *bytes_sent_io is incremented after each successful window and passed to
 * progress_cb.  It lives on the caller's stack, keeping the session struct
 * free of the bytes_sent field.
 */
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
            xQueueSendToFront(p->urc_queue, &urc, 0);
        }

        const uint16_t window = (remaining > SSL_FETCH_WINDOW)
                                 ? (uint16_t)SSL_FETCH_WINDOW
                                 : (uint16_t)remaining;

        if (!p->fetch_cb(p->fetch_ctx, pos, s_fetch_buf, window))
            return SSL_UPLOAD_ERR_FETCH;

        /* Byte count in AT command MUST equal window */
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
 * ssl_read_response — issue AT+CCHRECV, capture the response, parse HTTP status.
 *
 * "+CCHRECV" is absent from the AT channel's URC recogniser, so the lines
 *   "+CCHRECV:DATA,0,<n>"  and  "HTTP/1.1 NNN reason"
 * arrive as informational text and land in s_cap_buf.
 *
 * HTTP status is parsed manually from the three digits immediately following
 * "HTTP/1.1 " (offset +9):
 *
 *   if s[9..11] are all ASCII digits:
 *       code = (s[9]-'0')*100 + (s[10]-'0')*10 + (s[11]-'0')
 *
 * This replaces sscanf("%3d"), avoiding pulling ~1-2 KB of scanf machinery
 * into the firmware image when only snprintf is otherwise needed.
 *
 * 2xx → SSL_UPLOAD_OK.  Any other code → SSL_UPLOAD_ERR_HTTP.
 * AT error or missing status line → SSL_UPLOAD_ERR_RECV.
 *
 * Takes no params — reads from/writes to module statics only.
 */
static SslUploadResult_t ssl_read_response(void)
{
    at_channel_set_capture(s_cap_buf, SSL_RECV_CAP_SIZE);   /* before send */

    snprintf(s_cmd_buf, SSL_CMD_BUF_SIZE, "AT+CCHRECV=%d,0", SESSION_ID);
    const AtResult_t ar = at_channel_send_cmd(s_cmd_buf, SSL_RECV_TIMEOUT_MS);

    at_channel_set_capture(NULL, 0);   /* always clear */

    if (ar != AT_OK) return SSL_UPLOAD_ERR_RECV;

    /* Locate "HTTP/1.1 " in capture buffer */
    const char *s = strstr(s_cap_buf, "HTTP/1.1 ");
    if (!s) return SSL_UPLOAD_ERR_RECV;

    /*
     * s[9], s[10], s[11] are the three status-code digits.
     * Verify buffer bounds and that all three are decimal digits.
     * (s_cap_buf is null-terminated so strstr result + 12 is safe if
     *  the captured string is at least 12 chars — HTTP/1.1 + space + 3 digits.)
     */
    const unsigned char d0 = (unsigned char)(s[9]  - '0');
    const unsigned char d1 = (unsigned char)(s[10] - '0');
    const unsigned char d2 = (unsigned char)(s[11] - '0');

    if (d0 > 9u || d1 > 9u || d2 > 9u) return SSL_UPLOAD_ERR_RECV;

    const int code = (int)d0 * 100 + (int)d1 * 10 + (int)d2;
    return (code >= 200 && code < 300) ? SSL_UPLOAD_OK : SSL_UPLOAD_ERR_HTTP;
}

/* ══════════════════════════ Header builder ══════════════════════════════════*/

/*
 * build_headers — assemble one HTTP header block into s_hdr_buf.
 *
 *   with_range = false:
 *     POST <path> HTTP/1.1\r\n
 *     Host: <host>\r\n
 *     Content-Type: application/octet-stream\r\n
 *     Content-Length: <body_len>\r\n
 *     Connection: close\r\n\r\n
 *
 *   with_range = true:
 *     POST <path> HTTP/1.1\r\n
 *     Host: <host>\r\n
 *     Content-Type: application/octet-stream\r\n
 *     Content-Length: <body_len>\r\n
 *     Content-Range: bytes <range_first>-<range_last>/<file_size>\r\n
 *     Connection: close\r\n\r\n
 *
 * Content-Length always = body_len (bytes transmitted in this request),
 * not file_size.  RFC 9110 §8.6.
 *
 * Returns bytes written on success; -1 on truncation (path/host too long).
 */
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
 * wait_for_urc — drain the URC queue until the target event arrives or the
 * deadline elapses.  Polls in 200 ms increments.
 *
 * HTTP_URC_CCHOPEN: validates SESSION_ID; param==0 → OK, param≠0 → ERR_OPEN.
 * HTTP_URC_CCHCLOSE: any outcome accepted; timeout also non-fatal.
 * HTTP_URC_CCH_PEER_CLOSED / HTTP_URC_CCH_STOP: abort → ERR_PEER.
 * All other types: discarded.
 */
static SslUploadResult_t wait_for_urc(QueueHandle_t q,
                                       HttpUrcType_t  want,
                                       uint32_t       timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    HttpUrcEvent_t   ev;

    while (xTaskGetTickCount() < deadline)
    {
        TickType_t remaining = deadline - xTaskGetTickCount();
        TickType_t wait      = pdMS_TO_TICKS(200u);
        if (wait > remaining) wait = remaining;

        if (xQueueReceive(q, &ev, wait) != pdPASS) continue;

        if (ev.type == want)
        {
            if (want == HTTP_URC_CCHOPEN)
            {
                if (ev.client_idx != (int8_t)SESSION_ID) continue;
                return (ev.param == 0) ? SSL_UPLOAD_OK : SSL_UPLOAD_ERR_OPEN;
            }
            return SSL_UPLOAD_OK;   /* CCHCLOSE — any result is fine */
        }

        if (ev.type == HTTP_URC_CCH_PEER_CLOSED ||
            ev.type == HTTP_URC_CCH_STOP)
            return SSL_UPLOAD_ERR_PEER;
    }

    /* Timeout — non-fatal only for CCHCLOSE */
    return (want == HTTP_URC_CCHCLOSE) ? SSL_UPLOAD_OK : SSL_UPLOAD_ERR_OPEN;
}

/* ══════════════════════════ Inline utilities ════════════════════════════════*/

static inline bool validate_params(const SslUploadParams_t  *p,
                                    const SslUploadSession_t *s)
{
    return p && s
        && p->host      && p->host[0] != '\0'
        && p->path      && p->path[0] != '\0'
        && p->fetch_cb
        && p->file_size > 0u
        && p->urc_queue;
}

static inline uint8_t resolve_retries(const SslUploadParams_t *p)
{
    return p->max_retries ? p->max_retries : SSL_MAX_RETRIES;
}
