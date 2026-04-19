/**
 * @file    ota_control_block.h
 * @brief   OtaControlBlock_t — persistent OTA handshake between application
 *          and bootloader.
 *
 * Two copies (primary + mirror) are stored in the FRAM Config Sector:
 *   Primary : FRAM_OCB_PRIMARY  (0x0FE080)
 *   Mirror  : FRAM_OCB_MIRROR   (0x0FE0C0)
 *
 * Write protocol (both copies must succeed):
 *   1. Compute block_crc32 over bytes [0..59].
 *   2. Write primary; read back and verify CRC.
 *   3. Write mirror; read back and verify CRC.
 *
 * Read protocol:
 *   Try primary — if magic + CRC valid, use it.
 *   Else try mirror — if valid, use it.
 *   Else return OTA_ERR_BOTH_COPIES.
 *
 * CRC algorithm (block integrity only): CRC-32/MPEG-2
 *   Polynomial 0x04C11DB7, initial value 0xFFFFFFFF, no XorOut,
 *   no input/output reflection.  Matches STM32 HW CRC unit default.
 *
 * Image integrity: SHA-256 only (image_sha256[32]).
 *   Per-chunk CRC-32 is validated during download by a7670_ssl_downloader.
 *   No whole-image CRC-32 is stored or computed.
 *
 * This file is compiled into BOTH the application and the bootloader
 * (it lives in shared/ which both build environments include).
 */

#ifndef OTA_CONTROL_BLOCK_H
#define OTA_CONTROL_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

/** Magic word that must appear in every valid OCB. */
#define OCB_MAGIC           0x0AC0FFEEul

/** Value of ota_pending when a firmware update is waiting for the bootloader. */
#define OCB_PENDING_FLAG    0x01u

/** Value of ota_confirmed once the new firmware has proved stable. */
#define OCB_CONFIRMED_FLAG  0x01u

/** Maximum boot attempts before the bootloader rolls back. */
#define OCB_MAX_TRIES       3u

/* ── Status codes ─────────────────────────────────────────────────────── */

typedef enum
{
    OTA_OK = 0,           /**< Success                                      */
    OTA_ERR_NULL,         /**< NULL pointer argument                        */
    OTA_ERR_INVALID,      /**< Magic mismatch or CRC failure                */
    OTA_ERR_FRAM_READ,    /**< FRAM read returned fewer bytes than expected  */
    OTA_ERR_FRAM_WRITE,   /**< FRAM write returned fewer bytes than expected */
    OTA_ERR_BOTH_COPIES,  /**< Both primary and mirror copies are corrupt    */
} OtaStatus_t;

/* ── OtaControlBlock_t — 64 bytes, packed ─────────────────────────────── */

/**
 * @brief  Persistent OTA control record stored at FRAM_OCB_PRIMARY and
 *         FRAM_OCB_MIRROR.  Must be exactly 64 bytes.
 *
 * Field ordering is fixed — do not reorder without updating both
 * ocb_crc32() coverage and the CLAUDE.md struct documentation.
 */
typedef struct __attribute__((packed))
{
    uint32_t magic;              /**< OCB_MAGIC (0x0AC0FFEE)                 */
    uint8_t  ota_pending;        /**< OCB_PENDING_FLAG = update awaiting boot */
    uint8_t  ota_tried;          /**< Incremented by bootloader on each try  */
    uint8_t  ota_confirmed;      /**< OCB_CONFIRMED_FLAG set by app on success*/
    uint8_t  pad0;               /**< Reserved — write 0                     */
    uint32_t image_size;         /**< Staged image size in bytes              */
    uint8_t  image_sha256[32];   /**< SHA-256 digest of the full staged image */
    uint32_t fw_version;         /**< Monotonic firmware version number       */
    uint8_t  reserved[8];        /**< Future use — write 0                    */
    uint32_t download_timestamp; /**< Y2K epoch when download completed       */
    uint32_t block_crc32;        /**< CRC-32/MPEG-2 over bytes [0..59]        */
} OtaControlBlock_t;

/** Compile-time size guard — struct must be exactly 64 bytes. */
typedef char ocb_size_check_t[(sizeof(OtaControlBlock_t) == 64u) ? 1 : -1];

/* ── Public API ───────────────────────────────────────────────────────── */

/**
 * @brief  Read the OTA control block from FRAM.
 *
 * Attempts to read the primary copy first.  If the primary copy has a bad
 * magic word or a CRC mismatch the mirror copy is tried.  The caller
 * receives the first valid copy found.
 *
 * In the application build the caller must hold g_fram_spi_mutex.
 *
 * @param[out] out  Caller-allocated OtaControlBlock_t buffer.
 * @return    OTA_OK, OTA_ERR_NULL, OTA_ERR_FRAM_READ, or OTA_ERR_BOTH_COPIES.
 */
OtaStatus_t ocb_read(OtaControlBlock_t *out);

/**
 * @brief  Write the OTA control block to FRAM (primary then mirror).
 *
 * block_crc32 is recomputed over bytes [0..59] before writing so the
 * caller does not need to fill it in.
 *
 * In the application build the caller must hold g_fram_spi_mutex.
 *
 * @param[in] in  Block to write (block_crc32 is ignored on input).
 * @return    OTA_OK, OTA_ERR_NULL, OTA_ERR_FRAM_WRITE, or OTA_ERR_INVALID
 *            (if read-back after write fails).
 */
OtaStatus_t ocb_write(const OtaControlBlock_t *in);

/**
 * @brief  Invalidate both copies by writing zeroes to the FRAM addresses.
 *
 * Used after a completed or failed OTA cycle to guarantee that the
 * bootloader will not attempt to reprogram on the next reset.
 *
 * @return OTA_OK or OTA_ERR_FRAM_WRITE.
 */
OtaStatus_t ocb_clear(void);

/**
 * @brief  Validate magic word and block_crc32 of a decoded block.
 *
 * @param[in] cb  Block to validate (may be NULL — returns false).
 * @return    true if magic == OCB_MAGIC and CRC matches.
 */
bool ocb_is_valid(const OtaControlBlock_t *cb);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CONTROL_BLOCK_H */
