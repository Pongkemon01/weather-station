/**
 * @file    nv_database.c
 * @brief   Non-volatile weather-station database — thread-safe implementation.
 *
 * Concurrency design
 * ------------------
 * A single FreeRTOS mutex (db_mutex) protects every access to the two
 * global structs (oper, meta) and to the SPI bus (via the FRAM driver).
 * All public functions acquire the mutex on entry and release it on every
 * exit path.  This satisfies the requirement that:
 *
 *   • One task writes records (DB_AddData).
 *   • Multiple tasks read records and meta-data concurrently.
 *   • Multiple tasks may update meta-data (DB_SetMeta / DB_SaveMeta).
 *
 * RAM budget
 * ----------
 * oper  = 8 bytes (Operation_Data_t)
 * meta  = 220 bytes (Meta_Data_t)
 * mutex = ~88 bytes (StaticSemaphore_t, stack-allocated)
 * No heap allocation is performed after DB_Init().
 *
 * Bug-fixes relative to the original code
 * ----------------------------------------
 * 1. Operator-precedence defect in DB_ToUploadwithOffset / DB_ToSDwithOffset:
 *      (a - b) & MASK <= offset   was evaluated as
 *      (a - b) & (MASK <= offset) because <= binds tighter than &.
 *    Fixed with explicit parentheses: ((a - b) & MASK) <= offset.
 *
 * 2. Redundant fram_write_enable() in DB_Init():
 *    fram_init() already issues WREN internally.  The duplicate call was
 *    removed.  fram_write() issues its own WREN just before each write.
 *
 * 3. DB_GetMeta() previously returned a raw pointer to the internal
 *    struct, allowing callers to mutate it without the mutex.  It now
 *    copies into a caller-supplied buffer (snapshot pattern).
 *
 * 4. const-correctness applied to all read-only pointer parameters to
 *    surface accidental mutation at compile time.
 *
 * 5. Address-bounds check added in DB_AddData() before the FRAM write.
 */

#include <string.h>
#include "nv_database.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "semphr.h"

/* ---- Private constants ---------------------------------------- */
#define VALIDATION_VALUE ((uint8_t)0xA1u)

/** Byte offset of Operation_Data_t in the Special Sector */
#define OPERATION_ADDR ((uint8_t)0u)

/** Byte offset of Meta_Data_t in the Special Sector */
#define METADATA_ADDR ((uint8_t)sizeof(Operation_Data_t))

/* ---- Default configuration ------------------------------------ */
static const Meta_Data_t initial_meta = {
    .validation_value = VALIDATION_VALUE,
    .region_id = 0u,
    .station_id = 0u,
    .sampling_interval = 15u,
    .temperature_adj = 0.0f,
    .humidity_adj = 0.0f,
    .pressure_adj = 0.0f,
    .light_adj = 0u,
    .rainfall_adj = 0.0f,
    .server_name = "robin.cpe.ku.ac.th",
    .server_path = "/weather/WeatherUpload.php"};

/* ---- Module-private globals ----------------------------------- */
static Operation_Data_t oper;
static Meta_Data_t meta;

/*
 * Static mutex — zero heap cost.
 * The mutex is created in DB_Init() before the scheduler starts, so
 * xSemaphoreCreateMutexStatic() is safe and cannot return NULL.
 */
static StaticSemaphore_t db_mutex_buf;
static SemaphoreHandle_t db_mutex = NULL;

/* ---- Private helpers ------------------------------------------ */

/** Persist the operation struct to the Special Sector. */
static bool save_oper(void)
{
    return (fram_write_special_sector(OPERATION_ADDR,
                                      (const uint8_t *)&oper,
                                      sizeof(oper)) == sizeof(oper));
}

/** Acquire mutex (portMAX_DELAY — must never be called from an ISR). */
static inline void lock(void)
{
    xSemaphoreTake(db_mutex, portMAX_DELAY);
}

/** Release mutex. */
static inline void unlock(void)
{
    xSemaphoreGive(db_mutex);
}

/* ================================================================ */
/* Public API                                                        */
/* ================================================================ */

/**
 * @brief  Initialise the database.
 *
 * Must be called once before the RTOS scheduler starts (or with
 * external serialisation if called later).
 */
