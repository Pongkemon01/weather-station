/**
 * @file    boot_flash.h
 * @brief   STM32L476 Flash page erase / write / verify — bootloader only.
 *
 * Reads firmware pages from FRAM staging and programs them into the
 * application Flash partition (Bank 1, pages 16–255, 0x08008000–0x0807FFFF).
 *
 * Design constraints:
 *   - HAL_FLASH_Program granularity: 64-bit double-word.
 *   - Page size: 2 KB (FLASH_PAGE_SIZE = 2048).
 *   - IWDG is refreshed before every page to survive the 240-page loop
 *     (≈ 6 s at 25 ms/page erase).
 *   - page_buf is declared static (8-byte aligned); never placed on stack.
 *   - boot_fram_init() must be called before boot_flash_program().
 */

#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include "main.h"   /* IWDG_HandleTypeDef */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Flash layout constants ─────────────────────────────────────────── */

/** First application page number (0-based from Bank 1 start). */
#define APP_FIRST_PAGE      16u

/** Last application page number. */
#define APP_LAST_PAGE       255u

/** Flash origin of the application. */
#define FLASH_APP_ORIGIN    0x08008000ul

/** Total application Flash pages available. */
#define APP_TOTAL_PAGES     ((APP_LAST_PAGE) - (APP_FIRST_PAGE) + 1u)  /* 240 */

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * @brief  Program the application Flash partition from FRAM staging.
 *
 * Reads @p image_size bytes starting at FRAM_STAGING_IMAGE, programs them
 * page-by-page into Bank 1 pages 16–255, and verifies each page with
 * memcmp after programming.
 *
 * HAL_IWDG_Refresh() is called before each page so the watchdog does not
 * fire during the 240-page loop (~6 s).
 *
 * @param  image_size  Number of valid bytes in the staged image.
 *                     Must be > 0 and ≤ APP_TOTAL_PAGES × FLASH_PAGE_SIZE.
 * @param  hiwdg_ptr   IWDG handle used to refresh the watchdog each page.
 *                     Must not be NULL.
 * @return true on complete success (all pages programmed and verified).
 *         false on any erase, write, or verify failure.
 */
bool boot_flash_program(uint32_t image_size, IWDG_HandleTypeDef *hiwdg_ptr);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_FLASH_H */
