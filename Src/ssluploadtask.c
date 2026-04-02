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

void ssluploadtask(void *params)
{
    (void)params;

    while(1)
    {

    }
}