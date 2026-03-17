/**
 * @file    a7670_ssl_cert_manager.h
 * @brief   SSL certificate management for A7670E modem — minimal API
 *
 * Public API: three functions, one return type.
 *
 *   ssl_cert_inject()   Upload a PEM file to the modem filesystem.
 *   ssl_cert_exists()   Check whether a named cert is stored (bool result).
 *   ssl_cert_delete()   Remove a named cert from the modem filesystem.
 *
  * A7670E certificate AT commands (AT Manual §11)
 * ───────────────────────────────────────────────
 *   AT+CCERTDOWN="<n>",<len>   Upload PEM to modem FS (prompt → raw bytes)
 *   AT+CCERTLIST                  List stored certificate names
 *   AT+CCERTDELE="<n>"         Delete a stored certificate
 */

#ifndef __A7670_SSL_CERT_MANAGER_H
#define __A7670_SSL_CERT_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─────────────────────────── Configuration ──────────────────────────────── */

/** Maximum certificate name length accepted by the A7670E filesystem.      */
#define CERT_NAME_MAX_LEN           32u

/** Timeout for AT+CCERTDOWN (PEM upload, ms).                              */
#define CERT_DOWNLOAD_TIMEOUT_MS    15000u

/** Timeout for AT+CCERTLIST and AT+CCERTDELE (ms).                         */
#define CERT_MGMT_TIMEOUT_MS        5000u

/* ─────────────────────────── Return codes ───────────────────────────────── */

typedef enum {
    CERT_OK           =  0,  /**< Operation succeeded                        */
    CERT_ERR_PARAM    = -1,  /**< Bad argument (NULL, name too long, etc.)   */
    CERT_ERR_MODEM    = -2,  /**< AT command returned ERROR                  */
    CERT_ERR_TIMEOUT  = -3,  /**< Modem did not respond in time              */
    CERT_ERR_NOT_FOUND = -4, /**< Named certificate not in modem FS          */
    CERT_ERR_EXISTS   = -5,  /**< Certificate already present (same name)    */
} CertStatus_t;

/* ─────────────────────────── Public API ────────────────────────────────── */

/**
 * @brief  Upload a PEM certificate to the modem filesystem.
 *
 * Issues AT+CCERTDOWN, streams the PEM bytes after the '>' prompt, then
 * confirms storage with AT+CCERTLIST before returning.
 *
 * Returns CERT_ERR_EXISTS if a cert with the same name is already stored.
 * Delete it first with ssl_cert_delete() if replacement is intended.
 *
 * @param name      Name to store under (e.g. "ca.pem"). Max CERT_NAME_MAX_LEN-1.
 * @param pem_data  Complete PEM block including -----BEGIN/END----- headers.
 * @param pem_len   Length of pem_data in bytes.
 * @return CERT_OK on success.
 */
CertStatus_t ssl_cert_inject(const char    *name,
                             const uint8_t *pem_data,
                             size_t         pem_len);

/**
 * @brief  Check whether a named certificate exists in the modem filesystem.
 *
 * Issues AT+CCERTLIST and searches the response for `name`.
 * Costs one AT round-trip (~50 ms) regardless of how many certs are stored.
 *
 * @param name  Certificate name to search for.
 * @return true if the certificate is present, false if absent or on error.
 */
bool ssl_cert_exists(const char *name);

/**
 * @brief  Delete a named certificate from the modem filesystem.
 *
 * Returns CERT_ERR_NOT_FOUND if the cert is not present (verified via
 * ssl_cert_exists() before issuing AT+CCERTDELE).
 *
 * @param name  Certificate name to delete.
 * @return CERT_OK on success.
 */
CertStatus_t ssl_cert_delete(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* __A7670_SSL_CERT_MANAGER_H */
