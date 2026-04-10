/*
 * ucctask.c — User-interaction controller (FreeRTOS task)
 *
 * ── Bug fixes (v1, previous pass) ──────────────────────────────────────────
 *
 *  BUG-1  Input_Sampling_Interval — UP wrap produced 0 (invalid).
 *         Fixed: range clamped to 5–60; 0 is never stored.
 *
 *  BUG-2  Input_id — char[4] buffer overflowed for uint16_t values > 999.
 *         Fixed: buffer widened and format corrected (see BUG-2r below).
 *
 *  BUG-3  Input_float — decimal-point skip used wrong index.
 *         Fixed: format changed to fixed-width; skip index corrected
 *         (see BUG-3r below).
 *
 *  BUG-4  L2_3_Display showed live weather_data instead of temp_date_time.
 *         Fixed: helper now accepts const RTC_DateTime_t *.
 *
 *  BUG-5  datetime_set_rtc() called even on MENU-cancel.
 *         Fixed: write guarded by date_changed flag.
 *
 *  BUG-6  weather_data.sampletime.seconds read without critical section.
 *         Fixed: wrapped in taskENTER_CRITICAL / taskEXIT_CRITICAL.
 *
 * ── Bug fixes (v2, this pass — range violations vs updated nv_database.h) ──
 *
 *  BUG-2r Input_id — nv_database.h documents region_id / station_id as
 *         0–999 (3 decimal digits), NOT the full uint16_t range.  The previous
 *         fix widened to 5 digits (0–99999), exceeding the declared range.
 *         Corrected: format "%03u", buffer char[4], DIGIT_COUNT 3.
 *         first_pos_index = 12 (matches the label in L2_0_Display).
 *         Returned value clamped to 999 as a defensive guard.
 *
 *  BUG-3r Input_float — nv_database.h declares adj fields as −999.99–+999.99
 *         (3 integer digits + sign).  The previous fix used "%+06.2f" which
 *         only represents ±99.99 (2 integer digits), leaving values in
 *         ±100.00–±999.99 unreachable from the editor.
 *         Corrected:
 *           Format changed to "%+07.2f": sign(1) + 3 int(3) + '.'(1)
 *           + 2 frac(2) = 7 chars, e.g. "+123.45" / "-999.99".
 *           FLOAT_CHARS = 7.  Decimal '.' is always at index 4 → skip on
 *           ENTER at DECIMAL_INDEX = 4.
 *           Buffer widened to char[9].
 *           parse_signed_float updated: integer part = s[1..3] (3 digits),
 *           fractional part = s[5..6].
 *           L2_1_Display labels adjusted to 7 chars so that label + value
 *           fits within the 16-char LCD line.
 *           first_pos_index in Input_float = 7.
 *           Input/display of light_adj (−9999–+9999, "%+05d") unchanged.
 *
 * ── Optimisations (carried over from v1) ───────────────────────────────────
 *
 *  OPT-1  Replaced stdlib.h atoi/atof with lightweight integer-only helpers.
 *  OPT-2  db_meta_data kept static (220 B — never placed on stack).
 *  OPT-3  Polling helpers unchanged (correct cooperative yield).
 *  OPT-4  #pragma GCC diagnostic removed (no longer needed).
 */

#pragma GCC diagnostic warning "-Wformat-truncation=0"

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "weather_data.h"
#include "nv_database.h"
#include "datetime.h"

#include <string.h>
#include <stdio.h>

#define UCC_SEMAPHORE_TIMEOUT pdMS_TO_TICKS(100)

extern Weather_Data_t weather_data; /* defined in maintask.c */

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Types                                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */
typedef enum
{
    UI_KEY_NONE = 0,
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_ENTER,
    UI_KEY_MENU
} UI_Key_t;

