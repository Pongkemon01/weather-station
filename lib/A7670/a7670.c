#include "FreeRTOS.h"
#include "queue.h"
#include <stdio.h>
#include "a7670.h"
#include "a7670_at_channel.h"
#include "a7670_ssl_uploader.h"
#include "a7670_ssl_cert_manager.h"

/* ─────────────────────────── Private constant ───────────────────────────── */
#define HTTP_TIMEOUT_MS     30000

/* ─────────────────────────── Certificates var ───────────────────────────── */
extern const uint8_t server_der[];
extern const size_t server_der_len;
extern const uint8_t client_der[];
extern const size_t client_der_len;
extern const uint8_t client_key_der[];
extern const size_t client_key_der_len;

/* ─────────────────────────── Private Globals ─────────────────────────────── */
static UART_Ctx_t *modem_ctx = NULL;
static QueueHandle_t urc_queue = NULL;      // Create once, used through out the system

/* ══════════════════════════ Public API ══════════════════════════════════════*/

static bool Modem_Module_Init(void)
{
    CertStatus_t cert_status;

    if(modem_ctx == NULL || urc_queue == NULL)
        return false;

    // 1. Check whether modem is ready
    if(!(UART_Sys_FlushReceive(modem_ctx)))   // Flush all previous data
        return false;
    if (at_channel_ping_modem(2000u, 5u) != AT_OK)
        return false;

    // 2. Mandatory Time Sync (Must do every reset)
    if (at_channel_send_cmd("AT+CTZU=1", 1000) != AT_OK)
        return false;
    if(at_channel_send_cmd("AT+CNTP=\"time.navy.mi.th\",28", 1000) != AT_OK)
        return false;
    if(at_channel_send_cmd("AT+CNTP", 10000) != AT_OK)
        return false;

    // 3. Upload certificates (if the specified certificate name already exists, just skip it)
    cert_status = ssl_cert_inject("server.der", server_der, server_der_len);
    if(cert_status != SSL_CERT_OK && cert_status != SSL_CERT_EXISTS)
        return false;
    cert_status = ssl_cert_inject("client.der", client_der, client_der_len);
    if(cert_status != SSL_CERT_OK && cert_status != SSL_CERT_EXISTS)
        return false;
    cert_status = ssl_cert_inject("client_key.der", client_key_der, client_key_der_len);
    if(cert_status != SSL_CERT_OK && cert_status != SSL_CERT_EXISTS)
        return false;

    // 4. Re-apply Volatile Hardening (Must do every reset)
    // These settings are lost when the modem loses power
    if (at_channel_send_cmd("AT+CSSLCFG=\"sslversion\",0,3", 500) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"authmode\",0,2", 500) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"cacert\",0,\"server.der\"", 500) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"clientcert\",0,\"client.der\"", 500) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"clientkey\",0,\"client_key.der\"", 500) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CSSLCFG=\"sni\",0,1", 500) != AT_OK)
        return false;
    if (at_channel_send_cmd("AT+CCHSET=1", 500) != AT_OK)
        return false;

    return true;
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/
bool modem_init(UART_Ctx_t *ctx)
{
    if(ctx == NULL)
        return false;

    modem_ctx = ctx;
    if((urc_queue = xQueueCreate(10, sizeof(HttpUrcEvent_t))) == NULL)
    {
        modem_ctx = NULL;
        return false;
    }

    if(!at_channel_init(modem_ctx, urc_queue))
    {
        modem_ctx = NULL;
        xQueueDelete(urc_queue);
        urc_queue = NULL;
        return false;
    }

    if(!(Modem_Module_Init()))
    {
        Modem_Deinit();
        return false;
    }

    if(at_channel_wait_ready(10000u, 5u) != AT_READY_OK)
    {
        Modem_Deinit();
        return false;
    }

    return true;
}

/* -------------------------------------------------------------------------- */

void modem_deinit(void)
{
    at_channel_deinit();
    modem_ctx = NULL;
    xQueueDelete(urc_queue);
    urc_queue = NULL;
}

/* -------------------------------------------------------------------------- */
bool modem_is_init(void)
{
    return (modem_ctx != NULL);
}

/* -------------------------------------------------------------------------- */
QueueHandle_t modem_get_urc_queue(void)
{
    return(urc_queue);
}