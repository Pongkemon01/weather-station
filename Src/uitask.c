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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "i2c.h"
#include "ui.h"

/*
 LCD module is connected to MCP23017 port A with these corresponding bits:
   - D7 - D4 : GPA3 - GPA0 (4-bit data bus)
   - E :       GPA4 (Operation Enable : Falling-edge triggered)
   - R/W :     GPA5 (Read or Write operation : 0 = Write, 1 = Read)
   - R/S :     GPA6 (Command or Data : 0 = Command, 1 = Data)
   - BK-ON :   GPA7 (Turn-on Back-light)

 MCP23017 port B connects to LEDs and switches
   - Red LED : GPB7
   - Green LED : GPB6
   - N/A : GPB5 : GPB4
   - UP switch : GPB3
   - DOWN switch : GPB2
   - ENTER switch : GPB1
   - MENU switch : GPB0
*/

static uint8_t led_blinking_counter;
static uint8_t previous_sw_status;
UI_Interface_t ui_interface = {0};

void uitask(void *params)
{
    (void)params;
    // Initialize the last execution time
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20); // 20ms period

    previous_sw_status = 0u;
    led_blinking_counter = 0u;

    /* Initial data structure */
    if ((ui_interface.mutex = xSemaphoreCreateMutex()) == NULL)
        LED_DEBUG_RED_ON();
    else
        LED_DEBUG_RED_OFF();

    if (ui_interface.mutex != NULL && system_ready_status.ui_ready)
        ui_lcd_clear();

    /* Infinite Loop repeating every 20ms */
    while (1)
    {
        uint8_t current_sw_status;
        // Wait for the next cycle
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        // UI main operation
        if (system_ready_status.ui_ready)
        {
            // 1. Read switches
            // Switches are active-low, therefore, we invert the status
            current_sw_status = ~ui_key_status() & 0x0F;

            // Parse each switch. loop cycle time is 20ms. appropriate for de-bouncing
            if ((current_sw_status & 0x01) == (previous_sw_status & 0x01))
                ui_interface.key_menu = (current_sw_status & 0x01);
            if ((current_sw_status & 0x02) == (previous_sw_status & 0x02))
                ui_interface.key_enter = (current_sw_status & 0x02);
            if ((current_sw_status & 0x04) == (previous_sw_status & 0x04))
                ui_interface.key_down = (current_sw_status & 0x04);
            if ((current_sw_status & 0x08) == (previous_sw_status & 0x08))
                ui_interface.key_up = (current_sw_status & 0x08);

            previous_sw_status = current_sw_status;

            // 2. Update LED status
            // Update blinking counter
            if (++led_blinking_counter >= 50)
                led_blinking_counter = 0u;

            switch (ui_interface.led_red)
            {
            case LED_OFF:
                ui_led_red_off();
                break;
            case LED_ON:
                ui_led_red_on();
                break;
            case LED_BLINK:
                if (led_blinking_counter < 25)
                    ui_led_red_on();
                else
                    ui_led_red_off();
                break;
            default:
                ui_led_red_off();
                break;
            }

            switch (ui_interface.led_green)
            {
            case LED_OFF:
                ui_led_green_off();
                break;
            case LED_ON:
                ui_led_green_on();
                break;
            case LED_BLINK:
                if (led_blinking_counter < 25)
                    ui_led_green_on();
                else
                    ui_led_green_off();
                break;
            default:
                ui_led_green_off();
                break;
            }

            // 3. LCD back-light update
            if (ui_interface.lcd_bk_on)
                ui_lcd_bk_on();
            else
                ui_lcd_bk_off();

            // 4. LCD cursor update
            if (ui_interface.lcd_cursor_on)
                ui_lcd_cursor_on();
            else
                ui_lcd_cursor_off();

            // 5. LCD refresh
            if (ui_interface.lcd_need_updated)
            {
                if (xSemaphoreTake(ui_interface.mutex, pdMS_TO_TICKS(5)) == pdTRUE)
                {
                    ui_lcd_printXY(0, 0, ui_interface.disp[0]);
                    ui_lcd_printXY(0, 1, ui_interface.disp[1]);
                    ui_interface.lcd_need_updated = false;
                    xSemaphoreGive(ui_interface.mutex);
                }
            }
        }
        else
        {
            // UI system still failed to initialized. Re-initialize it
            if ((system_ready_status.ui_ready = ui_init(&hi2c1)))
            {
                ui_lcd_clear();
                ui_led_set_value(0);
                LED_DEBUG_RED_OFF();
            }
        }
    }
}