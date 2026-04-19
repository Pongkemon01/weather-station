/**
 * @file  a7670_ssl_downloader.c
 * @brief OTA firmware download via the A7670E HTTP(S) service (AT+HTTP* commands).
 *
 * AT command sequence per session:
 *   ssl_downloader_start:
 *     AT+HTTPINIT                          → OK
 *     AT+HTTPPARA="SSLCFG",0              → OK  (uses context pre-configured in Modem_Module_Init)
 *
 *   ssl_downloader_get (per request):
 *     AT+HTTPPARA="URL","https://..."      → OK
 *     AT+HTTPACTION=0                      → OK  (immediate)
 *     <wait for +HTTPACTION: 0,<status>,<datalen> URC>
 *     AT+HTTPREAD=0,<datalen>              → OK + +HTTPREAD: <n>\r\n<data>\r\n+HTTPREAD: 0
 *
 *   ssl_downloader_stop:
 *     AT+HTTPTERM                          → OK
 *
 * RAM layout (static .bss):
 *   s_cmd_buf[224]  224 B  AT command assembly (HTTPPARA URL, max 191-char URL)
 *   Total static:   224 B  (plus one QueueHandle_t pointer)
 *
 * Not re-entrant — only OtaManagerTask may call these functions.
 */

#include "a7670_ssl_downloader.h"
#include "a7670_at_channel.h"
#include "crc32.h"

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* ─────────────────────────── Static state ───────────────────────────────── */

static QueueHandle_t s_urc_queue;
static char          s_cmd_buf[224];

/* Internal read buffer for ssl_downloader_get_chunk(): chunk data + 4-byte CRC. */
static uint8_t       s_chunk_read_buf[HTTPS_DL_CHUNK_WITH_CRC_SIZE];

/* ─────────────────────────── Private helpers ────────────────────────────── */

/*
 * wait_for_httpaction — drain the URC queue until a +HTTPACTION event arrives
 * or the deadline elapses.  Non-matching URCs are discarded.  On success,
 * the event is written to *ev_out and SSL_DL_OK is returned.
 */
/* Wrap-safe elapsed-ticks check (mirrors BUG-AT-4 fix in at_channel.c). */
#define DL_TICKS_ELAPSED(start)        ((TickType_t)(xTaskGetTickCount() - (start)))
#define DL_DEADLINE_PASSED(start, tot) (DL_TICKS_ELAPSED(start) >= (tot))

static SslDlResult_t wait_for_httpaction(uint32_t        timeout_ms,
                                          HttpUrcEvent_t *ev_out)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);
    HttpUrcEvent_t   ev;

    while (!DL_DEADLINE_PASSED(start, total))
    {
        TickType_t elapsed   = DL_TICKS_ELAPSED(start);
        TickType_t remaining = (elapsed < total) ? (total - elapsed) : 0u;
        TickType_t wait      = pdMS_TO_TICKS(200u);
        if (wait > remaining) wait = remaining;

        if (xQueueReceive(s_urc_queue, &ev, wait) != pdPASS)
            continue;

        if (ev.type == HTTP_URC_HTTPACTION)
        {
            *ev_out = ev;
            return SSL_DL_OK;
        }
        /* Discard +HTTP_PEER_CLOSED, +HTTP_NONET_EVENT, etc. */
    }

    return SSL_DL_ERR_GET;
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/

SslDlResult_t ssl_downloader_start(QueueHandle_t urc_queue)
{
    if (urc_queue == NULL)
        return SSL_DL_ERR_PARAM;

    s_urc_queue = urc_queue;

    /* Start HTTP service — activates PDP context if not already up. */
    if (at_channel_send_cmd("AT+HTTPINIT", HTTPS_DL_INIT_MS) != AT_OK)
        return SSL_DL_ERR_INIT;

    /* Bind SSL context 0 (configured once in Modem_Module_Init). */
    if (at_channel_send_cmd("AT+HTTPPARA=\"SSLCFG\",0", HTTPS_DL_PARAM_MS) != AT_OK)
        return SSL_DL_ERR_SSL_CFG;

    return SSL_DL_OK;
}

/* -------------------------------------------------------------------------- */

