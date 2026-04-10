/*
 * Peripheral allocation:
 * - USART1 : RS485 Modbus RTU (Light addr = 0x01, Rain addr = 0x02)
 * - USART2 : Console port
 * - USART3 : A7670E LTE Modem
 * - I2C1   : MCP23017 (UI Interface) (addr = 0x20)
 * - I2C2   : Sensor bus (BMP390(addr=0x76) and SHT45(addr=0x44))
 * - SPI1   : FRAM (2M x 8bit)
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include <math.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "fatfs.h"
#include "i2c.h"
#include "rtc.h"
#include "sdmmc.h"
#include "usb_otg.h"
#include "usart.h"
#include "spi.h"

#include "tusb.h"
#include "uart_subsystem.h"
#include "modbus.h"
#include "bmp390.h"
#include "sht45.h"
#include "a7670.h"
#include "ui.h"
#include "nv_database.h"
#include "weather_data.h"
#include "datetime.h"
#include "rain_light.h"
#include "weather_data.h"
#include "fixedptc.h"

#define SENSOR_ALPHA 0.5f
#define SENSOR_BETA (1.0f - SENSOR_ALPHA)

extern osThreadId_t MainTaskHandle;      // MainTaskHandle was casted from TaskHandle_t to osThreadId_t
extern osThreadId_t SslUploadTaskHandle; // SslUploadTaskHandle was casted from TaskHandle_t to osThreadId_t

/* Current weather data */
Meta_Data_t db_meta_data;
Weather_Data_t weather_data = {0};
float accum_rainfall = 0.0f;