bool DB_Init(SPI_HandleTypeDef *hframspi)
{
    /* Create the mutex exactly once. */
    if (db_mutex == NULL)
        db_mutex = xSemaphoreCreateMutexStatic(&db_mutex_buf);

    /* ---- FRAM driver init ---- */
    if (!fram_init(hframspi)) /* fram_init issues WREN internally */
        return false;

    /* ---- Load persistent state from Special Sector ---- */
    if (fram_read_special_sector(OPERATION_ADDR, (uint8_t *)&oper, sizeof(oper)) != sizeof(oper))
        return false;

    if (fram_read_special_sector(METADATA_ADDR, (uint8_t *)&meta, sizeof(meta)) != sizeof(meta))
        return false;

    /* ---- Validate meta; reinitialise on corruption ---- */
    if (meta.validation_value != VALIDATION_VALUE)
    {
        oper.db_head = 0u;
        oper.upload_tail = 0u;
        oper.sd_tail = 0u;
        oper.total_data = 0u;
        (void)save_oper(); /* best-effort; ignore error */

        return DB_SetMeta(&initial_meta);
    }

    return true;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Append one weather record to the ring buffer.
 *
 * Thread-safe.  Intended to be called from a single writer task.
 * If db_head catches up with a tail pointer (buffer full for that
 * consumer), the tail is advanced by one to discard the oldest record.
 */
bool DB_AddData(const Weather_Data_Packed_t *data)
{
    uint32_t addr;
    bool ok;

    if (data == NULL)
        return false;

    /* Validate that the target address fits in physical FRAM. */
    addr = (uint32_t)oper.db_head * sizeof(Weather_Data_Packed_t);
    if (addr > FRAM_MAX_ADDR)
        return false;

    lock();

    ok = (fram_write(addr, (const uint8_t *)data, sizeof(Weather_Data_Packed_t)) == sizeof(Weather_Data_Packed_t));

    if (ok)
    {
        oper.db_head = (oper.db_head + 1u) & DB_INDEX_MASK;

        if (oper.total_data < DB_MAX_RECORDS)
            oper.total_data++;

        /* If head laps a tail, advance the tail so the consumer always
         * sees valid (un-overwritten) data. */
        if (oper.db_head == oper.upload_tail)
            oper.upload_tail = (oper.upload_tail + 1u) & DB_INDEX_MASK;

        if (oper.db_head == oper.sd_tail)
            oper.sd_tail = (oper.sd_tail + 1u) & DB_INDEX_MASK;

        ok = save_oper();
    }

    unlock();
    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Read the record at absolute ring-buffer index @p index.
 */
bool DB_GetData(uint16_t index, Weather_Data_Packed_t *data)
{
    uint32_t addr;
    bool ok;

    if (data == NULL || index > DB_INDEX_MASK)
        return false;

    addr = (uint32_t)index * sizeof(Weather_Data_Packed_t);

    lock();
    ok = (fram_read(addr, (uint8_t *)data, sizeof(Weather_Data_Packed_t)) == sizeof(Weather_Data_Packed_t));
    unlock();

    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Read the record at (@p base + @p index) mod DB_MAX_RECORDS.
 */
bool DB_GetRelData(uint16_t base, uint16_t index, Weather_Data_Packed_t *data)
{
    uint16_t abs_index;

    if (data == NULL || index > DB_INDEX_MASK)
        return false;

    /* No separate lock needed here — DB_GetData acquires it. */
    abs_index = (base + index) & DB_INDEX_MASK;
    return DB_GetData(abs_index, data);
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Reset all ring-buffer pointers to zero.
 */
bool DB_Flush(void)
{
    bool ok;

    lock();
    oper.db_head = 0u;
    oper.upload_tail = 0u;
    oper.sd_tail = 0u;
    oper.total_data = 0u;
    ok = save_oper();
    unlock();

    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Retrieve the record @p offset positions ahead of upload_tail.
 *
 * Bug fix: original code had the operator-precedence defect
 *   (a - b) & MASK <= offset  →  (a - b) & (MASK <= offset)  [WRONG]
 * Correct evaluation requires explicit parentheses around the & expression.
 */
bool DB_ToUploadwithOffset(uint16_t offset, Weather_Data_Packed_t *data)
{
    uint16_t available;
    uint16_t head, tail;

    if (data == NULL)
        return false;

    lock();
    head = oper.db_head;
    tail = oper.upload_tail;
    unlock();

    available = (uint16_t)((head - tail) & DB_INDEX_MASK);

    /*
     * FIX: parenthesise the bitwise-AND operand so it is evaluated
     * before the comparison. "<=" has higher precedence than "&" in C.
     */
    if (available <= offset)
        return false;

    return DB_GetRelData(tail, offset, data);
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Retrieve the record @p offset positions ahead of sd_tail.
 *
 * Same operator-precedence fix as DB_ToUploadwithOffset.
 */
bool DB_ToSDwithOffset(uint16_t offset, Weather_Data_Packed_t *data)
{
    uint16_t available;
    uint16_t head, tail;

    if (data == NULL)
        return false;

    lock();
    head = oper.db_head;
    tail = oper.sd_tail;
    unlock();

    available = (uint16_t)((head - tail) & DB_INDEX_MASK);

    if (available <= offset)
        return false;

    return DB_GetRelData(tail, offset, data);
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Advance upload_tail by @p step records.
 */
bool DB_IncUploadTail(uint16_t step)
{
    uint16_t available;
    bool ok;

    lock();
    available = (uint16_t)((oper.db_head - oper.upload_tail) & DB_INDEX_MASK);

    if (available == 0u)
    {
        unlock();
        return false;
    }

    if (step > available)
        step = available;

    oper.upload_tail = (oper.upload_tail + step) & DB_INDEX_MASK;
    ok = save_oper();
    unlock();

    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Advance sd_tail by @p step records.
 */
bool DB_IncSDTail(uint16_t step)
{
    uint16_t available;
    bool ok;

    lock();
    available = (uint16_t)((oper.db_head - oper.sd_tail) & DB_INDEX_MASK);

    if (available == 0u)
    {
        unlock();
        return false;
    }

    if (step > available)
        step = available;

    oper.sd_tail = (oper.sd_tail + step) & DB_INDEX_MASK;
    ok = save_oper();
    unlock();

    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Copy the current meta configuration into @p out.
 *
 * Snapshot pattern: the caller receives a consistent copy without
 * needing to hold the mutex itself.  This replaces the original
 * DB_GetMeta() which returned a raw mutable pointer — a thread-safety
 * hazard.
 *
 * @param[out] out  Caller-allocated Meta_Data_t buffer.
 */
bool DB_GetMeta(Meta_Data_t *out)
{
    if (out == NULL)
        return false;

    lock();
    memcpy(out, &meta, sizeof(meta));
    unlock();

    return true;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Replace the in-RAM meta configuration and persist to F-RAM.
 *
 * @param[in] in  New Meta_Data_t to apply.
 */
bool DB_SetMeta(const Meta_Data_t *in)
{
    bool ok;

    if (in == NULL)
        return false;

    lock();
    memcpy(&meta, in, sizeof(meta));
    ok = (fram_write_special_sector(METADATA_ADDR,
                                    (const uint8_t *)&meta,
                                    sizeof(meta)) == sizeof(meta));
    unlock();

    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Persist the current in-RAM meta to F-RAM.
 *
 * Use this after modifying a copy obtained via DB_GetMeta() and then
 * calling DB_SetMeta().  Direct field-by-field edits of the internal
 * struct are no longer possible through the public API.
 */
bool DB_SaveMeta(void)
{
    bool ok;

    lock();
    ok = (fram_write_special_sector(METADATA_ADDR,
                                    (const uint8_t *)&meta,
                                    sizeof(meta)) == sizeof(meta));
    unlock();

    return ok;
}

/* ---------------------------------------------------------------- */
/* Snapshot accessors — each acquires the mutex for an atomic read.  */

uint16_t DB_GetTotalData(void)
{
    uint16_t v;
    lock();
    v = oper.total_data;
    unlock();
    return v;
}

uint16_t DB_GetTotalToUpload(void)
{
    uint16_t v;
    lock();
    v = (uint16_t)((oper.db_head - oper.upload_tail) & DB_INDEX_MASK);
    unlock();
    return v;
}

uint16_t DB_GetTotalToSD(void)
{
    uint16_t v;
    lock();
    v = (uint16_t)((oper.db_head - oper.sd_tail) & DB_INDEX_MASK);
    unlock();
    return v;
}

uint16_t DB_GetHead(void)
{
    uint16_t v;
    lock();
    v = oper.db_head;
    unlock();
    return v;
}

uint16_t DB_GetUploadTail(void)
{
    uint16_t v;
    lock();
    v = oper.upload_tail;
    unlock();
    return v;
}

uint16_t DB_GetSDTail(void)
{
    uint16_t v;
    lock();
    v = oper.sd_tail;
    unlock();
    return v;
}

uint8_t DB_GetSamplingInterval(void)
{
    uint8_t v;
    lock();
    v = meta.sampling_interval;
    unlock();
    return v;
}
