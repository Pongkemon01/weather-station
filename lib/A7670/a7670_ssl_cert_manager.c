/**
 * @file    a7670_ssl_cert_manager.c
 * @brief   SSL certificate management — minimal implementation
 *
 * AT command flows
 * ────────────────
 *
 * ssl_cert_inject():
 *   → AT+CCERTDOWN="ca.pem",1234\r\n
 *   ← > (prompt)
 *   → <1234 bytes of PEM>
 *   ← OK
 *   → AT+CCERTLIST  (confirm storage)
 *   ← +CCERTLIST: "ca.pem"
 *   ← OK
 *
 * ssl_cert_exists():
 *   → AT+CCERTLIST\r\n
 *   ← +CCERTLIST: "ca.pem","client.crt"
 *   ← OK
 *   (searches capture for target name, returns bool)
 *
 * ssl_cert_delete():
 *   (ssl_cert_exists check first)
 *   → AT+CCERTDELE="ca.pem"\r\n
 *   ← OK
 *
 * Capture buffer sizing
 * ─────────────────────
 * CERTLIST_CAPTURE_SIZE (160 B) holds the +CCERTLIST informational line.
 * It does NOT need to store parsed name strings — cert_is_present() uses
 * strstr() directly on the raw capture.  Sized for a practical maximum of
 * 4 certificates on an embedded device:
 *
 *   "+CCERTLIST: " (12) + 4 × ('"' + 31 chars + '"' + ',') (4×34=136) = 148
 *   Rounded to 160 for safety.
 *
 * Compared to the previous 300-byte buffer: -140 B per call-site stack frame.
 */

#include "a7670_at_channel.h"
#include "a7670_ssl_cert_manager.h"

#include <string.h>
#include <stdio.h>

/* ─────────────────────────── Private constants ──────────────────────────── */

/**
 * Stack buffer for the raw AT+CCERTLIST response line.
 * Searched directly with strstr() — no name array needed.
 */
#define CERTLIST_CAPTURE_SIZE   160u

/* ─────────────────────────── Private helpers ────────────────────────────── */

/**
 * @brief  Return true if `name` appears as a quoted token in the
 *         AT+CCERTLIST capture buffer.
 *
 * The modem emits:  +CCERTLIST: "ca.pem","client.crt"
 * We search for:   "ca.pem"  (with surrounding quotes) to avoid partial
 * matches (e.g. "ca" matching inside "ca_new.pem").
 *
 * The quoted-search string is built on a 34-byte stack buffer
 * ('"' + up to 31 chars + '"' + '\0').
 */
static bool cert_is_present(const char *capture, const char *name)
{
    if (!capture || !name || name[0] == '\0') 
        return false;

    /* Build `"name"` for exact-token matching */
    char quoted[CERT_NAME_MAX_LEN + 2u];  /* 34 B on stack */
    int n = snprintf(quoted, sizeof(quoted), "\"%s\"", name);
    if (n <= 0 || (size_t)n >= sizeof(quoted)) 
        return false;

    return (strstr(capture, quoted) != NULL);
}

/**
 * @brief  Issue AT+CCERTLIST and return the raw capture in buf.
 *
 * @param buf   Caller-supplied buffer (CERTLIST_CAPTURE_SIZE bytes).
 * @return AT_OK on success, AT_ERROR if FS is empty, AT_TIMEOUT etc.
 */
static AtResult_t certlist_raw(char *buf)
{
    at_channel_set_capture(buf, CERTLIST_CAPTURE_SIZE);
    AtResult_t r = at_channel_send_cmd("AT+CCERTLIST", CERT_MGMT_TIMEOUT_MS);
    at_channel_set_capture(NULL, 0);
    return r;
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/

CertStatus_t ssl_cert_inject(const char    *name,
                             const uint8_t *pem_data,
                             size_t         pem_len)
{
    if (!name || !pem_data || pem_len == 0) 
        return CERT_ERR_PARAM;
    if (strlen(name) >= CERT_NAME_MAX_LEN)  
        return CERT_ERR_PARAM;

    if (ssl_cert_exists(name)) 
        return CERT_ERR_EXISTS;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CCERTDOWN=\"%s\",%zu", name, pem_len);

    AtResult_t r = at_channel_send_binary(cmd, pem_data, pem_len,
                                          CERT_DOWNLOAD_TIMEOUT_MS);
    if (r != AT_OK) 
    {
        return ((r == AT_TIMEOUT) ? CERT_ERR_TIMEOUT : CERT_ERR_MODEM);
    }

    /* Confirm the modem actually stored it */
    if (!ssl_cert_exists(name)) 
        return CERT_ERR_MODEM;

    return CERT_OK;
}

/* -------------------------------------------------------------------------- */

bool ssl_cert_exists(const char *name)
{
    if (!name || name[0] == '\0') 
        return false;

    char cap[CERTLIST_CAPTURE_SIZE];
    AtResult_t r = certlist_raw(cap);

    /*
     * AT+CCERTLIST returns ERROR on some firmware versions when the
     * filesystem is empty.  Both ERROR and empty capture mean "not present".
     */
    if (r != AT_OK) 
        return false;

    return cert_is_present(cap, name);
}

/* -------------------------------------------------------------------------- */

CertStatus_t ssl_cert_delete(const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) >= CERT_NAME_MAX_LEN) 
        return CERT_ERR_PARAM;

    if (!ssl_cert_exists(name)) 
        return CERT_ERR_NOT_FOUND;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CCERTDELE=\"%s\"", name);

    AtResult_t r = at_channel_send_cmd(cmd, CERT_MGMT_TIMEOUT_MS);
    if (r == AT_OK)      
        return CERT_OK;
    if (r == AT_TIMEOUT) 
        return CERT_ERR_TIMEOUT;
    return CERT_ERR_MODEM;
}
