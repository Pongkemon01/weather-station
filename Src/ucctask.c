/* User control task to manage user interface */
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "weather_data.h"
#include "ui.h"
#include "nv_database.h"

extern Weather_Data_t weather_data; // Current weather data, defined in maintask.c

void ucctask(void *params)
{
    TickType_t xLastWakeTime;

    (void)params;
    xLastWakeTime = xTaskGetTickCount();

    while(1)
    {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
}