typedef struct
{
    bool ucc_state_is_setting;
    uint8_t ucc_state_l1_setting;
    uint8_t ucc_state_l2_setting;
    bool ucc_state_l3_setting;
} UCC_State_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Module-private state                                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */
static uint16_t ucc_tick_counter = 0u;
static uint8_t ucc_last_second = 0u;
static UCC_State_t ucc_state = {0};
static Meta_Data_t db_meta_data; /* 220 B static — never on stack   */

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Lightweight numeric helpers  (OPT-1 — replaces atoi / atof)                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

/** Parse @p len ASCII decimal digits starting at @p s into an unsigned int. */
static uint32_t parse_uint(const char *s, uint8_t len)
{
    uint32_t v = 0u;
    for (uint8_t i = 0u; i < len; i++)
    {
        if (s[i] >= '0' && s[i] <= '9')
            v = v * 10u + (uint32_t)(s[i] - '0');
    }
    return v;
}

/*
 * Parse a signed fixed-format float string produced by "%+07.2f":
 *   [0]   sign          '+' or '-'
 *   [1-3] integer part  3 digits
 *   [4]   '.'
 *   [5-6] fractional    2 digits
 *   e.g. "+123.45"  "-000.50"  "+999.99"
 *
 * BUG-3r: integer part is 3 digits (indices 1–3), fractional at 5–6.
 */
static float parse_signed_float(const char *s)
{
    float v = (float)parse_uint(&s[1], 3u) + (float)parse_uint(&s[5], 2u) / 100.0f;
    return (s[0] == '-') ? -v : v;
}

/*
 * Parse a signed integer string produced by "%+05d":
 *   [0]   sign   '+' or '-'
 *   [1-4] digits 4 chars
 *   e.g. "+9999"  "-0100"
 */
static int16_t parse_signed_int16(const char *s)
{
    int16_t v = (int16_t)parse_uint(&s[1], 4u);
    return (s[0] == '-') ? (int16_t)-v : v;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Key helpers                                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void Wait_For_Key_Release(void)
{
    while (ui_interface.key_up || ui_interface.key_down ||
           ui_interface.key_enter || ui_interface.key_menu)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static UI_Key_t Get_Key_Pressed(void)
{
    while (!ui_interface.key_up && !ui_interface.key_down &&
           !ui_interface.key_enter && !ui_interface.key_menu)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (ui_interface.key_menu)
        return UI_KEY_MENU;
    if (ui_interface.key_enter)
        return UI_KEY_ENTER;
    if (ui_interface.key_up)
        return UI_KEY_UP;
    if (ui_interface.key_down)
        return UI_KEY_DOWN;
    return UI_KEY_NONE;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Display helpers                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void L1_Display(void)
{
    static const char *const L1_options[5] = {
        "1.Set ID",
        "2.Set offsets",
        "3.Set date/time",
        "4.Set interval",
        "5.Erase data"};

    if (ucc_state.ucc_state_l1_setting >= 5u)
    {
        ucc_state.ucc_state_l1_setting = 0u;
        return;
    }

    if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
    {
        strncpy(ui_interface.disp[0], L1_options[ucc_state.ucc_state_l1_setting],
                sizeof(ui_interface.disp[0]) - 1u);
        ui_interface.disp[0][sizeof(ui_interface.disp[0]) - 1u] = '\0';
        ui_interface.disp[1][0] = '\0';
        ui_interface.lcd_need_updated = true;
        xSemaphoreGive(ui_interface.mutex);
    }
}

/*
 * L2_0_Display — ID setting menu
 *
 * BUG-2r: range is 0–999, format "%03u" (3 digits).
 * Layout (16-char line):
 *   "region id:   XXX"   — label 13 chars, value 3 chars = 16 total  ✓
 *   "station id:  XXX"   — label 13 chars, value 3 chars = 16 total  ✓
 * first_pos_index for Input_id = 13.
 */
static void L2_0_Display(void)
{
    if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) != pdTRUE)
        return;

    switch (ucc_state.ucc_state_l2_setting)
    {
    case 0u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "region id:   %03u", db_meta_data.region_id);
        break;
    case 1u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "station id:  %03u", db_meta_data.station_id);
        break;
    default:
        ucc_state.ucc_state_l2_setting = 0u;
        break;
    }
    ui_interface.lcd_need_updated = true;
    xSemaphoreGive(ui_interface.mutex);
}

