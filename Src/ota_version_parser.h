/**
 * @file  ota_version_parser.h
 * @brief Parser for OTA version-check server response tokens.
 *
 * Response format: V.<uint32>:L.<uint32>:H.<64hex>[:W.<uint32>]
 *
 * This module has no OS or HAL dependencies and is compiled into both
 * the application and native host tests.
 */

#ifndef OTA_VERSION_PARSER_H
#define OTA_VERSION_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Scan buf[0..len-1] for a valid OTA version token.
 *
 * Extracts version, image size, SHA-256 digest, and optional rollout wait time.
 * The :W.<seconds> suffix is optional; absent means wait_seconds = 0.
 *
 * @param  buf          Buffer to search.
 * @param  len          Number of valid bytes in buf.
 * @param  version      Receives the firmware version number.
 * @param  img_size     Receives the image size in bytes.
 * @param  sha256_out   Receives the 32-byte SHA-256 digest.
 * @param  wait_seconds Receives the rollout wait time (0 = download now).
 * @return true on first valid match; false if no valid token is found.
 */
bool ovp_parse(const uint8_t *buf, uint16_t len,
               uint32_t *version, uint32_t *img_size,
               uint8_t sha256_out[32], uint32_t *wait_seconds);

#ifdef __cplusplus
}
#endif

#endif /* OTA_VERSION_PARSER_H */
