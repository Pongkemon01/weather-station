/**
 * @file  ota_manager_task.c
 * @brief OTA firmware update state machine (Phase 3).
 *
 * Static RAM budget:
 *   s_meta         220 B  Meta_Data_t (server_name + update_path)
 *   s_url_buf      192 B  AT command URL assembly
 *   s_io_buf       512 B  version check response AND firmware chunk buffer
 *   s_sha_ctx      108 B  SHA-256 running context
 *   s_ocb           64 B  OtaControlBlock_t working copy
 *   misc            ~8 B  state, flags, wdt id
 *   Total:        ~1104 B
 */

#include "ota_manager_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"           /* g_fram_spi_mutex, system_ready_status */
#include "cmsis_os.h"

#include "a7670.h"
#include "a7670_ssl_downloader.h"
#include "cy15b116qn.h"
#include "fram_addresses.h"
#include "nv_database.h"
#include "ota_control_block.h"
#include "ota_image_writer.h"
#include "ota_version_parser.h"
#include "sha256.h"
#include "watchdog_task.h"
#include "datetime.h"
#include "y2k_time.h"

#include <string.h>
#include <stdio.h>

#ifndef FW_VERSION
#define FW_VERSION 0u
#endif

/** 120 × WDT_PERIOD_MS (500 ms) = 60 s confirmation window. */
#define CONFIRM_TICKS       120u
#define MAX_CHUNK_RETRIES     3u

/* ── Task handle (extern declared in header) ─────────────────────────────── */
TaskHandle_t OtaManagerTaskHandle = NULL;

/* ── Static storage (no heap) ────────────────────────────────────────────── */

static int8_t            s_wdt_id;
static volatile OtaState_t s_state      = OTA_STATE_IDLE;
static volatile bool     s_confirm_req  = false;

static Meta_Data_t       s_meta;
static char              s_url_buf[192];
static uint8_t           s_io_buf[512];
static sha256_ctx_t      s_sha_ctx;
static OtaControlBlock_t s_ocb;

/* 128 zero bytes in Flash — used to clear the FRAM staging bitmap. */
static const uint8_t     k_zeros[OIW_BITMAP_BYTES];

/* ── Public API ──────────────────────────────────────────────────────────── */

void ota_confirm_success(void)
{
    s_confirm_req = true;
}

OtaState_t ota_get_state(void)
{
    return s_state;
}

/* ── Task entry point ────────────────────────────────────────────────────── */

