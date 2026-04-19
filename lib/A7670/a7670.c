#include "FreeRTOS.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>
#include "a7670.h"
#include "a7670_at_channel.h"
#include "a7670_https_uploader.h"
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
 * BUG-A7670-1  (time sync: AT+CNTP URC was never awaited)
 *   AT+CNTP="server",tz   only configures the NTP parameters.
 *   AT+CNTP (no args)     triggers the actual NTP synchronisation.
 *   The modem replies OK immediately; the "+CNTP: <err>" URC arrives up to
 *   10 s later when the server exchange completes.  The previous code used
 *   at_channel_send_cmd() which returned on OK, so the URC was never
 *   checked — the clock may still have been at epoch 0 when TLS opened.
 *   Fixed: at_channel_send_cntp() suppresses the OK and signals completion
 *   only when "+CNTP: 0" is received.  Timeout 12 000 ms covers the 10 s
 *   maximum with 2 s margin.
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

    /* Step C: Execute NTP synchronisation and await "+CNTP: 0" URC.
     *         at_channel_send_cntp() suppresses the immediate OK and returns
     *         only when the URC confirms the clock is set (err == 0).
     *         Timeout 12 000 ms: modem manual specifies up to 10 s. */
    if (at_channel_send_cntp(12000u) != AT_OK)
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

    /* 4. Volatile SSL hardening — SSL context 0, used by AT+HTTPPARA="SSLCFG",0.
     *    Settings are lost on power cycle and must be re-applied every boot.
     *
     *    sslversion 3  = TLS 1.2 (minimum acceptable for mutual auth).
     *    authmode 2    = mutual authentication (client + server certificates).
     *    sni 1         = send SNI extension; required for virtual hosting. */
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

    /* cap contains "+CCLK: <time>" followed by '\n'.
     * Prefix is 7 chars, time is 20 chars, plus '\n' = 28. Use >= 27 for safety. */
    if (r == AT_OK && strlen(cap) >= 27u)
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
