#ifndef NV_DATABASE_H
#define NV_DATABASE_H

/**
 * @file    nv_database.h
 * @brief   Non-volatile weather-station database backed by CY15B116QN F-RAM.
 *
 * Thread-safety model:
 *   All public functions acquire an internal FreeRTOS mutex before
 *   touching shared state (oper / meta structs and the SPI bus).
 *   They are safe to call from any task context.  They must NOT be
 *   called from an ISR.
 *
 * Layout in the F-RAM Special Sector (256 bytes):
 *   Offset 0                         : Operation_Data_t  (8 bytes)
 *   Offset sizeof(Operation_Data_t)  : Meta_Data_t       (220 bytes)
 *   Total occupied                   : 228 bytes  (≤ 256 — enforced by static_assert)
 *
 * Layout in the F-RAM main array:
 *   Slot N starts at address N * sizeof(Weather_Data_Packed_t).
 *   Maximum 32768 slots, addressed by a 15-bit index (0x0000–0x7FFF).
 *   db_head always points to the NEXT empty slot (head-insert ring buffer).
 */

#include <stdint.h>
#include <stdbool.h>
#include "cy15b116qn.h"
#include "weather_data.h"

/* ---- Ring-buffer capacity ------------------------------------- */
#define DB_MAX_RECORDS 32768u /* Must be a power of two      */
#define DB_INDEX_MASK 0x7FFFu /* (DB_MAX_RECORDS - 1)        */

/* ---- Persistent data structures ------------------------------- */

/**
 * @brief  Operational counters kept in the Special Sector.
 *         The field ORDER must never change between firmware versions
 *         (any new fields must be appended).
 */
typedef struct __attribute__((packed))
{
    uint16_t total_data;  /**< Total valid records stored (max DB_MAX_RECORDS) */
    uint16_t db_head;     /**< Next-empty slot index (15-bit ring)              */
    uint16_t upload_tail; /**< First slot pending upload to server              */
    uint16_t sd_tail;     /**< First slot pending save to SD card               */
} Operation_Data_t;       /* 8 bytes */

/**
 * @brief  Station configuration kept in the Special Sector.
 */
typedef struct __attribute__((packed))
{
    uint8_t validation_value;  /**< Must equal VALIDATION_VALUE to be trusted */
    uint16_t region_id;        /**< User-defined region ID (0–999)            */
    uint16_t station_id;       /**< User-defined station ID (0–999)           */
    uint8_t sampling_interval; /**< Minutes between samples, 1–60             */
    float temperature_adj;     /**< Temperature adjustment factor (-999.99 - 999.99) */
    float humidity_adj;        /**< Humidity adjustment factor (-999.99 - 999.99)  */
    float pressure_adj;        /**< Pressure adjustment factor (-999.99 - 999.99)  */
    int16_t light_adj;         /**< Light adjustment factor (-9999 - 9999)   */
    float rainfall_adj;        /**< Rainfall adjustment factor (-999.99 - 999.99) */
    char server_name[64];      /**< Null-terminated, 63 chars max             */
    char server_path[64];      /**< Data upload path, null-terminated, 63 chars max  */
    char update_path[64];      /**< OTA base URL (UPDATE_PATH), null-terminated, 63 chars max */
} Meta_Data_t;

/* Compile-time guard: both structs must fit in the 256-byte special sector */
_Static_assert(sizeof(Operation_Data_t) + sizeof(Meta_Data_t) <= FRAM_SPECIAL_SECTOR_SIZE,
               "Operation_Data_t + Meta_Data_t exceed the 256-byte Special Sector");

/* ---- Public API ----------------------------------------------- */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief  Initialise the database.  Call once before the RTOS scheduler
     *         starts, or protect externally if called later.
     */
    bool DB_Init(SPI_HandleTypeDef *hframspi);

    /** @brief  Append one weather record.  Single-writer; thread-safe. */
    bool DB_AddData(const Weather_Data_Packed_t *data);

    /** @brief  Read a record by absolute ring-buffer index (0–32767). */
    bool DB_GetData(uint16_t index, Weather_Data_Packed_t *data);

    /**
     * @brief  Read a record at @p index steps ahead of @p base.
     *         Handles 15-bit wrap-around automatically.
     */
    bool DB_GetRelData(uint16_t base, uint16_t index, Weather_Data_Packed_t *data);

    /** @brief  Reset all pointers and counters to zero (erases logical DB). */
    bool DB_Flush(void);

    /**
     * @brief  Fetch the record at @p offset steps ahead of upload_tail.
     * @retval false when fewer than (offset+1) records are pending upload.
     */
    bool DB_ToUploadwithOffset(uint16_t offset, Weather_Data_Packed_t *data);

    /**
     * @brief  Fetch the record at @p offset steps ahead of sd_tail.
     * @retval false when fewer than (offset+1) records are pending SD save.
     */
    bool DB_ToSDwithOffset(uint16_t offset, Weather_Data_Packed_t *data);

    /** @brief  Advance upload_tail by @p step (clamped to available records). */
    bool DB_IncUploadTail(uint16_t step);

    /** @brief  Advance sd_tail by @p step (clamped to available records). */
    bool DB_IncSDTail(uint16_t step);

    /**
     * @brief  Copy the current configuration into the caller-supplied buffer.
     *
     * Preferred over returning a raw pointer so that callers always see a
     * consistent snapshot without needing to hold the mutex themselves.
     *
     * @param[out] out  Caller-allocated Meta_Data_t to receive the copy.
     * @retval true always (provided @p out is non-NULL).
     */
    bool DB_GetMeta(Meta_Data_t *out);

    /**
     * @brief  Overwrite the in-RAM meta struct and persist it to F-RAM.
     *
     * @param[in] in  New configuration to apply.
     * @retval true if the F-RAM write succeeded.
     */
    bool DB_SetMeta(const Meta_Data_t *in);

    /** @brief  Persist the current in-RAM meta to F-RAM (after direct field edits). */
    bool DB_SaveMeta(void);

    /* ---- Snapshot accessors (all return consistent values) -------- */
    uint16_t DB_GetTotalData(void);
    uint16_t DB_GetTotalToUpload(void);
    uint16_t DB_GetTotalToSD(void);
    uint16_t DB_GetHead(void);
    uint16_t DB_GetUploadTail(void);
    uint16_t DB_GetSDTail(void);
    uint8_t DB_GetSamplingInterval(void);

#ifdef __cplusplus
}
#endif

#endif /* NV_DATABASE_H */