void ota_manager_task(void *params)
{
    (void)params;

    s_wdt_id      = wdt_register("ota_mgr");
    s_confirm_req = false;

    /* Startup: detect post-OTA boot awaiting confirmation.
     * Condition: OCB valid, ota_pending cleared (bootloader did flash),
     * ota_tried > 0 (at least one boot attempt), ota_confirmed not yet set. */
    if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(5000u)) == pdTRUE) {
        if (ocb_read(&s_ocb) == OTA_OK &&
            s_ocb.ota_pending  == 0u    &&
            s_ocb.ota_tried    >  0u    &&
            s_ocb.ota_confirmed == 0u) {
            s_state = OTA_STATE_CONFIRMING;
        }
        xSemaphoreGive(g_fram_spi_mutex);
    }

    for (;;) {
        switch (s_state) {

        /* ── IDLE: wait for upload-complete notification ─────────────── */
        case OTA_STATE_IDLE:
            while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WDT_PERIOD_MS)) == 0u)
                wdt_kick(s_wdt_id);
            wdt_kick(s_wdt_id);

            if (!system_ready_status.a7670_ready) break;
            s_state = OTA_STATE_POLLING_VERSION;
            break;

        /* ── POLLING_VERSION: GET version check URL, parse V/L/H ─────── */
        case OTA_STATE_POLLING_VERSION: {
            bool meta_ok = false;
            if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(1000u)) == pdTRUE) {
                meta_ok = DB_GetMeta(&s_meta);
                xSemaphoreGive(g_fram_spi_mutex);
            }
            if (!meta_ok) { s_state = OTA_STATE_IDLE; break; }

            /* `region_id` and `station_id` are documented as 0–999 in
             * nv_database.h, but a mis-provisioned station could exceed that
             * range. Crop `% 1000` so the 6-char `?id=%03u%03u` identity
             * always matches the server's `^\d{6}$` regex (Q-S7). */
            unsigned region_mod  = (unsigned)(s_meta.region_id  % 1000u);
            unsigned station_mod = (unsigned)(s_meta.station_id % 1000u);

            int n = snprintf(s_url_buf, sizeof(s_url_buf),
                             "https://%s%s/?id=%03u%03u",
                             s_meta.server_name, s_meta.update_path,
                             region_mod, station_mod);
            if (n <= 0 || n >= (int)sizeof(s_url_buf)) { s_state = OTA_STATE_IDLE; break; }

            wdt_kick(s_wdt_id);
            if (ssl_downloader_start(modem_get_urc_queue()) != SSL_DL_OK) {
                s_state = OTA_STATE_IDLE;
                break;
            }

            uint16_t received = 0u;
            wdt_kick(s_wdt_id);
            SslDlResult_t dl = ssl_downloader_get(s_url_buf, s_io_buf,
                                                   (uint16_t)sizeof(s_io_buf),
                                                   &received);
            (void)ssl_downloader_stop();

            if (dl != SSL_DL_OK) { s_state = OTA_STATE_IDLE; break; }

            uint32_t srv_version  = 0u;
            uint32_t img_size     = 0u;
            uint32_t wait_seconds = 0u;
            if (!ovp_parse(s_io_buf, received,
                           &srv_version, &img_size,
                           s_ocb.image_sha256,
                           &wait_seconds)               ||
                srv_version <= (uint32_t)FW_VERSION          ||
                img_size    == 0u                            ||
                img_size    >  FLASH_APP_SIZE_MAX) {
                s_state = OTA_STATE_IDLE;
                break;
            }

            /* Rollout gate: server is telling us to wait our slot. Return to
             * IDLE without downloading; retry on the next scheduled poll. */
            if (wait_seconds > 0u) {
                s_state = OTA_STATE_IDLE;
                break;
            }

            /* Cache metadata for subsequent states */
            s_ocb.magic         = OCB_MAGIC;
            s_ocb.ota_pending   = 0u;
            s_ocb.ota_tried     = 0u;
            s_ocb.ota_confirmed = 0u;
            s_ocb.pad0          = 0u;
            s_ocb.image_size    = img_size;
            s_ocb.fw_version    = srv_version;
            memset(s_ocb.reserved, 0, sizeof(s_ocb.reserved));

            s_state = OTA_STATE_FETCHING_METADATA;
            break;
        }

        /* ── FETCHING_METADATA: clear bitmap, initialise writer ────────── */
        case OTA_STATE_FETCHING_METADATA: {
            bool ok = false;
            if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(5000u)) == pdTRUE) {
                /* Clear staging bitmap so SHA-256 is computed over all chunks
                 * in order (no resume; avoids stale-chunk hash mismatch). */
                uint32_t wrote = fram_write(FRAM_STAGING_BITMAP, k_zeros, OIW_BITMAP_BYTES);
                if (wrote == OIW_BITMAP_BYTES)
                    ok = (oiw_begin(s_ocb.image_size) == OIW_OK);
                xSemaphoreGive(g_fram_spi_mutex);
            }
            s_state = ok ? OTA_STATE_DOWNLOADING : OTA_STATE_IDLE;
            wdt_kick(s_wdt_id);
            break;
        }

        /* ── DOWNLOADING: fetch all chunks, accumulate SHA-256 ─────────── */
        case OTA_STATE_DOWNLOADING: {
            sha256_init(&s_sha_ctx);

            wdt_kick(s_wdt_id);
            if (ssl_downloader_start(modem_get_urc_queue()) != SSL_DL_OK) {
                s_state = OTA_STATE_IDLE;
                break;
            }

            bool     download_ok = true;
            uint16_t next_chunk  = 0u;

            while (oiw_resume_info(&next_chunk)) {
                uint32_t offset = (uint32_t)next_chunk * (uint32_t)OIW_CHUNK_SIZE;
                snprintf(s_url_buf, sizeof(s_url_buf),
                         "https://%s%s/get_firmware?offset=%lu&length=%u&id=%03u%03u",
                         s_meta.server_name, s_meta.update_path,
                         (unsigned long)offset, (unsigned)HTTPS_DL_CHUNK_SIZE,
                         (unsigned)(s_meta.region_id  % 1000u),
                         (unsigned)(s_meta.station_id % 1000u));

                uint16_t   chunk_len = 0u;
                SslDlResult_t res    = SSL_DL_ERR_READ;
                for (uint8_t retry = 0u; retry < MAX_CHUNK_RETRIES; retry++) {
                    wdt_kick(s_wdt_id);
                    res = ssl_downloader_get_chunk(s_url_buf, s_io_buf,
                                                   HTTPS_DL_CHUNK_SIZE, &chunk_len);
                    if (res == SSL_DL_OK) break;
                }

                if (res != SSL_DL_OK || chunk_len == 0u) {
                    download_ok = false;
                    break;
                }

                sha256_update(&s_sha_ctx, s_io_buf, (uint32_t)chunk_len);

                bool wrote = false;
                if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(1000u)) == pdTRUE) {
                    wrote = (oiw_write_chunk(next_chunk, s_io_buf, chunk_len) == OIW_OK);
                    xSemaphoreGive(g_fram_spi_mutex);
                }
                if (!wrote) { download_ok = false; break; }
            }

            (void)ssl_downloader_stop();
            s_state = download_ok ? OTA_STATE_DOWNLOAD_COMPLETE : OTA_STATE_IDLE;
            break;
        }

        /* ── DOWNLOAD_COMPLETE: verify SHA-256 against server digest ────── */
        case OTA_STATE_DOWNLOAD_COMPLETE: {
            uint8_t digest[32];
            sha256_final(&s_sha_ctx, digest);

            if (memcmp(digest, s_ocb.image_sha256, 32u) == 0) {
                s_state = OTA_STATE_VERIFIED;
            } else {
                if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(1000u)) == pdTRUE) {
                    (void)ocb_clear();
                    xSemaphoreGive(g_fram_spi_mutex);
                }
                s_state = OTA_STATE_IDLE;
            }
            wdt_kick(s_wdt_id);
            break;
        }

        /* ── VERIFIED: write OCB with ota_pending=1, then schedule reboot ─ */
        case OTA_STATE_VERIFIED: {
            RTC_DateTime_t dt;
            s_ocb.download_timestamp = datetime_get_datetime_from_rtc(&dt)
                                       ? get_epoch_from_datetime(&dt)
                                       : 0u;
            s_ocb.ota_pending = OCB_PENDING_FLAG;

            OtaStatus_t st = OTA_ERR_INVALID;
            if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(1000u)) == pdTRUE) {
                st = ocb_write(&s_ocb);
                xSemaphoreGive(g_fram_spi_mutex);
            }

            s_state = (st == OTA_OK) ? OTA_STATE_REBOOT_PENDING : OTA_STATE_IDLE;
            wdt_kick(s_wdt_id);
            break;
        }

        /* ── REBOOT_PENDING: brief settle delay then reset into bootloader ─ */
        case OTA_STATE_REBOOT_PENDING:
            vTaskDelay(pdMS_TO_TICKS(500u));
            HAL_NVIC_SystemReset();
            break;

        /* ── CONFIRMING: wait 60 s (or early signal), then write confirmed ─ */
        case OTA_STATE_CONFIRMING:
            for (uint32_t i = 0u; i < CONFIRM_TICKS; i++) {
                wdt_kick(s_wdt_id);
                vTaskDelay(pdMS_TO_TICKS(WDT_PERIOD_MS));
                if (s_confirm_req) break;
            }
            s_ocb.ota_confirmed = OCB_CONFIRMED_FLAG;
            if (xSemaphoreTake(g_fram_spi_mutex, pdMS_TO_TICKS(1000u)) == pdTRUE) {
                (void)ocb_write(&s_ocb);
                xSemaphoreGive(g_fram_spi_mutex);
            }
            s_confirm_req = false;
            wdt_kick(s_wdt_id);
            s_state = OTA_STATE_IDLE;
            break;

        default:
            s_state = OTA_STATE_IDLE;
            break;
        }
    }
}
