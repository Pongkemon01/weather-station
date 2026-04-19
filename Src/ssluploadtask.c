#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "weather_data.h"
#include "ui.h"
#include "nv_database.h"
#include "uart_subsystem.h"
#include "a7670.h"
#include "a7670_at_channel.h"
#include "a7670_https_uploader.h"
#include "ota_manager_task.h"
#include "watchdog_task.h"

#include <stdio.h>
#include <string.h>

/* Maximum records packed into one 512-byte POST blob.
 * Blob layout: header(5B) + N × sizeof(Weather_Data_Packed_t)(18B).
 * (512 - 5) / 18 = 28 records exactly → 28×18+5 = 509 B ≤ 512 B. */
#define MAX_RECORDS_PER_UPLOAD  28u
#define UPLOAD_HEADER_SIZE      (2u * sizeof(uint16_t) + sizeof(uint8_t))  /* 5 B */
#define UPLOAD_BLOB_SIZE(n)     ((uint16_t)(UPLOAD_HEADER_SIZE + (n) * sizeof(Weather_Data_Packed_t)))

static Meta_Data_t  meta_data;
static char         s_url_buf[128];

/*---------------------------------------------------------------------------------------------*/

/*
 * Context passed to chunk_fill().
 * records_to_fetch:  set by the upload loop before each call.
 * records_fetched:   filled in by chunk_fill(); read back after the POST.
 */
typedef struct {
    uint8_t records_to_fetch;
    uint8_t records_fetched;
} FetchCtx_t;

/*
 * chunk_fill — HttpsUlFetchCb_t implementation.
 *
 * Assembles one upload blob into buf[]:
 *   [region_id 2B][station_id 2B][count 1B][record 0..18B]...[record N-1..18B]
 *
 * Reads exactly fc->records_to_fetch records starting at upload_tail+0.
 * DB_ToUploadwithOffset(n, ...) returns the record n steps ahead of
 * upload_tail; advance happens only after a successful POST via DB_IncUploadTail().
 */
static uint16_t chunk_fill(void *ctx, uint8_t *buf, uint16_t max_len)
{
    FetchCtx_t *fc  = (FetchCtx_t *)ctx;
    uint8_t    *pos = buf + UPLOAD_HEADER_SIZE;
    uint8_t     n;

    fc->records_fetched = 0u;

    for (n = 0u; n < fc->records_to_fetch; n++)
    {
        if ((uint16_t)(pos - buf) + (uint16_t)sizeof(Weather_Data_Packed_t) > max_len)
            break;
        if (!DB_ToUploadwithOffset((uint16_t)n, (Weather_Data_Packed_t *)pos))
            break;
        pos += sizeof(Weather_Data_Packed_t);
        fc->records_fetched++;
    }

    if (fc->records_fetched == 0u)
        return 0u;

    /* Write 5-byte header */
    memcpy(buf,                          &meta_data.region_id,  sizeof(uint16_t));
    memcpy(buf + sizeof(uint16_t),       &meta_data.station_id, sizeof(uint16_t));
    buf[2u * sizeof(uint16_t)] = fc->records_fetched;

    return UPLOAD_BLOB_SIZE(fc->records_fetched);
}

/*---------------------------------------------------------------------------------------------*/

void ssluploadtask(void *params)
{
    (void)params;

    static int8_t    wdt_id;
    static FetchCtx_t fetch_ctx;

    wdt_id = wdt_register("sslupload");

    while (1)
    {
        /* Wait for upload notification; kick watchdog every 500 ms while idle. */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WDT_PERIOD_MS)) == 0u)
            wdt_kick(wdt_id);
        wdt_kick(wdt_id);

        if (!system_ready_status.a7670_ready)
            continue;

        uint16_t total_to_upload = DB_GetTotalToUpload();
        if (total_to_upload == 0u)
            continue;

        (void)DB_GetMeta(&meta_data);

        /* Build upload URL: "https://<server_name><server_path>" */
        int n = snprintf(s_url_buf, sizeof(s_url_buf),
                         "https://%s%s",
                         meta_data.server_name,
                         meta_data.server_path);
        if (n <= 0 || n >= (int)sizeof(s_url_buf))
            continue;

        /* Start one HTTPS session for all batches in this upload cycle. */
        HttpsUlResult_t ul_result = https_uploader_start(modem_get_urc_queue());
        if (ul_result != HTTPS_UL_OK)
            continue;

        while (total_to_upload > 0u)
        {
            uint8_t records_this_batch =
                (total_to_upload < MAX_RECORDS_PER_UPLOAD)
                    ? (uint8_t)total_to_upload
                    : (uint8_t)MAX_RECORDS_PER_UPLOAD;

            fetch_ctx.records_to_fetch = records_this_batch;
            fetch_ctx.records_fetched  = 0u;

            ul_result = https_uploader_post(s_url_buf,
                                            chunk_fill,
                                            &fetch_ctx,
                                            UPLOAD_BLOB_SIZE(records_this_batch));
            wdt_kick(wdt_id);

            if (ul_result == HTTPS_UL_OK && fetch_ctx.records_fetched > 0u)
            {
                (void)DB_IncUploadTail((uint16_t)fetch_ctx.records_fetched);
                total_to_upload -= (uint16_t)fetch_ctx.records_fetched;
            }
            else
            {
                break;
            }
        }

        (void)https_uploader_stop();

        /* Notify OtaManagerTask to perform OTA version check in this modem session. */
        if (OtaManagerTaskHandle != NULL)
            xTaskNotify(OtaManagerTaskHandle, 0u, eNoAction);
    }
}