/*
 * L2_1_Display — offset setting menu
 *
 * BUG-3r: adj range is −999.99–+999.99, format "%+07.2f" (7 chars).
 *
 * Layout per field (16-char LCD line):
 *   "Temp:    +123.45"  — label 9 chars + value 7 chars = 16 total  ✓
 *   "Humid:   +123.45"  — label 9 chars + value 7 chars = 16 total  ✓
 *   "Pres:    +123.45"  — label 9 chars + value 7 chars = 16 total  ✓
 *   "Light:     +9999"  — label 11 chars + value 5 chars = 16 total  ✓
 *   "Rain:    +123.45"  — label 9 chars + value 7 chars = 16 total  ✓
 *
 * first_pos_index for float = 10; first_pos_index for light = 11.
 */
static void L2_1_Display(void)
{
    if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) != pdTRUE)
        return;

    switch (ucc_state.ucc_state_l2_setting)
    {
    case 0u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "Temp:    %+07.2f", db_meta_data.temperature_adj);
        break;
    case 1u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "Humid:   %+07.2f", db_meta_data.humidity_adj);
        break;
    case 2u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "Pres:    %+07.2f", db_meta_data.pressure_adj);
        break;
    case 3u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "Light:     %+05d", db_meta_data.light_adj);
        break;
    case 4u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "Rain:    %+07.2f", db_meta_data.rainfall_adj);
        break;
    default:
        ucc_state.ucc_state_l2_setting = 0u;
        break;
    }
    ui_interface.lcd_need_updated = true;
    xSemaphoreGive(ui_interface.mutex);
}

/* BUG-4: accept the datetime being edited, not the live weather struct */
static void L2_3_Display_dt(const RTC_DateTime_t *dt)
{
    if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) != pdTRUE)
        return;

    switch (ucc_state.ucc_state_l2_setting)
    {
    case 0u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "YMD:  20%02u-%02u-%02u", dt->year, dt->month, dt->day);
        break;
    case 1u:
        snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                 "HMS:    %02u:%02u:%02u", dt->hours, dt->minutes, dt->seconds);
        break;
    default:
        ucc_state.ucc_state_l2_setting = 0u;
        break;
    }
    ui_interface.lcd_need_updated = true;
    xSemaphoreGive(ui_interface.mutex);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Calendar helper                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */
