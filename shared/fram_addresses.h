/**
 * @file    fram_addresses.h
 * @brief   Single source of truth for all CY15B116QN FRAM address constants.
 *
 * The CY15B116QN is a 2 Mb (2 097 152 byte) SPI F-RAM.
 * Its 21-bit address space is divided into two logical 1 MB halves:
 *
 *   First 1 MB  (0x000000–0x0FFFFF): Database + Config
 *   Second 1 MB (0x100000–0x1FFFFF): OTA Staging
 *
 * In addition, the device has a 256-byte "Special Sector" accessed via
 * dedicated opcodes (CMD_SSRD / CMD_SSWR).  The nv_database module uses
 * the Special Sector to store Operation_Data_t and Meta_Data_t; those
 * accesses are managed internally by cy15b116qn.c and are NOT covered by
 * any address defined in this file.
 *
 * Rules
 * -----
 * - NEVER hard-code hex FRAM addresses elsewhere in the codebase.
 * - Always use the named constants from this file.
 * - The compiler will catch typos in constant names; it will NOT catch a
 *   transposed hex literal.
 *
 * This file is compiled into BOTH the application and the bootloader
 * (it lives in shared/ which is included by both build environments).
 */

#ifndef FRAM_ADDRESSES_H
#define FRAM_ADDRESSES_H

#include <stdint.h>

/* ── Physical device limits ─────────────────────────────────────── */

/** Total addressable bytes in the CY15B116QN (2 Mb). */
#define FRAM_TOTAL_SIZE     0x200000UL

/** Highest valid byte address in the main array (inclusive). */
#define FRAM_MAX_ADDR       0x1FFFFFUL

/* ── DB Region — first 1 MB, reserved for weather record ring buffer ── */

/** Byte address of the first DB ring-buffer slot. */
#define FRAM_DB_BASE        0x000000UL

/**
 * Byte address of the last allowed DB ring-buffer byte (inclusive).
 * Leaves room for the 4 KB Config sector that starts at FRAM_CONFIG_BASE.
 * (~1,016 KB usable; actual ring-buffer occupancy ≈ 576 KB for 32 768 × 18 B records)
 */
#define FRAM_DB_END         0x0FDFEFUL

/* ── Config Sector — last 8 KB of the first 1 MB ────────────────── */

/** Start of the 4 KB configuration sector. */
#define FRAM_CONFIG_BASE    0x0FE000UL

/** Last byte of the configuration sector (inclusive). */
#define FRAM_CONFIG_END     0x0FEFFFUL

/**
 * OtaControlBlock_t primary copy.
 * The OCB is 64 bytes; both copies must be written atomically with CRC.
 * Bytes 0x0FE000–0x0FE07F are reserved for OTA URL configuration strings
 * (defined when OTA manager is implemented in Phase 3).
 */
#define FRAM_OCB_PRIMARY    0x0FE080UL   /* OtaControlBlock_t primary copy (64 B) */

/** OtaControlBlock_t mirror (redundant) copy. */
#define FRAM_OCB_MIRROR     0x0FE0C0UL   /* OtaControlBlock_t mirror copy  (64 B) */

/* ── OTA Staging Region — full second 1 MB ───────────────────────── */

/** Base address of the OTA staging region. */
#define FRAM_STAGING_BASE   0x100000UL

/**
 * Staging header (256 bytes).
 * Stores firmware version, image size, CRC, and download state.
 */
#define FRAM_STAGING_HEADER 0x100000UL   /* staging header (256 B) */

/**
 * Start of the raw firmware binary image in staging.
 * Maximum image size: FRAM_STAGING_BITMAP - FRAM_STAGING_IMAGE = 512 KB.
 */
#define FRAM_STAGING_IMAGE  0x100100UL   /* raw .bin image data (≤ 512 KB) */

/**
 * Download resume bitmap (1 KB).
 * Each bit represents one 512-byte chunk; set = received.
 */
#define FRAM_STAGING_BITMAP 0x17F000UL   /* download resume bitmap (1 KB) */

/**
 * CRC-32 footer (4 bytes).
 * Written last, after the entire image is downloaded, to signal completion.
 */
#define FRAM_STAGING_CRC    0x17F400UL   /* CRC-32 footer (4 B), written last */

/** Last byte of the staging region (inclusive). */
#define FRAM_STAGING_END    0x1FFFFFUL

/* ── Derived size macros ─────────────────────────────────────────── */

/** Maximum firmware image size that fits in staging (bytes). */
#define FRAM_STAGING_IMAGE_MAX_SIZE \
    (FRAM_STAGING_BITMAP - FRAM_STAGING_IMAGE)   /* 520 KB */

/** Size of the download resume bitmap region (bytes). */
#define FRAM_STAGING_BITMAP_SIZE \
    (FRAM_STAGING_CRC - FRAM_STAGING_BITMAP)     /* 1 KB */

/* ── Application Flash partition limit ──────────────────────────────────── */

/**
 * Maximum application firmware size that can be programmed into Flash.
 * Derived from ldscript_app.ld: FLASH origin 0x08008000, length 480 KB.
 * See OTA_Firmware_Architecture.md §6.
 * Any staged image exceeding this value cannot fit in the application partition
 * and must be rejected before download begins (app) or Flash write begins (bootloader).
 */
#define FLASH_APP_SIZE_MAX  (480u * 1024u)

#endif /* FRAM_ADDRESSES_H */
