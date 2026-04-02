/* User control task to manage user interface */
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "weather_data.h"
#include "ui.h"
#include "nv_database.h"

void ucctask(void *params)
{
    (void)params;

    while(1)
    {

    }
}