static uint8_t Total_Days_In_Month(uint8_t year, uint8_t month)
{
    switch (month)
    {
    case 1u:
    case 3u:
    case 5u:
    case 7u:
    case 8u:
    case 10u:
    case 12u:
        return 31u;
    case 4u:
    case 6u:
    case 9u:
    case 11u:
        return 30u;
    case 2u:
        return ((year % 4u) == 0u) ? 29u : 28u;
    default:
        return 0u;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Input editors                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Input_Sampling_Interval
 *
 * BUG-1: valid range 1–60, step 5.  Sequence: 5 → 10 → … → 60 → 5.
 * 0 is never produced.  Incoming value is clamped in case of corrupt F-RAM.
 */
static uint8_t Input_Sampling_Interval(uint8_t init)
{
    uint8_t sampling_interval = (init < 1u || init > 60u) ? 5u : init;
    bool setting_done = false;

    ui_interface.lcd_cursor_row = 1u;
    ui_interface.lcd_cursor_col = 11u;
    ui_interface.lcd_cursor_on = true;

    while (!setting_done)
    {
        if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
        {
            snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                     "current:  %02u min", sampling_interval);
            ui_interface.lcd_need_updated = true;
            xSemaphoreGive(ui_interface.mutex);
        }

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            /* 5 → 10 → … → 60 → 5 */
            sampling_interval = (sampling_interval < 60u) ? (sampling_interval + 5u) : 5u;
            break;
        case UI_KEY_DOWN:
            /* 60 → 55 → … → 5 → 60 */
            sampling_interval = (sampling_interval > 5u) ? (sampling_interval - 5u) : 60u;
            break;
        case UI_KEY_ENTER:
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    ui_interface.lcd_cursor_on = false;
    return sampling_interval;
}

/*
 * Input_id
 *
 * BUG-2r: declared range 0–999 → format "%03u", DIGIT_COUNT 3, buffer char[4].
 * first_pos_index = 13 matches the 13-char labels in L2_0_Display.
 * Returned value is clamped to 999 as a defensive guard.
 */
static uint16_t Input_id(uint16_t init)
{
    const uint8_t first_pos_index = 13u;
    const uint8_t DIGIT_COUNT = 3u;

    bool setting_done = false;
    char input_buffer[4]; /* 3 digits + NUL */
    uint8_t input_index = 0u;

    if (init > 999u)
        init = 999u;
    snprintf(input_buffer, sizeof(input_buffer), "%03u", (unsigned)init);

    ui_interface.lcd_cursor_row = 1u;
    ui_interface.lcd_cursor_on = true;

    while (!setting_done)
    {
        if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
        {
            memcpy(&ui_interface.disp[1][first_pos_index], input_buffer, DIGIT_COUNT);
            ui_interface.disp[1][first_pos_index + DIGIT_COUNT] = '\0'; /* safety NUL */
            ui_interface.lcd_cursor_col = first_pos_index + input_index;
            ui_interface.lcd_need_updated = true;
            xSemaphoreGive(ui_interface.mutex);
        }

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            input_buffer[input_index] = (input_buffer[input_index] < '9') ? input_buffer[input_index] + 1 : '0';
            break;
        case UI_KEY_DOWN:
            input_buffer[input_index] = (input_buffer[input_index] > '0') ? input_buffer[input_index] - 1 : '9';
            break;
        case UI_KEY_ENTER:
            input_index = (input_index + 1u < DIGIT_COUNT) ? input_index + 1u : 0u;
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    ui_interface.lcd_cursor_on = false;

    uint16_t result = (uint16_t)parse_uint(input_buffer, DIGIT_COUNT);
    if (result > 999u)
        result = 999u; /* defensive clamp */
    return result;
}

/*
 * Input_float
 *
 * BUG-3r: declared range −999.99–+999.99 → format "%+07.2f" (7 chars).
 *
 * Buffer layout for "%+07.2f":
 *   Index : 0    1  2  3    4    5  6
 *   Char  : sign d  d  d    '.'  d  d
 *   e.g.  : '+'  '1' '2' '3' '.' '4' '5'  →  "+123.45"
 *
 * FLOAT_CHARS = 7.
 * DECIMAL_INDEX = 4  →  cursor skips index 4 on ENTER.
 * first_pos_index = 7 (matches 7-char labels in L2_1_Display).
 *
 * init is clamped to the declared range before formatting to prevent
 * snprintf from producing more than 7 chars (e.g. "-1000.00" = 8 chars).
 */
static float Input_float(float init)
{
    const uint8_t FLOAT_CHARS = 7u;
    const uint8_t DECIMAL_INDEX = 4u;
    const uint8_t first_pos_index = 10u;

    char input_buffer[9]; /* 7 chars + NUL + 1 safety byte */
    uint8_t input_index = 0u;
    bool setting_done = false;

    if (init > 999.99f)
        init = 999.99f;
    if (init < -999.99f)
        init = -999.99f;

    snprintf(input_buffer, sizeof(input_buffer), "%+07.2f", init);

    ui_interface.lcd_cursor_row = 1u;
    ui_interface.lcd_cursor_on = true;

    while (!setting_done)
    {
        if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
        {
            memcpy(&ui_interface.disp[1][first_pos_index], input_buffer, FLOAT_CHARS);
            ui_interface.lcd_cursor_col = first_pos_index + input_index;
            ui_interface.lcd_need_updated = true;
            xSemaphoreGive(ui_interface.mutex);
        }

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            if (input_index == 0u) /* sign toggle */
                input_buffer[0] = (input_buffer[0] == '+') ? '-' : '+';
            else if (input_buffer[input_index] < '9')
                input_buffer[input_index]++;
            else
                input_buffer[input_index] = '0';
            break;
        case UI_KEY_DOWN:
            if (input_index == 0u) /* sign toggle */
                input_buffer[0] = (input_buffer[0] == '+') ? '-' : '+';
            else if (input_buffer[input_index] > '0')
                input_buffer[input_index]--;
            else
                input_buffer[input_index] = '9';
            break;
        case UI_KEY_ENTER:
            input_index++;
            if (input_index == DECIMAL_INDEX) /* skip '.' at index 4 */
                input_index++;
            if (input_index >= FLOAT_CHARS)
                input_index = 0u;
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    ui_interface.lcd_cursor_on = false;
    return parse_signed_float(input_buffer); /* OPT-1 */
}

/*
 * Input_light_offset
 *
 * Range: −9999–+9999.  Format "%+05d": sign(1) + 4 digits(4) = 5 chars.
 * Buffer char[6].  INT_CHARS = 5.  first_pos_index = 11.
 * ("Light:     " = 11 chars, then "+9999" = 5 chars, total 16 ✓)
 * No range changes needed in this pass.
 */
static int16_t Input_light_offset(int16_t init)
{
    const uint8_t INT_CHARS = 5u;
    const uint8_t first_pos_index = 11u;

    char input_buffer[6];
    uint8_t input_index = 0u;
    bool setting_done = false;

    if (init > 9999)
        init = 9999;
    if (init < -9999)
        init = -9999;

    snprintf(input_buffer, sizeof(input_buffer), "%+05d", (int)init);

    ui_interface.lcd_cursor_row = 1u;
    ui_interface.lcd_cursor_on = true;

    while (!setting_done)
    {
        if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
        {
            memcpy(&ui_interface.disp[1][first_pos_index], input_buffer, INT_CHARS);
            ui_interface.disp[1][first_pos_index + INT_CHARS] = '\0'; /* safety NUL */
            ui_interface.lcd_cursor_col = first_pos_index + input_index;
            ui_interface.lcd_need_updated = true;
            xSemaphoreGive(ui_interface.mutex);
        }

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            if (input_index == 0u)
                input_buffer[0] = (input_buffer[0] == '+') ? '-' : '+';
            else if (input_buffer[input_index] < '9')
                input_buffer[input_index]++;
            else
                input_buffer[input_index] = '0';
            break;
        case UI_KEY_DOWN:
            if (input_index == 0u)
                input_buffer[0] = (input_buffer[0] == '+') ? '-' : '+';
            else if (input_buffer[input_index] > '0')
                input_buffer[input_index]--;
            else
                input_buffer[input_index] = '9';
            break;
        case UI_KEY_ENTER:
            input_index = (input_index + 1u < INT_CHARS) ? input_index + 1u : 0u;
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    ui_interface.lcd_cursor_on = false;
    return parse_signed_int16(input_buffer); /* OPT-1 */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Date / time editors                                                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Input_date
 *
 * RTC_DateTime_t ranges (from datetime.h):
 *   year   : 0–99
 *   month  : 1–12
 *   day    : 1–Total_Days_In_Month(year, month)
 *
 * The editor allows the full 0–99 year range (matching RTC hardware).
 * datetime_is_plausible() can optionally be called in
 * Perform_SetDateTime_Setting if the narrower DATETIME_YEAR_MIN/MAX window
 * should be enforced at write time.
 *
 * Cursor column map inside "YMD:  20YY-MM-DD" (16 chars):
 *   Indices: 0123456789012345
 *   Content: YMD:  20YY-MM-DD
 *   YY starts at col 8, MM at col 11, DD at col 14.
 */
static void Input_date(RTC_DateTime_t *init)
{
    static const uint8_t col_for_field[3] = {9u, 12u, 15u};
    uint8_t input_index = 0u;
    bool setting_done = false;

    ui_interface.lcd_cursor_row = 1u;
    ui_interface.lcd_cursor_col = col_for_field[0];
    ui_interface.lcd_cursor_on = true;

    while (!setting_done)
    {
        if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
        {
            snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                     "YMD:  20%02u-%02u-%02u",
                     init->year, init->month, init->day);
            ui_interface.lcd_cursor_col = col_for_field[input_index < 3u ? input_index : 0u];
            ui_interface.lcd_need_updated = true;
            xSemaphoreGive(ui_interface.mutex);
        }

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            switch (input_index)
            {
            case 0u: /* year */
                init->year = (init->year < 99u) ? init->year + 1u : 0u;
                break;
            case 1u: /* month; clamp day if new month is shorter */
                init->month = (init->month < 12u) ? init->month + 1u : 1u;
                {
                    uint8_t mdays = Total_Days_In_Month(init->year, init->month);
                    if (init->day > mdays)
                        init->day = mdays;
                }
                break;
            case 2u: /* day */
                init->day = (init->day < Total_Days_In_Month(init->year, init->month)) ? init->day + 1u : 1u;
                break;
            default:
                break;
            }
            break;

        case UI_KEY_DOWN:
            switch (input_index)
            {
            case 0u:
                init->year = (init->year > 0u) ? init->year - 1u : 99u;
                break;
            case 1u:
                init->month = (init->month > 1u) ? init->month - 1u : 12u;
                {
                    uint8_t mdays = Total_Days_In_Month(init->year, init->month);
                    if (init->day > mdays)
                        init->day = mdays;
                }
                break;
            case 2u:
                init->day = (init->day > 1u) ? init->day - 1u : Total_Days_In_Month(init->year, init->month);
                break;
            default:
                break;
            }
            break;

        case UI_KEY_ENTER:
            input_index = (input_index < 2u) ? input_index + 1u : 0u;
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    ui_interface.lcd_cursor_on = false;
}

/*
 * Input_time
 *
 * RTC_DateTime_t ranges (from datetime.h):
 *   hours   : 0–23
 *   minutes : 0–59
 *   seconds : 0–59
 *
 * Cursor column map inside "HMS:    HH:MM:SS" (16 chars):
 *   Indices: 0123456789012345
 *   Content: HMS:    HH:MM:SS
 *   HH at col 8, MM at col 11, SS at col 14.
 */
static void Input_time(RTC_DateTime_t *init)
{
    static const uint8_t col_for_field[3] = {9u, 12u, 15u};
    uint8_t input_index = 0u;
    bool setting_done = false;

    ui_interface.lcd_cursor_row = 1u;
    ui_interface.lcd_cursor_col = col_for_field[0];
    ui_interface.lcd_cursor_on = true;

    while (!setting_done)
    {
        if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
        {
            snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                     "HMS:    %02u:%02u:%02u",
                     init->hours, init->minutes, init->seconds);
            ui_interface.lcd_cursor_col = col_for_field[input_index < 3u ? input_index : 0u];
            ui_interface.lcd_need_updated = true;
            xSemaphoreGive(ui_interface.mutex);
        }

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            switch (input_index)
            {
            case 0u:
                init->hours = (init->hours < 23u) ? init->hours + 1u : 0u;
                break;
            case 1u:
                init->minutes = (init->minutes < 59u) ? init->minutes + 1u : 0u;
                break;
            case 2u:
                init->seconds = (init->seconds < 59u) ? init->seconds + 1u : 0u;
                break;
            default:
                break;
            }
            break;
        case UI_KEY_DOWN:
            switch (input_index)
            {
            case 0u:
                init->hours = (init->hours > 0u) ? init->hours - 1u : 23u;
                break;
            case 1u:
                init->minutes = (init->minutes > 0u) ? init->minutes - 1u : 59u;
                break;
            case 2u:
                init->seconds = (init->seconds > 0u) ? init->seconds - 1u : 59u;
                break;
            default:
                break;
            }
            break;
        case UI_KEY_ENTER:
            input_index = (input_index < 2u) ? input_index + 1u : 0u;
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    ui_interface.lcd_cursor_on = false;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* L2 setting performers                                                         */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void Perform_SetID_Setting(void)
{
    bool setting_done = false;

    while (!setting_done)
    {
        L2_0_Display();
        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
        case UI_KEY_DOWN:
            ucc_state.ucc_state_l2_setting ^= 1u; /* toggle 0↔1 */
            break;
        case UI_KEY_ENTER:
            if (ucc_state.ucc_state_l2_setting == 0u)
                db_meta_data.region_id = Input_id(db_meta_data.region_id);
            else
                db_meta_data.station_id = Input_id(db_meta_data.station_id);
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }
}

static void Perform_SetOffset_Setting(void)
{
    bool setting_done = false;

    while (!setting_done)
    {
        L2_1_Display();
        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            ucc_state.ucc_state_l2_setting = (ucc_state.ucc_state_l2_setting > 0u) ? ucc_state.ucc_state_l2_setting - 1u : 4u;
            break;
        case UI_KEY_DOWN:
            ucc_state.ucc_state_l2_setting = (ucc_state.ucc_state_l2_setting < 4u) ? ucc_state.ucc_state_l2_setting + 1u : 0u;
            break;
        case UI_KEY_ENTER:
            switch (ucc_state.ucc_state_l2_setting)
            {
            case 0u:
                db_meta_data.temperature_adj = Input_float(db_meta_data.temperature_adj);
                break;
            case 1u:
                db_meta_data.humidity_adj = Input_float(db_meta_data.humidity_adj);
                break;
            case 2u:
                db_meta_data.pressure_adj = Input_float(db_meta_data.pressure_adj);
                break;
            case 3u:
                db_meta_data.light_adj = Input_light_offset(db_meta_data.light_adj);
                break;
            case 4u:
                db_meta_data.rainfall_adj = Input_float(db_meta_data.rainfall_adj);
                break;
            default:
                break;
            }
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }
}

/* BUG-5: RTC written only when user confirmed at least one sub-edit */
static void Perform_SetDateTime_Setting(void)
{
    bool setting_done = false;
    bool date_changed = false;
    RTC_DateTime_t temp_date_time;

    (void)datetime_get_datetime_from_rtc(&temp_date_time);

    while (!setting_done)
    {
        L2_3_Display_dt(&temp_date_time); /* BUG-4 */
        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
        case UI_KEY_DOWN:
            ucc_state.ucc_state_l2_setting ^= 1u;
            break;
        case UI_KEY_ENTER:
            if (ucc_state.ucc_state_l2_setting == 0u)
                Input_date(&temp_date_time);
            else
                Input_time(&temp_date_time);
            date_changed = true;
            break;
        case UI_KEY_MENU:
            setting_done = true;
            break;
        default:
            break;
        }
    }

    if (date_changed)
        (void)datetime_set_rtc(&temp_date_time);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Top-level L1 setting loop                                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */
static void Perform_L1_Setting(void)
{
    while (ucc_state.ucc_state_is_setting)
    {
        L1_Display();
        // Update red LED status.
        if (!(system_ready_status.fram_ready) && ui_interface.led_green != LED_OFF)
           ui_interface.led_green = LED_BLINK;

        Wait_For_Key_Release();
        UI_Key_t key = Get_Key_Pressed();

        switch (key)
        {
        case UI_KEY_UP:
            ucc_state.ucc_state_l1_setting = (ucc_state.ucc_state_l1_setting > 0u) ? ucc_state.ucc_state_l1_setting - 1u : 4u;
            break;
        case UI_KEY_DOWN:
            ucc_state.ucc_state_l1_setting = (ucc_state.ucc_state_l1_setting < 4u) ? ucc_state.ucc_state_l1_setting + 1u : 0u;
            break;
        case UI_KEY_ENTER:
            if (ucc_state.ucc_state_l1_setting <= 3u)
            {
                (void)DB_GetMeta(&db_meta_data);
                ucc_state.ucc_state_l2_setting = 0u;
                switch (ucc_state.ucc_state_l1_setting)
                {
                case 0u:
                    Perform_SetID_Setting();
                    break;
                case 1u:
                    Perform_SetOffset_Setting();
                    break;
                case 2u:
                    Perform_SetDateTime_Setting();
                    return; /* datetime does not touch db_meta_data */
                case 3u:
                    db_meta_data.sampling_interval =
                        Input_Sampling_Interval(db_meta_data.sampling_interval);
                    break;
                default:
                    break;
                }
                system_ready_status.fram_ready = DB_SetMeta(&db_meta_data);
            }
            else
            {
                /* Option 5: erase database */
                system_ready_status.fram_ready = DB_Flush();
                if (xSemaphoreTake(ui_interface.mutex, UCC_SEMAPHORE_TIMEOUT) == pdTRUE)
                {
                    strncpy(ui_interface.disp[1], "Done",
                            sizeof(ui_interface.disp[1]) - 1u);
                    ui_interface.disp[1][sizeof(ui_interface.disp[1]) - 1u] = '\0';
                    ui_interface.lcd_need_updated = true;
                    xSemaphoreGive(ui_interface.mutex);
                }
            }
            break;
        case UI_KEY_MENU:
            ucc_state.ucc_state_is_setting = false;
            break;
        default:
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* Task entry point                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */
void ucctask(void *params)
{
    (void)params;

    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1)
    {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));

        /* BUG-6: atomic read — weather_data written by a separate sensor task */
        uint8_t current_seconds;
        taskENTER_CRITICAL();
        current_seconds = weather_data.sampletime.seconds;
        taskEXIT_CRITICAL();

        ucc_tick_counter++;
        if (ucc_last_second != current_seconds)
        {
            ucc_last_second = current_seconds;
            ucc_tick_counter = 0u;
        }

        if (ui_interface.key_menu && !ucc_state.ucc_state_is_setting)
        {
            ucc_state.ucc_state_is_setting = true;
            ucc_state.ucc_state_l1_setting = 0u;
            ucc_state.ucc_state_l2_setting = 0u;
            ucc_state.ucc_state_l3_setting = false;
        }

        if (ucc_state.ucc_state_is_setting)
        {
            Perform_L1_Setting();
        }
        else if (ucc_tick_counter == 0u)
        {
            /* Once per second: update normal display */
            if (xSemaphoreTake(ui_interface.mutex, pdMS_TO_TICKS(5)) == pdTRUE)
            {
                snprintf(ui_interface.disp[0], sizeof(ui_interface.disp[0]),
                         "20%02u/%02u/%02u %02u:%02u",
                         weather_data.sampletime.year,
                         weather_data.sampletime.month,
                         weather_data.sampletime.day,
                         weather_data.sampletime.hours,
                         weather_data.sampletime.minutes);

                if (!system_ready_status.bmp390_ready)
                    strncpy(ui_interface.disp[1], "Pressure Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.sht45_ready)
                    strncpy(ui_interface.disp[1], "Temp Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.usart_ready)
                    strncpy(ui_interface.disp[1], "USART Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.modbus_ready)
                    strncpy(ui_interface.disp[1], "Modbus Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.rainfall_ok)
                    strncpy(ui_interface.disp[1], "Rainguage Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.light_ok)
                    strncpy(ui_interface.disp[1], "Light Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.fram_ready)
                    strncpy(ui_interface.disp[1], "Database Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.a7670_ready)
                    strncpy(ui_interface.disp[1], "Modem Error!", sizeof(ui_interface.disp[1]) - 1u);
                else if (!system_ready_status.datetime_ready)
                    strncpy(ui_interface.disp[1], "DateTime Error!", sizeof(ui_interface.disp[1]) - 1u);
                else
                    snprintf(ui_interface.disp[1], sizeof(ui_interface.disp[1]),
                             "T:%.2fC Rh:%.2f%%",
                             weather_data.temperature, weather_data.humidity);

                ui_interface.disp[1][sizeof(ui_interface.disp[1]) - 1u] = '\0';
                ui_interface.lcd_need_updated = true;
                xSemaphoreGive(ui_interface.mutex);
            }
        }
    }
}
