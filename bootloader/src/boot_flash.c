/**
 * @file    boot_flash.c
 * @brief   STM32L476 Flash programming for the bootloader.
 *
 * Reads pages from FRAM staging (FRAM_STAGING_IMAGE onwards) and programs
 * them into the application Flash partition (Bank 1, pages 16–255).
 *
 * Write sequence per page:
 *   1. Refresh IWDG.
 *   2. boot_fram_read() → static page_buf (2 KB, 8-byte aligned).
 *   3. HAL_FLASH_Unlock().
 *   4. HAL_FLASHEx_Erase() one page.
 *   5. 256 × HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, ...).
 *   6. HAL_FLASH_Lock().
 *   7. memcmp() Flash vs page_buf — return false on mismatch.
 *
 * Partial last page:
 *   If image_size is not a multiple of FLASH_PAGE_SIZE, the remainder of
 *   page_buf is zero-filled before programming so every double-word write
 *   targets a valid value (erased Flash reads 0xFFFFFFFFFFFFFFFF — but we
 *   zero-pad to avoid programming half-written 64-bit words which would
 *   prevent a future re-program without erase).
 */

#include "boot_flash.h"
#include "boot_fram.h"
#include "fram_addresses.h"

#include <string.h>
#include <stddef.h>

/* ── Flash page size (bytes) — STM32L476 Bank 1/2 ──────────────────── */
#ifndef FLASH_PAGE_SIZE
#define FLASH_PAGE_SIZE  2048u
#endif

/* ── Double-words per page ──────────────────────────────────────────── */
#define DW_PER_PAGE  (FLASH_PAGE_SIZE / sizeof(uint64_t))   /* 256 */

/* ── Static page buffer (8-byte aligned for double-word writes) ─────── */
static uint64_t page_buf[DW_PER_PAGE];

/* ── Private helpers ────────────────────────────────────────────────── */

/**
 * @brief  Erase one Flash page in Bank 1.
 * @param  page  0-based page number (16–255 for the application partition).
 * @return true on success.
 */
static bool erase_page(uint32_t page)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t page_error = 0u;
    HAL_StatusTypeDef st;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks     = FLASH_BANK_1;
    erase.Page      = page;
    erase.NbPages   = 1u;

    st = HAL_FLASHEx_Erase(&erase, &page_error);
    return (st == HAL_OK) && (page_error == 0xFFFFFFFFu);
}

/**
 * @brief  Program one full page from page_buf into Flash at dst_addr.
 * @param  dst_addr  Must be 8-byte aligned.
 * @return true on success.
 */
static bool program_page(uint32_t dst_addr)
{
    for (uint32_t dw = 0u; dw < DW_PER_PAGE; dw++)
    {
        uint32_t word_addr = dst_addr + (dw * sizeof(uint64_t));

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              word_addr,
                              page_buf[dw]) != HAL_OK)
        {
            return false;
        }
    }
    return true;
}

/* ================================================================ */
/* Public API                                                        */
/* ================================================================ */

/**
 * @brief  Program the application Flash partition from FRAM staging.
 */
bool boot_flash_program(uint32_t image_size, IWDG_HandleTypeDef *hiwdg_ptr)
{
    uint32_t max_size;
    uint32_t pages_needed;
    uint32_t page;
    uint32_t bytes_remaining;
    uint32_t bytes_this_page;
    uint32_t fram_src;
    uint32_t flash_dst;

    if (hiwdg_ptr == NULL || image_size == 0u)
        return false;

    max_size = (uint32_t)APP_TOTAL_PAGES * FLASH_PAGE_SIZE;
    if (image_size > max_size)
        return false;

    pages_needed    = (image_size + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE;
    bytes_remaining = image_size;

    for (page = APP_FIRST_PAGE; page < APP_FIRST_PAGE + pages_needed; page++)
    {
        /* Keep the watchdog alive throughout the erase/write loop. */
        HAL_IWDG_Refresh(hiwdg_ptr);

        fram_src  = FRAM_STAGING_IMAGE
                    + (page - APP_FIRST_PAGE) * FLASH_PAGE_SIZE;
        flash_dst = FLASH_APP_ORIGIN
                    + (page - APP_FIRST_PAGE) * FLASH_PAGE_SIZE;

        /* Determine how many valid bytes go into this page. */
        bytes_this_page = (bytes_remaining >= FLASH_PAGE_SIZE)
                          ? FLASH_PAGE_SIZE
                          : bytes_remaining;

        /* Read firmware data from FRAM into page_buf. */
        if (boot_fram_read(fram_src, (uint8_t *)page_buf, bytes_this_page) != bytes_this_page)
            return false;

        /* Zero-pad the remainder of the last partial page. */
        if (bytes_this_page < FLASH_PAGE_SIZE)
        {
            memset((uint8_t *)page_buf + bytes_this_page,
                   0x00,
                   FLASH_PAGE_SIZE - bytes_this_page);
        }

        /* Unlock Flash, erase the page, program, lock. */
        if (HAL_FLASH_Unlock() != HAL_OK)
            return false;

        if (!erase_page(page))
        {
            HAL_FLASH_Lock();
            return false;
        }

        if (!program_page(flash_dst))
        {
            HAL_FLASH_Lock();
            return false;
        }

        HAL_FLASH_Lock();

        /* Reset D-cache before read-back verify to avoid stale cache hits.
         * The cache must be disabled before reset and re-enabled after. */
        __HAL_FLASH_DATA_CACHE_DISABLE();
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();

        /* Read-back verify: compare programmed Flash against page_buf. */
        if (memcmp((const void *)flash_dst, page_buf, FLASH_PAGE_SIZE) != 0)
            return false;

        bytes_remaining = (bytes_remaining > FLASH_PAGE_SIZE)
                          ? bytes_remaining - FLASH_PAGE_SIZE
                          : 0u;
    }

    /* Reset I-cache so the new application's instructions are fetched fresh. */
    __HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
    __HAL_FLASH_INSTRUCTION_CACHE_RESET();
    __HAL_FLASH_INSTRUCTION_CACHE_ENABLE();

    return true;
}
