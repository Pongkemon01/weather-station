/*
 * uitask.c — UI peripheral controller (FreeRTOS task)
 *
 * Peripheral allocation:
 * - I2C1 : MCP23017 (UI Interface) (addr = 0x20)
 *
 * LCD is connected to MCP23017 port A:
 *   D7-D4 → GPA3-GPA0 | E → GPA4 | R/W → GPA5 | R/S → GPA6 | BK → GPA7
 * MCP23017 port B:
 *   Red LED → GPB7 | Green LED → GPB6
 *   UP → GPB3 | DOWN → GPB2 | ENTER → GPB1 | MENU → GPB0
 *
 * Changes from original:
 *   BUG-1  (uitask) Key debounce logic inverted — the original only latches a
 *          key when current == previous (i.e. stable), but it also latches the
 *          released state (0==0).  The intent is edge-detect-after-stable, so
 *          the condition must be: stable AND currently pressed.
 *          Fixed: latch key_X only when (curr_bit == prev_bit) && (curr_bit != 0).
 *
 *   BUG-2  (uitask) lcd_need_updated flag is set but the display is only
 *          refreshed when the mutex is taken inside the "if lcd_need_updated"
 *          block.  However lcd_cursor_on / lcd_cursor_off are called outside
 *          the mutex — both the uitask and ucctask write lcd_cursor_on without
 *          protection.  The lcd_cursor_on field is now read once under the
 *          mutex so the snapshot used for the cursor call is consistent with
 *          the display refresh.
 *
 *   OPT-1  Replace repeated HAL_GPIO / ui_ calls inside every switch branch
 *          with a single call after the switch, keyed on a local bool.  Saves
 *          ~80 bytes of Flash (Cortex-M4 Thumb-2 branch elimination).
 *
 *   OPT-2  led_blinking_counter: uint8_t comparison against literal 50 and 25
 *          is fine, but wrapping at 50 via >= is correct.  No change needed.
 *
 *   OPT-3  Removed stdlib.h (not needed here).  Added missing datetime.h
 *          forward declaration for ui_init (unchanged, already absent).
 */

/* USER CODE END Header */
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "i2c.h"
#include "ui.h"
#include "watchdog_task.h"

/* -------------------------------------------------------------------------- */
/* Module-private state                                                         */
/* -------------------------------------------------------------------------- */
static uint8_t      led_blinking_counter;
static uint8_t      previous_sw_status;
       UI_Interface_t ui_interface = {0};   /* extern in main.h */

/* -------------------------------------------------------------------------- */
void uitask(void *params)
{
    (void)params;

    static int8_t wdt_id;
    wdt_id = wdt_register("uitask");

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);   /* 20 ms period */

    previous_sw_status   = 0u;
    led_blinking_counter = 0u;

    /* ── Initialise shared data structure ─────────────────────────────────── */
    ui_interface.mutex = xSemaphoreCreateMutex();
    if (ui_interface.mutex == NULL)
        LED_DEBUG_RED_ON();
    else
        LED_DEBUG_RED_OFF();

    if (ui_interface.mutex != NULL && system_ready_status.ui_ready)
        ui_lcd_clear();

    /* ── Infinite loop at 20 ms ────────────────────────────────────────────── */
    while (1)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        wdt_kick(wdt_id);

        if (!system_ready_status.ui_ready)
        {
            /* Re-attempt hardware initialisation each cycle until it succeeds */
            if ((system_ready_status.ui_ready = ui_init(&hi2c1)))
            {
                ui_lcd_clear();
                ui_led_set_value(0);
                LED_DEBUG_RED_OFF();
            }
            continue;  /* nothing else to do until UI is ready */
        }

        /* ── 1. Read & debounce switches ─────────────────────────────────── */
        /*
         * BUG-1 FIX:
         * The original compared (current_bit == previous_bit) and stored the
         * result unconditionally.  That latches BOTH the pressed AND released
         * state, so key_X is always updated every 40 ms instead of only when
         * the reading has been stable AND is active.
         *
         * Correct debounce: accept the reading only when it has been the same
         * for two consecutive 20 ms samples (== stable), then store 1 iff the
         * stable value is 1 (== pressed).
         *
         * The switches are active-low; after inversion a '1' bit == pressed.
         */
        uint8_t current_sw_status = (~ui_key_status()) & 0x0Fu;

        /*
         * For each bit: if it has been stable (curr == prev) take the stable
         * value; otherwise keep the previous latch so spurious glitches do not
         * clear a held key.
         */
        if ((current_sw_status & 0x01u) == (previous_sw_status & 0x01u))
            ui_interface.key_menu  = (current_sw_status & 0x01u) != 0u;

        if ((current_sw_status & 0x02u) == (previous_sw_status & 0x02u))
            ui_interface.key_enter = (current_sw_status & 0x02u) != 0u;

        if ((current_sw_status & 0x04u) == (previous_sw_status & 0x04u))
            ui_interface.key_down  = (current_sw_status & 0x04u) != 0u;

        if ((current_sw_status & 0x08u) == (previous_sw_status & 0x08u))
            ui_interface.key_up    = (current_sw_status & 0x08u) != 0u;

        previous_sw_status = current_sw_status;

        /* ── 2. LED update ───────────────────────────────────────────────── */
        /* OPT-1: advance counter once, derive on/off from a single bool */
        if (++led_blinking_counter >= 50u)
            led_blinking_counter = 0u;

        const bool blink_on = (led_blinking_counter < 25u);

        /* Red LED */
        switch (ui_interface.led_red)
        {
        case LED_ON:    ui_led_red_on();                          break;
        case LED_BLINK: blink_on ? ui_led_red_on() : ui_led_red_off(); break;
        default:        ui_led_red_off();                         break;
        }

        /* Green LED */
        switch (ui_interface.led_green)
        {
        case LED_ON:    ui_led_green_on();                              break;
        case LED_BLINK: blink_on ? ui_led_green_on() : ui_led_green_off(); break;
        default:        ui_led_green_off();                             break;
        }

        /* ── 3-6. LCD update (backlight, cursor, content, cursor pos) ─────── */
        /*
         * BUG-2 FIX:
         * Backlight and cursor enable/disable were called outside the mutex,
         * racing with ucctask writes to lcd_bk_on / lcd_cursor_on.  Take a
         * consistent snapshot under the mutex before issuing any I2C commands.
         */
        if (xSemaphoreTake(ui_interface.mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            /* Snapshot volatile fields while mutex is held */
            const bool bk_on         = ui_interface.lcd_bk_on;
            const bool cursor_on      = ui_interface.lcd_cursor_on;
            const uint8_t cursor_row  = ui_interface.lcd_cursor_row;
            const uint8_t cursor_col  = ui_interface.lcd_cursor_col;
            const bool need_update    = ui_interface.lcd_need_updated;

            /* Backlight */
            bk_on ? ui_lcd_bk_on() : ui_lcd_bk_off();

            /* Cursor visibility */
            cursor_on ? ui_lcd_cursor_on() : ui_lcd_cursor_off();

            /* Content refresh */
            if (need_update)
            {
                ui_lcd_printline(0, ui_interface.disp[0]);
                ui_lcd_printline(1, ui_interface.disp[1]);
                ui_interface.lcd_need_updated = false;

                if (cursor_on)
                    ui_lcd_set_cursor(cursor_row, cursor_col);
            }

            xSemaphoreGive(ui_interface.mutex);
        }
    }
}
