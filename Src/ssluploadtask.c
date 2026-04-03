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
#include "a7670_ssl_uploader.h"

#include <string.h>

#define MAX_RECORDS_PER_UPLOAD 12u // Limit the number of records to upload in one batch to avoid long blocking time

static Meta_Data_t meta_data;

/*---------------------------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------------------------*/
/*
 * Database read — called by ssl_uploader once per SSL_FETCH_WINDOW slice.
 *
 * @param ctx     Opaque pointer; cast to whatever your SPI driver expects.
 * @param offset  Absolute byte offset within the file stored in flash.
 * @param buf     Destination buffer (SSL_FETCH_WINDOW bytes available).
 * @param len     Exact number of bytes to read (≤ SSL_FETCH_WINDOW).
 * @return true on success; false on SPI error or out-of-range offset.
 */
static uint16_t chunk_read(void *ctx, uint32_t offset,
                           uint8_t *buf, uint16_t len)
{
    uint16_t max_bytes_to_read = len - (2 * sizeof(uint16_t)) - sizeof(uint8_t); /* Reserve space for region_id and station_id at the beginning of the buffer */
    uint8_t *pos;
    uint16_t i;
    uint8_t *count = (uint8_t *)ctx; /* Cast context to a pointer to uint8_t to store the count of records read */

    /* Build data field first */
    pos = buf + (2 * sizeof(uint16_t)) + sizeof(uint8_t); /* Reserve space for region_id and station_id at the beginning of the buffer */

    // Add records to upload batch
    *count = 0; /* Initialize count of records read */
    for (i = 0; i < max_bytes_to_read; i += sizeof(Weather_Data_Packed_t))
    {
        if ((i + sizeof(Weather_Data_Packed_t)) > max_bytes_to_read)
            break; /* Not enough space for another record, stop adding more records to this batch */
        if (!DB_ToUploadwithOffset(i, (Weather_Data_Packed_t *)pos))
            break; /* Failed to read data from FRAM, stop adding more records to this batch */

        pos += sizeof(Weather_Data_Packed_t);
        (*count)++; /* Increment count of records read */
    }

    if (*count > 0)
    {
        /* Add header for upload batch at the beginning of the buffer */
        pos = buf;
        i += (2 * sizeof(uint16_t)); /* Add header size to the total bytes read */

        // Create header for upload batch
        memcpy(pos, &(meta_data.region_id), sizeof(uint16_t));
        pos += sizeof(uint16_t);
        memcpy(pos, &(meta_data.station_id), sizeof(uint16_t));
        pos += sizeof(uint16_t);
        *pos = *count; /* Store the count of records in the header */
    }

    return i;
}

/*---------------------------------------------------------------------------------------------*/
void ssluploadtask(void *params)
{
    (void)params;
    static const uint32_t max_chunk_size = (sizeof(Weather_Data_Packed_t) * MAX_RECORDS_PER_UPLOAD) + (2 * sizeof(uint16_t)) + sizeof(uint8_t); /* Max chunk size based on the number of records and header size */
    uint16_t total_to_upload;
    uint8_t record_count;

    SslUploadSession_t session; /* always start from the beginning */
    SslUploadResult_t result;

    SslUploadParams_t ssl_params = {
        .host = meta_data.server_name, /* Assuming server_name is null-terminated and fits within SSL_HOST_MAX_LEN */
        .path = meta_data.server_path, /* Assuming server_path is null-terminated and fits within SSL_PATH_MAX_LEN */
        .fetch_cb = chunk_read,
        .fetch_ctx = (void *)&record_count, /* Pass record_count as context to the fetch callback if needed */
        .urc_queue = NULL,                  /* This should be set to the actual URC queue used by the AT channel */
        .file_size = max_chunk_size,        /* Chunk size */
        .chunk_size = 0u,                   /* 0 → uses SSL_CHUNK_SIZE_DEFAULT (4096 B) but capped with file_size */
        .port = 443u,
        .max_retries = 0u, /* 0 → uses SSL_MAX_RETRIES default (3) */
        .use_range_on_retry = false,
    };

    while (1)
    {
        /* Wait for notification from main task to perform upload */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Check if Modem is ready */
        if (!system_ready_status.a7670_ready)
            continue; /* Modem is not ready, skip this iteration */

        /* Perform SSL upload */
        total_to_upload = DB_GetTotalToUpload();
        (void)DB_GetMeta(&meta_data);
        ssl_params.urc_queue = modem_get_urc_queue(); /* Get the URC queue from the modem to pass to the upload parameters */

        while (total_to_upload > 0)
        {
            /* Update chunk size */
            if (total_to_upload < MAX_RECORDS_PER_UPLOAD)
                ssl_params.file_size = (total_to_upload * sizeof(Weather_Data_Packed_t)) + (2 * sizeof(uint16_t)) + sizeof(uint8_t);
            else
                ssl_params.file_size = max_chunk_size;

            /* Initialize variables */
            record_count = 0;                     /* Reset record count before each upload attempt */
            memset(&session, 0, sizeof(session)); /* Initialize session */

            result = ssl_upload_chunked(&ssl_params, &session);

            if (result == SSL_UPLOAD_OK)
            {
                /* Update total_to_upload based on the number of records successfully uploaded */
                total_to_upload -= (uint16_t)record_count;      /* Subtract the number of records uploaded in this chunk from the total */
                (void)DB_IncUploadTail((uint16_t)record_count); /* Move the upload tail forward by the number of records uploaded */
            }
            else
            {
                /* Handle upload failure (e.g., log error, retry logic, etc.) */
                break; /* Exit the loop on failure, or implement retry logic as needed */
            }
        }
    }
}