/* ------------------------------------------------------------------------ */
static inline bool SaveRecordToSD(FIL *file, Weather_Data_Packed_t *data)
{
    /*
     * Line structure:
     * dd-mmm-yyyy hh:mm:ss,-ttt.tt,hhh.hh,ppp.pp,llll,rrr.rr,eee.ee,bb.bb
     * Total length = 67 characters + "\r\n" = 69 characters
     */
    const char *month_str[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static char line[80], *pos;
    static RTC_DateTime_t dt;
    unsigned int len, written;

    if (file == NULL || data == NULL)
        return false;

    get_datetime_from_epoch(data->time_stamp, &dt);

    pos = line;
    pos += sprintf(pos, "%02d-%s-20%02d %02d:%02d:%02d,", dt.day, month_str[dt.month - 1], dt.year, dt.hours, dt.minutes, dt.seconds);
    pos += fixedpt_str(data->temperature, pos, 2);
    *pos++ = ',';
    pos += fixedpt_str(data->humidity, pos, 2);
    *pos++ = ',';
    pos += fixedpt_str(data->pressure, pos, 2);
    *pos++ = ',';
    pos += sprintf(pos, "%u,", data->light_par);

    // CHeck pos is likely to overflow here if strlen(line) >= 60
    *pos = '\0'; // Temporary null-termination for strlen() check
    if (strlen(line) >= 60)
        return false;

    pos += fixedpt_str(data->rainfall, pos, 2);
    *pos++ = ',';
    pos += fixedpt_str(data->dew_point, pos, 2);
    *pos++ = ',';
    pos += fixedpt_str(data->bus_value, pos, 2);
    *pos++ = '\r';
    *pos++ = '\n';
    *pos = '\0';

    // Write the line to SD card
    len = strlen(line);
    if (f_write(file, line, len, &written) != FR_OK || written != len)
        return false;

    return true;
}

/* ------------------------------------------------------------------------ */
#if __STDC_VERSION__ >= 201112L || __cplusplus >= 201103L
#define U16_LITERAL(x) u##x
#else
// Fallback for older compilers - may not be strictly UTF-16
#define U16_LITERAL(x) L##x
#endif
static inline void SaveToSD(void)
{
    static const char *header = "Date Time,Temperature (C),Humidity (%Rh),Pressure (kPa),Light PAR (umol/s*m^2),Rainfall (mm/hr),Dew Point (C),BUS\r\n";
    static const TCHAR *filename = U16_LITERAL("weather_data.csv");
    FIL file;
    Weather_Data_Packed_t data;
    uint16_t total_to_sd, i;

    if (!(system_ready_status.sd_detected) || system_ready_status.sd_write_protected)
        return;
    if (f_open(&file, filename, FA_OPEN_APPEND | FA_WRITE) != FR_OK)
    {
        if (f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) // Try to create a new file if failed to open existing file
            return;

        // Write header line for new file
        f_write(&file, header, strlen(header), NULL);
        f_sync(&file);
    }

    total_to_sd = DB_GetTotalToSD();
    for (i = 0; i < total_to_sd; i++)
    {
        if (!(system_ready_status.fram_ready = DB_ToSDwithOffset(i, &data)))
            break; // Failed to read data from FRAM, stop the process

        if (!SaveRecordToSD(&file, &data))
            break; // Failed to write data to SD card, stop the process
    }
    f_close(&file);

    if (i > 0)
        system_ready_status.fram_ready = DB_IncSDTail(i); // Update SD tail in database after successfully saving "i" records to SD card
}

/* ------------------------------------------------------------------------ */
static inline float ema(float old, float new)
{
    return (SENSOR_ALPHA * new) + (SENSOR_BETA * old);
}

/* ------------------------------------------------------------------------ */
static inline void Reinitalize(void)
{
    if (!system_ready_status.usart_ready)
    {
        if ((system_ready_status.usart_ready = UART_Sys_Init()))
        {
            // A7670E Modem
            if ((system_ready_status.a7670_ready = modem_init(&huart3)))
                LED_DEBUG_BLUE_ON();
            else
                LED_DEBUG_BLUE_OFF();

            // Modbus
            system_ready_status.modbus_ready = modbus_init(&huart1);
        }
    }
    else
    {
        // A7670E Modem
        if (!system_ready_status.a7670_ready)
        {
            if ((system_ready_status.a7670_ready = modem_init(&huart3)))
                LED_DEBUG_BLUE_ON();
            else
                LED_DEBUG_BLUE_OFF();
        }

        // Modbus
        if (!system_ready_status.modbus_ready)
            system_ready_status.modbus_ready = modbus_init(&huart1);
    }
    if (!system_ready_status.bmp390_ready)
        system_ready_status.bmp390_ready = bmp390_init(&hi2c2);
    if (!system_ready_status.sht45_ready)
        system_ready_status.sht45_ready = sht45_init(&hi2c2);
    if (!system_ready_status.fram_ready)
        system_ready_status.fram_ready = DB_Init(&hspi1);

    /* -------------------------------------------------------- */
    /* If some devices are still not ready. blink the red led   */
    if (!(system_ready_status.modbus_ready && system_ready_status.bmp390_ready &&
          system_ready_status.sht45_ready && system_ready_status.fram_ready))
        ui_interface.led_red = LED_BLINK;
    else
        ui_interface.led_red = LED_OFF;
}

/* ------------------------------------------------------------------------ */
static inline void SensorUpdate(void)
{
    float sht45_temperature, sht45_humidity;
    float bmp390_temperature, bmp390_pressure;
    float rainfall;
    uint16_t light_par;

    if (system_ready_status.bmp390_ready)
        system_ready_status.bmp390_ready = bmp390_get_sensor_data(&bmp390_temperature, &bmp390_pressure);
    else
    {
        bmp390_temperature = 0.0f;
        bmp390_pressure = 0.0f;
    }

    if (system_ready_status.sht45_ready)
        system_ready_status.sht45_ready = sht45_get_sensor_data(&sht45_temperature, &sht45_humidity);
    else
    {
        sht45_temperature = 0.0f;
        sht45_humidity = 0.0f;
    }

    system_ready_status.light_ok = get_light(&light_par);
    system_ready_status.rainfall_ok = get_rain(&rainfall);

    /* Processing sensors data */

    // Adjust data
    sht45_temperature += db_meta_data.temperature_adj;
    sht45_humidity += db_meta_data.humidity_adj;
    bmp390_temperature += db_meta_data.temperature_adj;
    bmp390_pressure += db_meta_data.pressure_adj;
    light_par += db_meta_data.light_adj;

    // EMA fusion
    weather_data.temperature = ema(weather_data.temperature, (sht45_temperature + bmp390_temperature) / 2.0f);
    weather_data.humidity = ema(weather_data.humidity, sht45_humidity);
    bmp390_pressure /= 1000.0f; // Convert PA to kPA
    weather_data.pressure = ema(weather_data.pressure, bmp390_pressure);

    // EMA simulation for uint16_t from light sensor
    if (light_par & 1)
        light_par = (light_par >> 1) + 1u; // div by 2 and round-up
    else
        light_par >>= 1;

    if (weather_data.light_par & 1)
        weather_data.light_par = (weather_data.light_par >> 1) + 1u; // div by 2 and round-up
    else
        weather_data.light_par >>= 1;

    weather_data.light_par += light_par;

    // Process rain fall. Accumulate for 1 hour
    accum_rainfall += rainfall;
    if (weather_data.sampletime.minutes == 0 && weather_data.sampletime.seconds == 0)
    {
        weather_data.rainfall = accum_rainfall + db_meta_data.rainfall_adj;
        accum_rainfall = 0.0f;
    }
}

/* ------------------------------------------------------------------------ */
static inline float DewPointCalc(float RH, float T) // RH in %, T in C (such as 30 %Rh and 35 C)
{
    // For these b and c constants, (err = 0.35C for -45 < T < 60)
    //
    // Some calculations may use different b and c such as
    // b = 17.123, c = 234.95 (err = 0.15C for 0 < T < 100)

    static const float b = 17.62f;
    static const float c = 243.12f;

    float H, Dp;

    H = ((log10f(RH) - 2.0f) / 0.4343f) + ((b * T) / (c + T));
    Dp = c * H / (b - H); // this is the dew point in Celsius
    return Dp;
}

/* ------------------------------------------------------------------------ */
/*
 * The calculation is per daily basis. The required parameters are:
 *  - Mean air temperaure (Tavg)
 *  - Hours of leaf wetness (Temp < Temp(dew)) (Lw)
 *  - Hours of relative humidity > 90% (Rh)
 *
 * The calculation are in sequential steps as:
 *  1. If Tavg is outside the range of 15 to 38C, then return BUS = 0
 *  2. If Lw < 9 hours, then return BUS = 0
 *  3. Set initial BUS = Lw / 4.0
 *  4. If Rh > 16 hours, then BUS = BUS(from 3) + (Rh - 12.0) / 6.0
 *  5. If Tavg < 23C or Tavg > 26C, then BUS = BUS(from 4) - 2.0
 *  6. If Tavg < 19C or Tavg > 29C, then BUS = BUS(from 5) - 2.0
 *  7. If BUS < 0, then BUS = 0
 *
 */
#define SECONDS_IN_DAY 86400UL
static inline bool BUSCalc(float *BUS)
{
    Weather_Data_Packed_t db_data;
    uint32_t current_epoch, start_epoch;
    uint16_t db_index, total_data, total_data_per_hr, total_valid_data = 0;
    float Tavg;
    uint8_t Lw, Rh;

    *BUS = 0.0f;

    if (!(system_ready_status.fram_ready))
        return false; // Database errors

    // Calculate total records for 1 day
    total_data = 24u * (60u / (uint16_t)(db_meta_data.sampling_interval));   // Estimate total data for 24 hours.
    if(DB_GetTotalData() < total_data)
        return false; // Not enough data for calculation

    // Calculate the starting record number of data
    db_index = (DB_GetHead() - total_data) & 0x7FFF;

    // Verify that the database timestamp is correct.
    current_epoch = get_epoch_from_datetime(&weather_data.sampletime);
    start_epoch = current_epoch - SECONDS_IN_DAY;
    if (!(system_ready_status.fram_ready = DB_GetData(db_index, &db_data)))
        return false; // Failed to read database
    if (db_data.time_stamp != current_epoch - SECONDS_IN_DAY)
        return false; // Database timestamp is incorrect

    // Calculate parameters
    Tavg = 0.0f;
    Lw = 0;
    Rh = 0;
    for (uint16_t i = 0; i < total_data; i++)
    {
        if (!(system_ready_status.fram_ready = DB_GetData(db_index, &db_data)))
            return false; // Failed to read database

        if(db_data.time_stamp < start_epoch)
            continue; // Skip old data that is outside the 24-hour window

        total_valid_data++;
        Tavg += fixedpt_tofloat(db_data.temperature);
        if (db_data.humidity > fixedpt_rconst(90.0))
            Rh++;
        if (db_data.dew_point < db_data.temperature)
            Lw++;
        db_index = (db_index + 1) & 0x7FFF;
    }
    Tavg /= (float)total_valid_data;
    total_data_per_hr = total_valid_data / 24u; // Count in the unit of hour (not per sampling)
    Rh /= total_data_per_hr; // Count in the unit of hour (not per sampling)
    Lw /= total_data_per_hr;

    // Calculate BUS. BUS is already initialized to 0.0f
    //  1. If Tavg is outside the range of 15 to 38C, then return BUS = 0
    if (Tavg < 15.0f || Tavg > 38.0f)
        return true;

    //  2. If Lw < 9 hours, then return BUS = 0
    if (Lw < 9)
        return true;

    //  3. Set initial BUS = Lw / 4.0
    *BUS = (float)Lw / 4.0f;

    //  4. If Rh > 16 hours, then BUS = BUS(from 3) + (Rh - 12.0) / 6.0
    if (Rh > 16)
        *BUS += ((float)Rh - 12.0f) / 6.0f;

    //  5. If Tavg < 23C or Tavg > 26C, then BUS = BUS(from 4) - 2.0
    if (Tavg < 23.0f || Tavg > 26.0f)
        *BUS -= 2.0f;

    //  6. If Tavg < 19C or Tavg > 29C, then BUS = BUS(from 5) - 2.0
    if (Tavg < 19.0f || Tavg > 29.0f)
        *BUS -= 2.0f;

    //  7. If BUS < 0, then BUS = 0
    if (*BUS < 0.0f)
        *BUS = 0.0f;

    return true;
}

/* ------------------------------------------------------------------------ */
// STM32 HAL RTC wakeup callback (called from RTC_WKUP_IRQHandler)
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Direct-to-task notification: zero-copy, no semaphore overhead
    vTaskNotifyGiveFromISR(MainTaskHandle, &xHigherPriorityTaskWoken);

    // Yield to the notified task immediately if it has higher priority
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* ------------------------------------------------------------------------ */
void maintask(void *params)
{
    (void)params;

    /* Second stage of initialization : After RTOS scheduler */
    /* Now, all ui calls must be done through ui_task */

    ui_interface.led_red = LED_OFF;
    ui_interface.led_green = LED_OFF;

    // 1. UART subsystem
    if ((system_ready_status.usart_ready = UART_Sys_Init()))
    {
        // 2. A7670E Modem
        if ((system_ready_status.a7670_ready = modem_init(&huart3)))
            LED_DEBUG_BLUE_ON();

        // 3. Modbus
        system_ready_status.modbus_ready = modbus_init(&huart1);
    }

    // 2. Weather database
    system_ready_status.fram_ready = DB_Init(&hspi1);

    // 3. BMP390 and SHT41
    system_ready_status.bmp390_ready = bmp390_init(&hi2c2);
    system_ready_status.sht45_ready = sht45_init(&hi2c2);

    // 4. Real-time clock (RTC)
    if (datetime_sync_with_best_source() == TIME_SOURCE_NONE)
    {
        system_ready_status.datetime_ready = false;
        ui_interface.led_red = LED_ON;
        LED_DEBUG_YELLOW_ON();
    }
    else
    {
        system_ready_status.datetime_ready = true;
        ui_interface.led_red = LED_OFF;
        LED_DEBUG_YELLOW_OFF();
    }

    /* Snapshot current metadata */
    (void)DB_GetMeta(&db_meta_data);

    // Enable 1Hz interrupt
    HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);

    /* Infinite Loop */
    while (1)
    {
        // Block until ISR sends notification — no CPU usage while waiting
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Main operation for every 1 second */
        /* Update SD Card status */
        system_ready_status.sd_detected = SD_INSERTED_STATUS();
        system_ready_status.sd_write_protected = SD_WRITE_PROTECTED_STATUS();

        /* All operation require precise time from the system.
         * If we cannot retrieve correct time, we cannot do any thing.
         */
        if (!system_ready_status.datetime_ready)
        {
            if (datetime_sync_with_best_source() == TIME_SOURCE_NONE)
            {
                ui_interface.led_red = LED_ON;
                LED_DEBUG_YELLOW_ON();
                continue; // Still unable to sync time.
            }
            else
            {
                ui_interface.led_red = LED_OFF;
                LED_DEBUG_YELLOW_OFF();
            }
        }

        if (!datetime_get_datetime_from_rtc(&(weather_data.sampletime)))
        {
            ui_interface.led_red = LED_BLINK;
            LED_DEBUG_RED_ON();
            continue; // Unable to get time from RTC, we cannot do any thing.
        }
        else
        {
            ui_interface.led_red = LED_OFF;
            LED_DEBUG_RED_OFF();
        }

        /* -------------------------------------------------------- */
        /* Re-initialize failed devices */
        Reinitalize();

        /* -------------------------------------------------------- */
        /* Sensors operation */

        // Measure sensors every 10 second. Also update metadata
        if (weather_data.sampletime.seconds % 10 == 0)
        {
            /* Snapshot current metadata*/
            (void)DB_GetMeta(&db_meta_data);

            SensorUpdate();
            weather_data.dew_point = DewPointCalc(weather_data.humidity, weather_data.temperature);
        }

        /* BUS calculation on midnight */
        if (weather_data.sampletime.seconds == 0 &&
            weather_data.sampletime.minutes == 0 &&
            weather_data.sampletime.hours == 0)
        {
            if (!(BUSCalc(&(weather_data.bus_value))))
                printf("BUS Calculation error!!!\r\n");
        }

        /* Save sensor data to FRAM when every configured datetime. */
        if (weather_data.sampletime.seconds == 0 &&
            (weather_data.sampletime.minutes % db_meta_data.sampling_interval) == 0)
        {
            Weather_Data_Packed_t data;

            // Save current measurement to FRAM database
            PackData(&weather_data, &data);
            if (!(system_ready_status.fram_ready = DB_AddData(&data)))
            {
                printf("Failed to add data to FRAM\r\n");
                ui_interface.led_green = LED_BLINK;
                continue; // Failed to add data to FRAM, skip the rest of this iteration
            }

            // Save unsaved data to SD card
            if (system_ready_status.sd_detected && !system_ready_status.sd_write_protected)
            {
                SaveToSD();
            }
        }

        if (system_ready_status.fram_ready)
            ui_interface.led_green = LED_ON;
        else
            ui_interface.led_green = LED_BLINK;

        /* Trig the SSL upload task to activate twice daily */
        if (weather_data.sampletime.seconds == 0 &&
            weather_data.sampletime.minutes == 0 &&
            (weather_data.sampletime.hours == 0 || weather_data.sampletime.hours == 12))
        {
            xTaskNotifyGive(SslUploadTaskHandle);
        }
    }
}
