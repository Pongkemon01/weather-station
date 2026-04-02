#include <string.h>
#include "nv_database.h"

#define VALIDATION_VALUE '\xA1'
#define OPERATION_ADDR 0
#define METADATA_ADDR sizeof(Operation_Data_t)

/* Default meta data */
static const Meta_Data_t initial_meta = {
    .validation_value = VALIDATION_VALUE,
    .region_id = 0,
    .station_id = 0,
    .sampling_interval = 15,
    .temperature_adj = 0.0f,
    .humidity_adj = 0.0f,
    .pressure_adj = 0.0f,
    .light_adj = 0,
    .rainfall_adj = 0.0f,
    .server_url = "\0"
};

/* Global variables */
static Operation_Data_t oper;
static Meta_Data_t meta;

/* Local API */
static bool DB_SaveOper(void)
{
    if(fram_write_special_sector(OPERATION_ADDR, (uint8_t *)&oper, sizeof(oper)) == sizeof(oper))
        return(true);
    else
        return(false);
}

/* API Implementaion */
/* ---------------------------------------------------------------- */
bool DB_Init(SPI_HandleTypeDef *hframspi)
{
    /* Initialize FRAM */
    if(!(fram_init(hframspi)))
        return(false);
    if(!(fram_write_enable()))
        return(false);

    /* Read meta data */
    if(fram_read_special_sector(OPERATION_ADDR, (uint8_t *)&oper, sizeof(oper)) != sizeof(oper))
        return(false);
    if(fram_read_special_sector(METADATA_ADDR, (uint8_t *)&meta, sizeof(meta)) != sizeof(meta))
        return(false);
    if(meta.validation_value != VALIDATION_VALUE)
    {
        /* Meta data is unusable. Then re-initialize it */
        oper.db_head = 0;
        oper.upload_tail = 0;
        oper.sd_tail = 0;
        oper.total_data = 0;
        (void)DB_SaveOper();        // Save and discard error

        memcpy(&meta, &initial_meta, sizeof(meta));
        return(DB_SaveMeta());
    }
    return(true);
}

/* ---------------------------------------------------------------- */
bool DB_AddData(Weather_Data_Packed_t *data)
{
    uint32_t addr;

    addr = (uint32_t)oper.db_head * sizeof(Weather_Data_Packed_t);
    if(fram_write(addr, (uint8_t *)data, sizeof(Weather_Data_Packed_t)) != sizeof(Weather_Data_Packed_t))
        return(false);

    /* Update meta data */
    oper.db_head = (oper.db_head + 1) & 0x7FFF;
    if(oper.total_data < 32768)
        oper.total_data++;
    if(oper.db_head == oper.upload_tail)        // db_head hits upload_tail (discard the clash record)
        oper.upload_tail = (oper.upload_tail + 1) & 0x7FFF;
    if(oper.db_head == oper.sd_tail)            // db_head hits sd_tail (discard the clash record
        oper.sd_tail = (oper.sd_tail + 1) & 0x7FFF;

    return(DB_SaveOper());
}

/* ---------------------------------------------------------------- */
bool DB_GetData(uint16_t index, Weather_Data_Packed_t *data)
{
    uint32_t addr;

    if(index > 32767)
        return(false);

    addr = (uint32_t)index * sizeof(Weather_Data_Packed_t);

    if(fram_read(addr, (uint8_t *)data, sizeof(Weather_Data_Packed_t)) == sizeof(Weather_Data_Packed_t))
        return(true);
    else
        return(false);
}

/* ---------------------------------------------------------------- */
bool DB_GetRelData(uint16_t base, uint16_t index, Weather_Data_Packed_t *data)
{
    uint16_t abs_index;

    if(index > 32767)
        return(false);

    abs_index = (base + index) & 0x7FFF;

    return(DB_GetData(abs_index, data));
}

/* ---------------------------------------------------------------- */
bool DB_Flush(void)
{
    oper.db_head = 0;
    oper.upload_tail = 0;
    oper.sd_tail = 0;
    oper.total_data = 0;
    return(DB_SaveOper());
}

/* ---------------------------------------------------------------- */
bool DB_ToUploadwithOffset(uint16_t offset, Weather_Data_Packed_t *data)
{
    if(oper.db_head <= ((oper.upload_tail + offset) & 0x7FFF))
        return(false);
    return(DB_GetRelData(oper.upload_tail, offset, data));
}

/* ---------------------------------------------------------------- */
bool DB_ToSDwithOffset(uint16_t offset, Weather_Data_Packed_t *data)
{
    if(oper.db_head <= ((oper.sd_tail + offset) & 0x7FFF))
        return(false);
    return(DB_GetRelData(oper.sd_tail, offset, data));
}

/* ---------------------------------------------------------------- */
bool DB_IncSDTail(uint16_t step)
{
    if(oper.db_head == oper.sd_tail)
        return(false);
    if(((oper.sd_tail + step) & 0x7FFF) > oper.db_head)
        step = oper.db_head - oper.sd_tail;
    oper.sd_tail = (oper.sd_tail + step) & 0x7FFF;
    return(DB_SaveOper());
}

/* ---------------------------------------------------------------- */
bool DB_IncUploadTail(uint16_t step)
{
    if(oper.db_head == oper.upload_tail)
        return(false);
    if(((oper.upload_tail + step) & 0x7FFF) > oper.db_head)
        step = oper.db_head - oper.upload_tail;
    oper.upload_tail = (oper.upload_tail + step) & 0x7FFF;
    return(DB_SaveOper());
}

/* ---------------------------------------------------------------- */
Meta_Data_t *DB_GetMeta(void)
{
    return &meta;
}

/* ---------------------------------------------------------------- */
bool DB_SaveMeta(void)
{
    if(fram_write_special_sector(METADATA_ADDR, (uint8_t *)&meta, sizeof(meta)) == sizeof(meta))
        return(true);
    else
        return(false);
}

/* ---------------------------------------------------------------- */
uint16_t DB_GetTotalData(void)
{
    return oper.total_data;
}

/* ---------------------------------------------------------------- */
uint16_t DB_GetTotalToUpload(void)
{
    return((oper.db_head - oper.upload_tail) & 0x7FFF);
}

/* ---------------------------------------------------------------- */
uint16_t DB_GetTotalToSD(void)
{
    return((oper.db_head - oper.sd_tail) & 0x7FFF);
}

/* ---------------------------------------------------------------- */
uint16_t DB_GetHead(void)
{
    return oper.db_head;
}

/* ---------------------------------------------------------------- */
uint16_t DB_GetUploadTail(void)
{
    return oper.upload_tail;
}

/* ---------------------------------------------------------------- */
uint16_t DB_GetSDTail(void)
{
    return oper.sd_tail;
}
