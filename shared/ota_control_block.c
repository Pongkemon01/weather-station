/**
 * @file    ota_control_block.c
 * @brief   Dual-copy OTA Control Block read/write for FRAM config sector.
 *
 * CRC algorithm
 * -------------
 * CRC-32/MPEG-2: polynomial 0x04C11DB7, initial value 0xFFFFFFFF, no
 * XorOut, no input/output bit-reflection.  The shared table-based
 * implementation in crc32.c is used in both bootloader and application.
 * The STM32 hardware CRC unit is not used here because it is configured
 * for CRC-16/Modbus by modbus_init() and cannot be shared.
 *
 * FRAM backend selection
 * ----------------------
 * When BOOTLOADER_BUILD is defined (bootloader environment):
 *   boot_fram_read() / boot_fram_write() — polling SPI, no RTOS.
 *
 * Otherwise (application environment):
 *   fram_read() / fram_write()           — cy15b116qn driver.
 *   The caller must hold g_fram_spi_mutex before calling any ocb_* function.
 */

#include "ota_control_block.h"
#include "crc32.h"
#include "fram_addresses.h"

#include <stddef.h>
#include <string.h>

#ifdef BOOTLOADER_BUILD
#   include "boot_fram.h"
#   define ocb_raw_read(addr, buf, len)  boot_fram_read((addr), (buf), (len))
#   define ocb_raw_write(addr, buf, len) boot_fram_write((addr), (buf), (len))
#else
#   include "cy15b116qn.h"
#   define ocb_raw_read(addr, buf, len)  fram_read((addr), (buf), (len))
#   define ocb_raw_write(addr, buf, len) fram_write((addr), (buf), (len))
#endif

/* ── Private helpers ───────────────────────────────────────────────────── */

/**
 * @brief  Attempt to read one OCB copy from FRAM and validate it.
 * @param  addr  FRAM address (FRAM_OCB_PRIMARY or FRAM_OCB_MIRROR).
 * @param  out   Destination buffer.
 * @return OTA_OK if magic and CRC are valid, otherwise an error code.
 */
static OtaStatus_t read_one_copy(uint32_t addr, OtaControlBlock_t *out)
{
    if (ocb_raw_read(addr, (uint8_t *)out, sizeof(OtaControlBlock_t))
        != sizeof(OtaControlBlock_t))
    {
        return OTA_ERR_FRAM_READ;
    }

    return ocb_is_valid(out) ? OTA_OK : OTA_ERR_INVALID;
}

/**
 * @brief  Write one OCB copy to FRAM and read it back to confirm.
 * @param  addr  FRAM address (FRAM_OCB_PRIMARY or FRAM_OCB_MIRROR).
 * @param  in    Block to write (block_crc32 must already be set).
 * @return OTA_OK on success, OTA_ERR_FRAM_WRITE or OTA_ERR_INVALID on error.
 */
static OtaStatus_t write_one_copy(uint32_t addr, const OtaControlBlock_t *in)
{
    OtaControlBlock_t verify;

    if (ocb_raw_write(addr, (const uint8_t *)in, sizeof(OtaControlBlock_t))
        != sizeof(OtaControlBlock_t))
    {
        return OTA_ERR_FRAM_WRITE;
    }

    /* Read back and verify integrity. */
    if (ocb_raw_read(addr, (uint8_t *)&verify, sizeof(OtaControlBlock_t))
        != sizeof(OtaControlBlock_t))
    {
        return OTA_ERR_FRAM_WRITE;
    }

    if (memcmp(in, &verify, sizeof(OtaControlBlock_t)) != 0)
        return OTA_ERR_INVALID;

    return OTA_OK;
}

/* ── Public API ────────────────────────────────────────────────────────── */

/**
 * @brief  Read the OTA control block (primary, then mirror fallback).
 */
OtaStatus_t ocb_read(OtaControlBlock_t *out)
{
    if (out == NULL)
        return OTA_ERR_NULL;

    /* Try primary copy first. */
    if (read_one_copy(FRAM_OCB_PRIMARY, out) == OTA_OK)
        return OTA_OK;

    /* Primary failed — try mirror. */
    if (read_one_copy(FRAM_OCB_MIRROR, out) == OTA_OK)
        return OTA_OK;

    /* Both copies are corrupt. */
    memset(out, 0, sizeof(OtaControlBlock_t));
    return OTA_ERR_BOTH_COPIES;
}

/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Write the OTA control block (primary then mirror).
 *
 * block_crc32 is recomputed here — the caller's value is ignored.
 */
OtaStatus_t ocb_write(const OtaControlBlock_t *in)
{
    OtaControlBlock_t work;
    OtaStatus_t st;

    if (in == NULL)
        return OTA_ERR_NULL;

    /* Copy and stamp the CRC over bytes [0..59]. */
    memcpy(&work, in, sizeof(work));
    work.block_crc32 = crc32_update(CRC32_INIT_VALUE,
                                    (const uint8_t *)&work,
                                    offsetof(OtaControlBlock_t, block_crc32));

    /* Write primary. */
    st = write_one_copy(FRAM_OCB_PRIMARY, &work);
    if (st != OTA_OK)
        return st;

    /* Write mirror. */
    return write_one_copy(FRAM_OCB_MIRROR, &work);
}

/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Overwrite both OCB copies with zeroes to invalidate them.
 */
OtaStatus_t ocb_clear(void)
{
    static const OtaControlBlock_t zero;   /* zero-initialised by C standard */

    OtaStatus_t st;

    st = write_one_copy(FRAM_OCB_PRIMARY, &zero);
    if (st != OTA_OK)
        return st;

    return write_one_copy(FRAM_OCB_MIRROR, &zero);
}

/* ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief  Validate magic word and block_crc32 of an already-decoded block.
 */
bool ocb_is_valid(const OtaControlBlock_t *cb)
{
    uint32_t expected;

    if (cb == NULL)
        return false;

    if (cb->magic != OCB_MAGIC)
        return false;

    expected = crc32_update(CRC32_INIT_VALUE,
                            (const uint8_t *)cb,
                            offsetof(OtaControlBlock_t, block_crc32));

    return (cb->block_crc32 == expected);
}
