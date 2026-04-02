#include "FreeRTOS.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>
#include "a7670.h"
#include "a7670_at_channel.h"
#include "a7670_ssl_uploader.h"
#include "a7670_ssl_cert_manager.h"

/* ─────────────────────────── Private constants ──────────────────────────── */

/*
 * NTP server used for time sync.
 * timezone offset +28 = UTC+7 (Thailand Standard Time, ICT).
 * The modem stores this as quarters-of-an-hour, so 28 = +7 h.
 * time.navy.mi.th is the Royal Thai Navy time server.
 */
#define NTP_SERVER "time.navy.mi.th"
#define NTP_TZ_OFFSET "28"

/* ─────────────────────────── Certificates var ───────────────────────────── */

extern const uint8_t server_der[];
extern const size_t server_der_len;
extern const uint8_t client_der[];
extern const size_t client_der_len;
extern const uint8_t client_key_der[];
extern const size_t client_key_der_len;

/* ─────────────────────────── Private globals ─────────────────────────────── */

static UART_Ctx_t *modem_ctx = NULL;
static QueueHandle_t urc_queue = NULL;

/* ─────────────────────────── Private helpers ─────────────────────────────── */

/*
 * Modem_Module_Init — run once per power-on (or reset) to configure the modem.
 *
 * Sequence:
 *   1. Flush stale RX data, verify modem is alive.
 *   2. Time sync via NTP (mandatory before TLS — the modem uses system time
 *      to validate certificate validity windows).
 *   3. Inject certificates (idempotent — skip if already present).
 *   4. Apply volatile SSL configuration (lost on power cycle).
 *
 * =============================================================================
 * BUG-A7670-1  (time sync: AT+CNTP has no effect without AT+CNTP execute)
 *   AT+CNTP="server",tz   only configures the NTP parameters.
 *   AT+CNTP (no args)     triggers the actual NTP synchronisation.
 *   The original code issued both but with only 1 000 ms timeout for the
 *   execute step.  The modem manual states the NTP response URC can take up
 *   to 10 000 ms.  The timeout was already 10 000 ms, which is correct — but
 *   verified here for clarity.
 *
 * BUG-A7670-2  (time sync: AT+CTZU=1 enables automatic time-zone update from
 *   the network, but does NOT by itself set the clock.  On networks that do not
 *   broadcast NITZ, the RTC stays at the modem's default epoch.  AT+CNTP is
 *   therefore always required.  Both steps must be present — they are NOT
 *   alternatives.  The code was already correct; comment added for clarity.)
 *
 * BUG-A7670-3  (cert injection: ssl_cert_inject may return CERT_ERR_EXISTS
 *   legitimately on the first boot if flash survived a warm reset.  The code
 *   already handled this correctly.  However, there was no timeout on the
 *   AT+CCERTDOWN command inside ssl_cert_inject for large certificates.
 *   That is a concern in a7670_ssl_cert_manager.c, noted here for traceability.)
 *
 * BUG-A7670-4  (SSL config: AT+CCHSET=1 enables the CCH receive-data URC
 *   mode.  If this is not set, +CCHEVENT URCs are not generated and the
 *   uploader's ssl_stream_body peer-closed check never fires.  It was already
 *   present in the original; no change required.)
 * =============================================================================
 */
