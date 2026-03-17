#ifndef __NV_DATABASE_H
#define __NV_DATABASE_H

#include <stdint.h>
#include <stdbool.h>
#include "cy15b116qn.h"
#include "weather_data.h"

/* Data that store in special sector (256 bytes max.) */
typedef struct {
    /* Operation (The order should be fixed. Additional parameters should add after these section)*/
    uint16_t    total_data;             // Total valid recorded data in FRAM (32768 MAX)
    uint16_t    db_head;                // point to the slot that is considered empty
    uint16_t    upload_tail;            // point to the first slot that need to upload to server
    uint16_t    sd_tail;                // point to the first slot that need to save to SD card
}Opeeration_Data_t;

typedef struct {
    uint8_t     validation_value;       // Indicate whether these configurations are valid

    /* Station ID */
    uint16_t    region_id;
    uint16_t    station_id;

    /* Configuration */
    uint8_t     sampling_interval;      // in minutes (1 - 60)
    float       temperature_adj;        // Manual calibration value for each sensors
    float       humidity_adj;
    float       pressure_adj;
    uint16_t    light_adj;
    float       rainfall_adj;
    char        server_url[128];        // URL for internet server to POST weather data (127 chars max.)
}Meta_Data_t;

// Verify whether the size of Meta_Data_t is not too large
#if ((sizeof(Meta_Data_t) + sizeof(Opeeration_Data_t)) > 256)
#error "Meta_Data_t is too large"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* API */
bool DB_Init(SPI_HandleTypeDef *hframspi);
bool DB_AddData(Weather_Data_Packed_t *data);
bool DB_GetData(uint16_t index, Weather_Data_Packed_t *data);
bool DB_GetRelData(uint16_t base, uint16_t index, Weather_Data_Packed_t *data);
bool DB_Flush(void);
bool DB_ToUploadwithOffset(uint16_t offset, Weather_Data_Packed_t *data);
bool DB_ToSDwithOffset(uint16_t offset, Weather_Data_Packed_t *data);
bool DB_IncSDTail(uint16_t step);
bool DB_IncUploadTail(uint16_t step);
Meta_Data_t *DB_GetMeta(void);
bool DB_SaveMeta(void);
uint16_t DB_GetTotalData(void);
uint16_t DB_GetTotalToUpload(void);
uint16_t DB_GetTotalToSD(void);
uint16_t DB_GetHead(void);
uint16_t DB_GetUploadTail(void);
uint16_t DB_GetSDTail(void);

#ifdef __cplusplus
}
#endif

#endif  // __NV_DATABASE_H