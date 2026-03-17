#ifndef __WEATHER_DATA_H

#define __WEATHER_DATA_H

// Configuration for fixed-point data
#define FIXEDPT_BITS 16
#define FIXEDPT_WBITS 9 // Total whole bits including sign bit

#include "fixedptc.h"
#include "y2k_time.h"

typedef struct __attribute__((packed))
{
    uint32_t time_stamp; // Epoch from 1-Jan-2000 00:00.00
    fixedpt temperature; // Temperature in C (2-digit decimal points)
    fixedpt humidity;    // Humidity in %Rh (2-digit decimal points)
    fixedpt pressure;    // Pressure in kPa (2-digit decimal points)
    uint16_t light_par;  // Light intensity in umol/s*m^2 (0 - 2500 integer)
    fixedpt rainfall;    // Rain fall cummulative value in mm/hr (2-digit decimal point)
    fixedpt dew_point;   // Dew-point temperature at the measurement time
    fixedpt bus_value;   // Blast Unit of Severity (2-dight decimal point)
} Weather_Data_Packed_t;

typedef struct
{
    RTC_DateTime_t sampletime; // Time stamp at the measurement point
    float temperature;         // Temperature in C
    float humidity;            // Humidity in %Rh
    float pressure;            // Pressure in kPa
    uint16_t light_par;        // Light intensity in umol/s*m^2
    float rainfall;            // Rain fall cummulative value in mm/hr
    float dew_point;           // Dew-point temperature at the measurement time
    float bus_value;           // Blast Unit of Severity (Re-compute at midnight)
} Weather_Data_t;

#ifdef __cplusplus
extern "C"
{
#endif
    /* API */
    void PackData(Weather_Data_t *data, Weather_Data_Packed_t *packed);
    void UnpackData(Weather_Data_Packed_t *packed, Weather_Data_t *data);

#ifdef __cplusplus
}
#endif

#endif //__WEATHER_DATA_H