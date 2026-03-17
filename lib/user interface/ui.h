#ifndef __UI_H
#define __UI_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif
    bool ui_init(I2C_HandleTypeDef *hi2c);

    /* push-button API */
    uint8_t ui_key_status(void);

    /* LED API */
    bool ui_led_set_value(uint8_t value);
    bool ui_led_red_on(void);
    bool ui_led_red_off(void);
    bool ui_led_green_on(void);
    bool ui_led_green_off(void);

    /* LCD API */
    bool ui_lcd_clear(void);
    bool ui_lcd_bk_on(void);
    bool ui_lcd_bk_off(void);
    bool ui_lcd_cursor_on(void);
    bool ui_lcd_cursor_off(void);
    bool ui_lcd_set_cursor(uint8_t col, uint8_t row);
    bool ui_lcd_putchar(char c);
    bool ui_lcd_print(char *str);
    bool ui_lcd_printXY(uint8_t col, uint8_t row, char *str);

#ifdef __cplusplus
}
#endif

#endif  /* __UI_H */