SslDlResult_t ssl_downloader_get(const char *full_url,
                                  uint8_t    *buf,
                                  uint16_t    max_len,
                                  uint16_t   *received)
{
    if (full_url == NULL || buf == NULL || max_len == 0u || received == NULL)
        return SSL_DL_ERR_PARAM;

    *received = 0u;

    /* Set the request URL. */
    int n = snprintf(s_cmd_buf, sizeof(s_cmd_buf),
                     "AT+HTTPPARA=\"URL\",\"%.*s\"",
                     (int)(HTTPS_DL_URL_MAX_LEN - 1u), full_url);
    if (n <= 0 || n >= (int)sizeof(s_cmd_buf))
        return SSL_DL_ERR_PARAM;

    if (at_channel_send_cmd(s_cmd_buf, HTTPS_DL_PARAM_MS) != AT_OK)
        return SSL_DL_ERR_URL;

    /* Issue the GET.  The modem responds OK immediately; +HTTPACTION URC follows. */
    if (at_channel_send_cmd("AT+HTTPACTION=0", HTTPS_DL_PARAM_MS) != AT_OK)
        return SSL_DL_ERR_GET;

    HttpUrcEvent_t ev;
    if (wait_for_httpaction(HTTPS_DL_ACTION_MS, &ev) != SSL_DL_OK)
        return SSL_DL_ERR_GET;

    if (ev.statuscode < 200u || ev.statuscode >= 300u)
        return SSL_DL_ERR_HTTP;

    if (ev.datalen == 0u)
        return SSL_DL_OK;   /* empty response body — treat as success */

    /* Clamp read size to buffer capacity. */
    uint16_t read_len = (ev.datalen < (uint32_t)max_len)
                        ? (uint16_t)ev.datalen : max_len;

    AtResult_t ar = at_channel_http_read(0u, read_len, buf, max_len,
                                          received, HTTPS_DL_READ_MS);
    return (ar == AT_OK) ? SSL_DL_OK : SSL_DL_ERR_READ;
}

/* -------------------------------------------------------------------------- */

SslDlResult_t ssl_downloader_get_chunk(const char *full_url,
                                        uint8_t    *chunk_buf,
                                        uint16_t    max_chunk_len,
                                        uint16_t   *chunk_received)
{
    if (full_url == NULL || chunk_buf == NULL || max_chunk_len == 0u
        || chunk_received == NULL)
        return SSL_DL_ERR_PARAM;

    *chunk_received = 0u;

    int n = snprintf(s_cmd_buf, sizeof(s_cmd_buf),
                     "AT+HTTPPARA=\"URL\",\"%.*s\"",
                     (int)(HTTPS_DL_URL_MAX_LEN - 1u), full_url);
    if (n <= 0 || n >= (int)sizeof(s_cmd_buf))
        return SSL_DL_ERR_PARAM;

    if (at_channel_send_cmd(s_cmd_buf, HTTPS_DL_PARAM_MS) != AT_OK)
        return SSL_DL_ERR_URL;

    if (at_channel_send_cmd("AT+HTTPACTION=0", HTTPS_DL_PARAM_MS) != AT_OK)
        return SSL_DL_ERR_GET;

    HttpUrcEvent_t ev;
    if (wait_for_httpaction(HTTPS_DL_ACTION_MS, &ev) != SSL_DL_OK)
        return SSL_DL_ERR_GET;

    if (ev.statuscode < 200u || ev.statuscode >= 300u)
        return SSL_DL_ERR_HTTP;

    /* Response must be at least 5 bytes: 1 byte data + 4 byte CRC trailer. */
    if (ev.datalen < 5u)
        return SSL_DL_ERR_READ;

    uint16_t total_len = (ev.datalen <= (uint32_t)sizeof(s_chunk_read_buf))
                         ? (uint16_t)ev.datalen
                         : (uint16_t)sizeof(s_chunk_read_buf);
    uint16_t total_received;

    AtResult_t ar = at_channel_http_read(0u, total_len, s_chunk_read_buf,
                                          (uint16_t)sizeof(s_chunk_read_buf),
                                          &total_received, HTTPS_DL_READ_MS);
    if (ar != AT_OK || total_received < 5u)
        return SSL_DL_ERR_READ;

    uint16_t data_len = total_received - 4u;

    /* Decode 4-byte little-endian CRC-32 trailer. */
    uint32_t expected_crc =  (uint32_t)s_chunk_read_buf[data_len]
                           | ((uint32_t)s_chunk_read_buf[data_len + 1u] <<  8u)
                           | ((uint32_t)s_chunk_read_buf[data_len + 2u] << 16u)
                           | ((uint32_t)s_chunk_read_buf[data_len + 3u] << 24u);

    if (crc32_update(CRC32_INIT_VALUE, s_chunk_read_buf, data_len) != expected_crc)
        return SSL_DL_ERR_CRC;

    /* CRC valid — copy chunk data to caller's buffer. */
    if (data_len > max_chunk_len)
        data_len = max_chunk_len;

    memcpy(chunk_buf, s_chunk_read_buf, data_len);
    *chunk_received = data_len;
    return SSL_DL_OK;
}

/* -------------------------------------------------------------------------- */

SslDlResult_t ssl_downloader_stop(void)
{
    if (at_channel_send_cmd("AT+HTTPTERM", HTTPS_DL_STOP_MS) != AT_OK)
        return SSL_DL_ERR_STOP;

    return SSL_DL_OK;
}
