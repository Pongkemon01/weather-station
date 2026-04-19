/**
 * @file  ota_image_writer.c
 * @brief Chunked OTA firmware write to FRAM staging region with download bitmap.
 *
 * RAM layout (static .bss):
 *   s_bitmap[128]   128 B  in-RAM shadow of FRAM_STAGING_BITMAP
 *   s_image_size      4 B  total image size for this session
 *   s_chunk_count     2 B  ceil(s_image_size / OIW_CHUNK_SIZE)
 *   Total:          134 B
 */

#include "ota_image_writer.h"
#include "cy15b116qn.h"
#include "fram_addresses.h"

#include <string.h>

/* ─────────────────────────── Static state ───────────────────────────────── */

static uint8_t  s_bitmap[OIW_BITMAP_BYTES];
static uint32_t s_image_size;
static uint16_t s_chunk_count;

/* ─────────────────────────── Private helpers ────────────────────────────── */

static inline void bitmap_set(uint16_t idx)
{
    s_bitmap[idx >> 3u] |= (uint8_t)(1u << (idx & 7u));
}

static inline bool bitmap_get(uint16_t idx)
{
    return (bool)((s_bitmap[idx >> 3u] >> (idx & 7u)) & 1u);
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/

OiwResult_t oiw_begin(uint32_t image_size)
{
    if (image_size == 0u || image_size > OIW_MAX_IMAGE_SIZE)
        return OIW_ERR_SIZE;

    s_image_size  = image_size;
    s_chunk_count = (uint16_t)((image_size + OIW_CHUNK_SIZE - 1u) / OIW_CHUNK_SIZE);

    /* Load existing bitmap from FRAM — preserves progress on resume. */
    uint32_t got = fram_read(FRAM_STAGING_BITMAP, s_bitmap, OIW_BITMAP_BYTES);
    if (got != OIW_BITMAP_BYTES)
        return OIW_ERR_FRAM;

    return OIW_OK;
}

/* -------------------------------------------------------------------------- */

OiwResult_t oiw_write_chunk(uint16_t chunk_index,
                              const uint8_t *data,
                              uint16_t len)
{
    if (data == NULL || len == 0u)
        return OIW_ERR_PARAM;
    if (chunk_index >= s_chunk_count)
        return OIW_ERR_INDEX;
    if (len > OIW_CHUNK_SIZE)
        return OIW_ERR_PARAM;

    /* Write chunk payload to FRAM staging image region. */
    uint32_t addr = FRAM_STAGING_IMAGE + (uint32_t)chunk_index * OIW_CHUNK_SIZE;
    if (fram_write(addr, data, len) != (uint32_t)len)
        return OIW_ERR_FRAM;

    /* Mark chunk as received in the RAM bitmap and persist to FRAM. */
    bitmap_set(chunk_index);
    if (fram_write(FRAM_STAGING_BITMAP, s_bitmap, OIW_BITMAP_BYTES)
            != OIW_BITMAP_BYTES)
        return OIW_ERR_FRAM;

    return OIW_OK;
}

/* -------------------------------------------------------------------------- */

bool oiw_chunk_received(uint16_t chunk_index)
{
    if (chunk_index >= s_chunk_count)
        return false;
    return bitmap_get(chunk_index);
}

/* -------------------------------------------------------------------------- */

OiwResult_t oiw_finalize(void)
{
    /* No FRAM writes needed — whole-image integrity is provided by SHA-256
     * in OtaControlBlock_t.image_sha256, written by the OTA manager.
     * Per-chunk CRC-32 (validated during download) ensures transfer correctness. */
    return OIW_OK;
}

/* -------------------------------------------------------------------------- */

bool oiw_resume_info(uint16_t *next_missing)
{
    if (next_missing == NULL)
        return false;

    for (uint16_t i = 0u; i < s_chunk_count; i++)
    {
        if (!bitmap_get(i))
        {
            *next_missing = i;
            return true;
        }
    }

    return false;   /* all chunks received — download complete */
}