static bool Modem_Module_Init(void)
{
    CertStatus_t cert_status;
    char cmd_buf[64];

    if (modem_ctx == NULL || urc_queue == NULL)
        return false;

    /* 1. Check modem is ready */
    if (!UART_Sys_FlushReceive(modem_ctx))
        return false;
    if (at_channel_ping_modem(2000u, 5u) != AT_OK)
        return false;

    /* 2. Time sync
     *
     * Step A: Enable automatic network time-zone update (NITZ).
     *         This sets the RTC when the operator broadcasts NITZ.
     *         On networks without NITZ the next NTP step is the fallback.
     */
    if (at_channel_send_cmd("AT+CTZU=1", 1000u) != AT_OK)
        return false;

    /* Step B: Configure NTP server and timezone. */
    snprintf(cmd_buf, sizeof(cmd_buf),
             "AT+CNTP=\"%s\",%s", NTP_SERVER, NTP_TZ_OFFSET);
    if (at_channel_send_cmd(cmd_buf, 1000u) != AT_OK)
        return false;

    /* Step C: Execute NTP synchronisation.
     *         The modem queries the NTP server and updates the RTC.
     *         Timeout 10 000 ms — modem manual specifies up to 10 s.
     *         Without a valid RTC, TLS certificate validation will reject
     *         any certificate whose validity window doesn't include epoch 0. */
    if (at_channel_send_cmd("AT+CNTP", 10000u) != AT_OK)
        return false;

    /* 3. Upload certificates
     *    CERT_ERR_EXISTS means already present from a previous boot — skip. */
    cert_status = ssl_cert_inject("server.der", server_der, server_der_len);
    if (cert_status != CERT_OK && cert_status != CERT_ERR_EXISTS)
        return false;

    cert_status = ssl_cert_inject("client.der", client_der, client_der_len);
    if (cert_status != CERT_OK && cert_status != CERT_ERR_EXISTS)
        return false;

    cert_status = ssl_cert_inject("client_key.der", client_key_der, client_key_der_len);
    if (cert_status != CERT_OK && cert_status != CERT_ERR_EXISTS)
        return false;

    /* 4. Volatile SSL hardening (reset on every power cycle)
     *
     *    sslversion 3  = TLS 1.2 (minimum acceptable for mutual auth).
     *    authmode 2    = mutual authentication (client + server certificates).
     *    sni 1         = send SNI extension; required for virtual hosting.
     *    cchset 1      = enable CCH unsolicited receive-data notifications. */
    if (at_channel_send_cmd("AT+CSSLCFG=\"sslversion\",0,3", 500u) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"authmode\",0,2", 500u) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"cacert\",0,\"server.der\"", 500u) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"clientcert\",0,\"client.der\"", 500u) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"clientkey\",0,\"client_key.der\"", 500u) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"sni\",0,1", 500u) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CCHSET=1", 500u) != AT_OK)
        return false;

    return true;
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/

bool modem_init(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
        return false;

    if ((urc_queue = xQueueCreate(10u, sizeof(HttpUrcEvent_t))) == NULL)
        return false;

    modem_ctx = UART_Sys_Register(huart);
    if (modem_ctx == NULL)
    {
        vQueueDelete(urc_queue);
        urc_queue = NULL;
        return false;
    }

    if (!at_channel_init(modem_ctx, urc_queue))
    {
        UART_Sys_UnRegister(modem_ctx);
        modem_ctx = NULL;
        vQueueDelete(urc_queue);
        urc_queue = NULL;
        return false;
    }

    if (!Modem_Module_Init())
    {
        modem_deinit();
        return false;
    }

    if (at_channel_wait_ready(10000u, 5u) != AT_READY_OK)
    {
        modem_deinit();
        return false;
    }

    return true;
}

/* -------------------------------------------------------------------------- */

void modem_deinit(void)
{
    at_channel_deinit();
    if (modem_ctx)
    {
        UART_Sys_UnRegister(modem_ctx);
        modem_ctx = NULL;
    }
    if (urc_queue)
    {
        vQueueDelete(urc_queue);
        urc_queue = NULL;
    }
}

/* -------------------------------------------------------------------------- */

bool modem_is_init(void)
{
    return (modem_ctx != NULL);
}

/* -------------------------------------------------------------------------- */

QueueHandle_t modem_get_urc_queue(void)
{
    return urc_queue;
}

/* -------------------------------------------------------------------------- */

/**
 * modem_get_datetime — issue AT+CCLK? and return the datetime string.
 *
 * @param buf  Caller-supplied buffer, minimum 21 bytes.
 *             Filled with the 20-character time string "yy/MM/dd,hh:mm:ss±zz"
 *             plus a null terminator.
 * @return true on success.
 */
bool modem_get_datetime(char *buf)
{
    char cap[32];

    if (buf == NULL)
        return false;

    buf[0] = '\0';

    at_channel_set_capture(cap, sizeof(cap));
    AtResult_t r = at_channel_send_cmd("AT+CCLK?", 1000u);
    at_channel_set_capture(NULL, 0);

    /* cap contains "+CCLK: <time>" — prefix is 7 chars, time is 20 chars. */
    if (r == AT_OK && strlen(cap) == 27u)
    {
        memcpy(buf, &cap[7], 20u);
        buf[20] = '\0';
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------- */

/**
 * modem_set_datetime — issue AT+CCLK="<time>".
 *
 * @param buf  Datetime string "yy/MM/dd,hh:mm:ss±zz" (exactly 20 chars).
 * @return true on success.
 */
bool modem_set_datetime(const char *buf)
{
    char cmd[32];

    if (buf == NULL || strlen(buf) != 20u)
        return false;

    snprintf(cmd, sizeof(cmd), "AT+CCLK=\"%s\"", buf);
    return (at_channel_send_cmd(cmd, 1000u) == AT_OK);
}
