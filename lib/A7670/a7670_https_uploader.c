/**
 * @file  a7670_https_uploader.c
 * @brief Weather data upload via the A7670E HTTP(S) service (AT+HTTP* commands).
 *
 * AT command sequence per session:
 *   https_uploader_start:
 *     AT+HTTPINIT                                → OK
 *     AT+HTTPPARA="SSLCFG",0                     → OK  (context pre-configured in Modem_Module_Init)
 *
 *   https_uploader_post (per batch):
 *     AT+HTTPPARA="URL","https://..."            → OK
 *     AT+HTTPPARA="CONTENT","application/octet-stream" → OK
 *     AT+HTTPDATA=<len>,30                       → DOWNLOAD  (prompt dispatched as AT_PROMPT)
 *     <send len bytes of binary body>            → OK
 *     AT+HTTPACTION=1                            → OK  (immediate)
 *     <wait for +HTTPACTION: 1,<status>,<datalen> URC>
 *
 *   https_uploader_stop:
 *     AT+HTTPTERM                                → OK
 *
 * RAM layout (static .bss):
 *   s_fetch_buf[512]  512 B  assembled POST body (header + packed weather records)
 *   s_cmd_buf[256]    256 B  AT command assembly (HTTPPARA URL, HTTPDATA)
 *   Total static:     768 B  (plus one QueueHandle_t pointer)
 *
 * Not re-entrant — only SslUploadTask may call these functions.
 */

#include "a7670_https_uploader.h"
#include "a7670_at_channel.h"

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* ─────────────────────────── Static state ───────────────────────────────── */

static QueueHandle_t s_urc_queue;
static uint8_t       s_fetch_buf[HTTPS_UL_FETCH_WINDOW];
static char          s_cmd_buf[256];

/* ─────────────────────────── Private helpers ────────────────────────────── */

/*
 * wait_for_httpaction — drain the URC queue until a +HTTPACTION event arrives
 * or the deadline elapses.  Non-matching URCs are discarded.  On success,
 * the event is written to *ev_out and HTTPS_UL_OK is returned.
 */
/* Wrap-safe elapsed-ticks check (mirrors BUG-AT-4 fix in at_channel.c). */
#define UL_TICKS_ELAPSED(start)        ((TickType_t)(xTaskGetTickCount() - (start)))
#define UL_DEADLINE_PASSED(start, tot) (UL_TICKS_ELAPSED(start) >= (tot))

static HttpsUlResult_t wait_for_httpaction(uint32_t        timeout_ms,
                                            HttpUrcEvent_t *ev_out)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);
    HttpUrcEvent_t   ev;

    while (!UL_DEADLINE_PASSED(start, total))
    {
        TickType_t elapsed   = UL_TICKS_ELAPSED(start);
        TickType_t remaining = (elapsed < total) ? (total - elapsed) : 0u;
        TickType_t wait      = pdMS_TO_TICKS(200u);
        if (wait > remaining) wait = remaining;

        if (xQueueReceive(s_urc_queue, &ev, wait) != pdPASS)
            continue;

        if (ev.type == HTTP_URC_HTTPACTION)
        {
            *ev_out = ev;
            return HTTPS_UL_OK;
        }
        /* Discard +HTTP_PEER_CLOSED, +HTTP_NONET_EVENT, etc. */
    }

    return HTTPS_UL_ERR_POST;
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/

HttpsUlResult_t https_uploader_start(QueueHandle_t urc_queue)
{
    if (urc_queue == NULL)
        return HTTPS_UL_ERR_PARAM;

    s_urc_queue = urc_queue;

    /* Start HTTP service — activates PDP context if not already up. */
    if (at_channel_send_cmd("AT+HTTPINIT", HTTPS_UL_INIT_MS) != AT_OK)
        return HTTPS_UL_ERR_INIT;

    /* Bind SSL context 0 (configured once in Modem_Module_Init). */
    if (at_channel_send_cmd("AT+HTTPPARA=\"SSLCFG\",0", HTTPS_UL_PARAM_MS) != AT_OK)
        return HTTPS_UL_ERR_INIT;

    return HTTPS_UL_OK;
}

/* -------------------------------------------------------------------------- */

HttpsUlResult_t https_uploader_post(const char      *full_url,
                                     HttpsUlFetchCb_t fetch_cb,
                                     void            *ctx,
                                     uint16_t         len)
{
    if (full_url == NULL || fetch_cb == NULL || len == 0u ||
        len > HTTPS_UL_FETCH_WINDOW)
        return HTTPS_UL_ERR_PARAM;

    /* 1. Set the POST URL. */
    int n = snprintf(s_cmd_buf, sizeof(s_cmd_buf),
                     "AT+HTTPPARA=\"URL\",\"%.*s\"",
                     (int)(HTTPS_UL_URL_MAX_LEN - 1u), full_url);
    if (n <= 0 || n >= (int)sizeof(s_cmd_buf))
        return HTTPS_UL_ERR_PARAM;

    if (at_channel_send_cmd(s_cmd_buf, HTTPS_UL_PARAM_MS) != AT_OK)
        return HTTPS_UL_ERR_URL;

    /* 2. Set content-type for binary blob. */
    if (at_channel_send_cmd("AT+HTTPPARA=\"CONTENT\",\"application/octet-stream\"",
                             HTTPS_UL_PARAM_MS) != AT_OK)
        return HTTPS_UL_ERR_URL;

    /* 3. Assemble the POST body via fetch callback. */
    uint16_t filled = fetch_cb(ctx, s_fetch_buf, HTTPS_UL_FETCH_WINDOW);
    if (filled != len)
        return HTTPS_UL_ERR_PARAM;

    /* 4. Prime the modem POST buffer.
     *    AT+HTTPDATA=<size>,<timeout_s> → modem replies "DOWNLOAD" (AT_PROMPT)
     *    → send binary body → modem replies OK.
     *    The 30-second timeout is the modem-side window to receive the body. */
    n = snprintf(s_cmd_buf, sizeof(s_cmd_buf), "AT+HTTPDATA=%u,30", (unsigned)len);
    if (n <= 0 || n >= (int)sizeof(s_cmd_buf))
        return HTTPS_UL_ERR_POST;

    if (at_channel_send_binary(s_cmd_buf, s_fetch_buf, len, HTTPS_UL_DATA_MS) != AT_OK)
        return HTTPS_UL_ERR_POST;

    /* 5. Issue the POST.  Modem returns OK immediately; +HTTPACTION URC follows. */
    if (at_channel_send_cmd("AT+HTTPACTION=1", HTTPS_UL_PARAM_MS) != AT_OK)
        return HTTPS_UL_ERR_POST;

    /* 6. Wait for +HTTPACTION: 1,<status>,<datalen> URC. */
    HttpUrcEvent_t ev;
    if (wait_for_httpaction(HTTPS_UL_ACTION_MS, &ev) != HTTPS_UL_OK)
        return HTTPS_UL_ERR_POST;

    /* 7. Verify HTTP 2xx status.  Discard response body (datalen not used). */
    if (ev.statuscode < 200u || ev.statuscode >= 300u)
        return HTTPS_UL_ERR_HTTP;

    return HTTPS_UL_OK;
}

/* -------------------------------------------------------------------------- */

HttpsUlResult_t https_uploader_stop(void)
{
    if (at_channel_send_cmd("AT+HTTPTERM", HTTPS_UL_STOP_MS) != AT_OK)
        return HTTPS_UL_ERR_STOP;

    return HTTPS_UL_OK;
}
