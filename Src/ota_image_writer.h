/**
 * @file  ota_image_writer.h
 * @brief Chunked OTA firmware write to FRAM staging region with download bitmap.
 *
 * ── Staging layout (FRAM second 1 MB) ─────────────────────────────────────
 *   FRAM_STAGING_IMAGE  (0x100100) — raw .bin image (≤ 512 KB)
 *   FRAM_STAGING_BITMAP (0x17F000) — download resume bitmap (128 B used, 1 KB allocated)
 *   FRAM_STAGING_CRC    (0x17F400) — unused (whole-image CRC removed; SHA-256 in OCB)
 *
 * ── Bitmap encoding ────────────────────────────────────────────────────────
 *   One bit per 512-byte chunk.  Bit N is 1 when chunk N has been received.
 *   128 bytes × 8 bits = 1024 bits → covers the maximum 512-byte × 1024 chunks
 *   = 512 KB image.  The bitmap is shadowed in RAM (s_bitmap[128]) for fast
 *   access and written back to FRAM on every oiw_write_chunk() call.
 *
 * ── Calling convention ────────────────────────────────────────────────────
 *   // Fresh download or resume
 *   oiw_begin(image_size);   // loads bitmap from FRAM
 *
 *   // Resume: find where to start
 *   uint16_t next;
 *   while (oiw_resume_info(&next)) {
 *       // fetch chunk at offset next * OIW_CHUNK_SIZE
 *       oiw_write_chunk(next, chunk_data, chunk_len);
 *   }
 *
 *   // Seal the staging area
 *   oiw_finalize();
 *
 * ── Thread safety ──────────────────────────────────────────────────────────
 *   The caller MUST hold g_fram_spi_mutex before calling any function that
 *   touches FRAM (oiw_begin, oiw_write_chunk, oiw_finalize).
 *   oiw_chunk_received() and oiw_resume_info() operate on the RAM bitmap only
 *   and do not require the mutex.
 */

#ifndef OTA_IMAGE_WRITER_H
#define OTA_IMAGE_WRITER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════ Tuning ══════════════════════════════════════════*/

/** Bytes per staging chunk — must match AT+CHTTPSRECV request size. */
#define OIW_CHUNK_SIZE    512u

/** Bitmap size in bytes: 1024 chunks / 8 bits = 128 B. */
#define OIW_BITMAP_BYTES  128u

/** Maximum image size the staging region can hold (512 KB). */
#define OIW_MAX_IMAGE_SIZE  (OIW_CHUNK_SIZE * (OIW_BITMAP_BYTES * 8u))

/* ══════════════════════════ Return codes ════════════════════════════════════*/

typedef enum {
    OIW_OK         = 0,
    OIW_ERR_PARAM  = 1, /**< NULL pointer or out-of-range argument.          */
    OIW_ERR_SIZE   = 2, /**< image_size exceeds OIW_MAX_IMAGE_SIZE.          */
    OIW_ERR_INDEX  = 3, /**< chunk_index >= chunk_count for this image.      */
    OIW_ERR_FRAM   = 4, /**< FRAM read or write returned fewer bytes.        */
} OiwResult_t;

/* ══════════════════════════ Public API ══════════════════════════════════════*/

/**
 * @brief  Initialise writer state and load the download bitmap from FRAM.
 *
 * Must be called once before any oiw_write_chunk() call.  On a fresh download
 * the FRAM bitmap region should be zeroed beforehand by the OTA manager;
 * on resume it is loaded as-is so previously received chunks are not re-fetched.
 *
 * Caller MUST hold g_fram_spi_mutex.
 *
 * @param  image_size  Total firmware image size in bytes (≤ OIW_MAX_IMAGE_SIZE).
 * @return OIW_OK, OIW_ERR_SIZE, or OIW_ERR_FRAM.
 */
OiwResult_t oiw_begin(uint32_t image_size);

/**
 * @brief  Write one chunk to the FRAM staging area and mark it as received.
 *
 * Writes @p len bytes to FRAM at FRAM_STAGING_IMAGE + chunk_index * OIW_CHUNK_SIZE,
 * sets bit chunk_index in the RAM bitmap, then writes the updated bitmap back to
 * FRAM_STAGING_BITMAP.
 *
 * Caller MUST hold g_fram_spi_mutex.
 *
 * @param  chunk_index  Zero-based chunk number (< total chunk count for this image).
 * @param  data         Source buffer containing the chunk payload.
 * @param  len          Number of valid bytes in @p data (≤ OIW_CHUNK_SIZE).
 * @return OIW_OK, OIW_ERR_INDEX, OIW_ERR_PARAM, or OIW_ERR_FRAM.
 */
OiwResult_t oiw_write_chunk(uint16_t chunk_index, const uint8_t *data, uint16_t len);

/**
 * @brief  Check whether chunk @p chunk_index has already been received.
 *
 * Operates on the in-RAM bitmap — does NOT touch FRAM.
 * oiw_begin() must have been called first.
 *
 * @param  chunk_index  Zero-based chunk number.
 * @return true if the chunk is present; false otherwise.
 */
bool oiw_chunk_received(uint16_t chunk_index);

/**
 * @brief  Seal the staging area after all chunks have been received.
 *
 * No-op at the FRAM level — image integrity is provided by SHA-256 stored
 * in OtaControlBlock_t.image_sha256, computed and written by the OTA manager
 * after this call.  Call oiw_resume_info() first to confirm all chunks are
 * present before calling oiw_finalize().
 *
 * @return OIW_OK always.
 */
OiwResult_t oiw_finalize(void);

/**
 * @brief  Scan the bitmap for the first missing chunk.
 *
 * Used to determine the resume offset after a power-loss or retry.
 * Operates on the in-RAM bitmap — does NOT touch FRAM.
 *
 * @param[out] next_missing  Set to the index of the first chunk whose bit is 0.
 *                           Undefined when the function returns false.
 * @return true  if at least one chunk is missing (@p next_missing is valid).
 * @return false if all chunks for this image have been received (download complete).
 */
bool oiw_resume_info(uint16_t *next_missing);

#ifdef __cplusplus
}
#endif

#endif /* OTA_IMAGE_WRITER_H */
