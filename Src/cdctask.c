#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "tusb.h"
#include "nv_database.h"
#include "weather_data.h"
#include "datetime.h"

//--------------------------------------------------------------------+
// USB CDC
//--------------------------------------------------------------------+
void cdc_task(void *params)
{
    (void)params;

    // RTOS forever loop
    while (1)
    {
        // connected() check for DTR bit
        // Most but not all terminal client set this when making connection
        // if ( tud_cdc_connected() )
        {
            // There are data available
            while (tud_cdc_available())
            {
                uint8_t buf[64];

                // read and echo back
                uint32_t count = tud_cdc_read(buf, sizeof(buf));
                (void)count;

                // Echo back
                // Note: Skip echo by commenting out write() and write_flush()
                // for throughput test e.g
                //    $ dd if=/dev/zero of=/dev/ttyACM0 count=10000
                tud_cdc_write(buf, count);
            }

            tud_cdc_write_flush();

            // Press on-board button to send Uart status notification
            static uint32_t btn_prev = 0;
            static cdc_notify_uart_state_t uart_state = {.value = 0};
            // const uint32_t btn = board_button_read();
            const uint32_t btn = 0; // Dummy
            if (!btn_prev && btn)
            {
                uart_state.dsr ^= 1;
                tud_cdc_notify_uart_state(&uart_state);
            }
            btn_prev = btn;
        }

        // For ESP32-Sx this delay is essential to allow idle how to run and reset watchdog
        vTaskDelay(1);
    }
}

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;

    // TODO set some indicator
    if (dtr)
    {
        // Terminal connected
    }
    else
    {
        // Terminal disconnected
    }
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
}
