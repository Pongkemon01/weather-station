/**
 * @file    crc32.h
 * @brief   Fast table-based CRC-32/MPEG-2 for OTA image verification.
 *
 * Algorithm parameters
 * --------------------
 *   Polynomial  : 0x04C11DB7 (non-reflected / MSB-first)
 *   Initial CRC : 0xFFFFFFFF  (use CRC32_INIT_VALUE)
 *   XorOut      : none
 *   Reflection  : none (input and output are NOT bit-reversed)
 *
 * These match the STM32 hardware CRC unit's default configuration.
 * Because the HW CRC unit is configured for CRC-16/Modbus by the RS-485
 * driver (modbus_init()), image verification uses this software implementation
 * exclusively.
 *
 * Performance (table-based, 1 table lookup per byte)
 * ---------------------------------------------------
 *   512 KB image at 80 MHz  ≈  26 ms
 *   Bit-by-bit alternative  ≈ 210 ms  (8× slower)
 *
 * Flash cost: 1 KB for the pre-computed const lookup table (.rodata).
 * RAM cost: 0 bytes — no runtime table construction.
 *
 * Usage — chunked accumulation:
 * @code
 *   uint32_t crc = CRC32_INIT_VALUE;
 *   crc = crc32_update(crc, chunk1, len1);
 *   crc = crc32_update(crc, chunk2, len2);
 *   // ... more chunks ...
 *   uint32_t result = crc;   // no final XorOut step needed
 * @endcode
 *
 * This file is compiled into both the application and the bootloader
 * (it lives in shared/ which both build environments include).
 */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pass as the initial @p crc argument to start a new CRC-32/MPEG-2 calculation. */
#define CRC32_INIT_VALUE  0xFFFFFFFFUL

/**
 * @brief  Accumulate CRC-32/MPEG-2 over a data buffer.
 *
 * May be called repeatedly to process data in chunks.
 *
 * @param  crc   Running CRC.  Pass CRC32_INIT_VALUE to begin a new calculation,
 *               or the value returned by a previous call to continue.
 * @param  data  Pointer to input bytes.  If NULL or @p len is zero the
 *               function returns @p crc unchanged.
 * @param  len   Number of bytes to process.
 * @return Updated CRC-32/MPEG-2 value.  For CRC-32/MPEG-2 this is also the
 *         final result — no additional XorOut step is required.
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC32_H */
