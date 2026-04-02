#include "mcp23017.h"
#include "ui.h"

/*
 LCD module is connected to MCP23017 port A with these corresponding bits:
   - D7 - D4 : GPA3 - GPA0 (4-bit data bus)
   - E :       GPA4 (Operation Enable : Falling-edge triggered)
   - R/W :     GPA5 (Read or Write operation : 0 = Write, 1 = Read)
   - R/S :     GPA6 (Command or Data : 0 = Command, 1 = Data)
   - BK-ON :   GPA7 (Turn-on Back-light)
*/

/* LCD instructions */
#define LCD_INST_CLR_DISPLAY        0x01
#define LCD_INST_RETURN_HOME        0x02
#define LCD_INST_ENTRY_MODE         0x04
#define LCD_INST_DISPLAY_CTRL       0x08
#define LCD_INST_CURSOR_CTRL        0x10
#define LCD_INST_FUNCTION_SET       0x20
#define LCD_INST_SET_CGRAM_ADDR     0x40
#define LCD_INST_SET_DDRAM_ADDR     0x80

/* Configuration bit */
#define LCD_ENTRY_CURSOR_INCREMENT  0x02
#define LCD_ENTRY_CURSOR_DECREMENT  0x00
#define LCD_ENTRY_DISPLAY_SHIFT     0x01

#define LCD_DISPLAY_DISPLAY_ON      0x04
#define LCD_DISPLAY_DISPLAY_OFF     0x00
#define LCD_DISPLAY_CURSOR_ON       0x02
#define LCD_DISPLAY_CURSOR_OFF      0x00
#define LCD_DISPLAY_CURSOR_BLINK_ON 0x01
#define LCD_DISPLAY_CURSOR_BLINK_OFF 0x00

#define LCD_CURSOR_DISPLAY_SHIFT    0x08
#define LCD_CURSOR_SHIFT_LEFT       0x00
#define LCD_CURSOR_SHIFT_RIGHT      0x04

#define LCD_FUNCTION_8BIT_MODE      0x10
#define LCD_FUNCTION_4BIT_MODE      0x00
#define LCD_FUNCTION_2LINE          0x08
#define LCD_FUNCTION_1LINE          0x00
#define LCD_FUNCTION_5x10DOT        0x04
#define LCD_FUNCTION_5x8DOT         0x00

/* Private API */
static uint8_t current_bk_value = 0;       // Current back-light status
static bool ui_lcd_write_command(uint8_t cmd)
{
    uint8_t buff[4];
    uint8_t cmd_high, cmd_low;

    cmd_high = (cmd >> 4) & 0x0F;
    cmd_low = cmd & 0x0F;

    buff[0] = cmd_high | 0x10 | current_bk_value;        // Set E
    buff[1] = cmd_high | current_bk_value;               // Clear E
    buff[2] = cmd_low | 0x10 | current_bk_value;         // Set E
    buff[3] = cmd_low | current_bk_value;                // Clear E

    return(mcp23017_bitbanging_write_data(buff, 4));
}

static bool ui_lcd_write_data(uint8_t data)
{
    uint8_t buff[4];
    uint8_t data_high, data_low;

    data_high = (data >> 4) & 0x0F;
    data_low = data & 0x0F;

    buff[0] = data_high | 0x50 | current_bk_value;       // Set E, set RS
    buff[1] = data_high | current_bk_value;              // Clear E, set RS
    buff[2] = data_low | 0x50 | current_bk_value;        // Set E, set RS
    buff[3] = data_low | current_bk_value;               // Clear E, set RS

    return(mcp23017_bitbanging_write_data(buff, 4));
}

/**
 * @brief  This function provides a delay (in microseconds)
 * @param  microseconds: delay in microseconds
 */
static inline void DWT_Delay_us(volatile uint32_t microseconds)
{
  uint32_t clk_cycle_start = DWT->CYCCNT;

  /* Go to number of cycles for system */
  microseconds *= (HAL_RCC_GetHCLKFreq() / 1000000);

  /* Delay till end */
  while ((DWT->CYCCNT - clk_cycle_start) < microseconds);
}

/* Public API */
/* ----------------------------------------------------------- */
bool ui_init(I2C_HandleTypeDef *hi2c)
{
    if(!(mcp23017_init(hi2c)))
        return false;

    /* Init LED */
    if(!(ui_led_set_value(0x00)))
        return false;

    /* Init LCD */
    if(!(mcp23017_write_port_a(0x00)))
        return false;

    if(!(ui_lcd_write_command(0x30)))   // 0x30 means wake-up
        return false;
    DWT_Delay_us(160);
    if(!(ui_lcd_write_command(0x30)))
        return false;
    DWT_Delay_us(160);
    if(!(ui_lcd_write_command(0x30)))
        return false;
    DWT_Delay_us(160);

    /* Initialization sequence */
    if(!(ui_lcd_write_command(LCD_INST_FUNCTION_SET | LCD_FUNCTION_4BIT_MODE)))      // Initialize to 4-bit mode
        return false;
    if(!(ui_lcd_write_command(LCD_INST_FUNCTION_SET | LCD_FUNCTION_4BIT_MODE | LCD_FUNCTION_2LINE | LCD_FUNCTION_5x8DOT)))
        return false;
    if(!(ui_lcd_write_command(LCD_INST_CURSOR_CTRL | LCD_CURSOR_SHIFT_RIGHT)))
        return false;
    if(!(ui_lcd_write_command(LCD_INST_DISPLAY_CTRL | LCD_DISPLAY_DISPLAY_ON)))
        return false;
    if(!(ui_lcd_write_command(LCD_INST_ENTRY_MODE | LCD_ENTRY_CURSOR_INCREMENT)))
        return false;

    return (ui_lcd_clear());
}

/* push-button API */
uint8_t ui_key_status(void)
{
    return((mcp23017_read_port_b() & 0x0F));
}

/* LED API */
bool ui_led_set_value(uint8_t value)
{
    value &= 0xF0;      // Mask only LED bit
    return(mcp23017_write_port_b(value));
}

bool ui_led_red_on(void)
{
    uint8_t current_led;
    current_led = mcp23017_read_latch_b();
    return(ui_led_set_value(current_led | 0x80));
}

bool ui_led_red_off(void)
{
    uint8_t current_led;
    current_led = mcp23017_read_latch_b();
    return(ui_led_set_value(current_led & 0x7F));
}

bool ui_led_green_on(void)
{
    uint8_t current_led;
    current_led = mcp23017_read_latch_b();
    return(ui_led_set_value(current_led | 0x40));
}

bool ui_led_green_off(void)
{
    uint8_t current_led;
    current_led = mcp23017_read_latch_b();
    return(ui_led_set_value(current_led & 0xB0));
}

/* LCD API */
bool ui_lcd_clear(void)
{
    return(ui_lcd_write_command(LCD_INST_CLR_DISPLAY));
}

bool ui_lcd_bk_on(void)
{
    current_bk_value = 0x80;
    return(mcp23017_write_port_a(current_bk_value));
}

bool ui_lcd_bk_off(void)
{
    current_bk_value = 0x00;
    return(mcp23017_write_port_a(current_bk_value));
}

bool ui_lcd_cursor_on(void)
{
    return(ui_lcd_write_command(LCD_INST_DISPLAY_CTRL | LCD_DISPLAY_CURSOR_ON | LCD_DISPLAY_CURSOR_BLINK_ON | LCD_DISPLAY_DISPLAY_ON));
}

bool ui_lcd_cursor_off(void)
{
    return(ui_lcd_write_command(LCD_INST_DISPLAY_CTRL | LCD_DISPLAY_CURSOR_OFF | LCD_DISPLAY_CURSOR_BLINK_OFF | LCD_DISPLAY_DISPLAY_ON));
}

bool ui_lcd_set_cursor(uint8_t col, uint8_t row)
{
    uint8_t addr;

    if(col >= 16)
        return false;

    if(row == 0)
        addr = LCD_INST_SET_DDRAM_ADDR + col;
    else if(row == 1)
        addr = (LCD_INST_SET_DDRAM_ADDR | 0x40) + col;
    else
        return false;

    return(ui_lcd_write_command(addr));
}

bool ui_lcd_putchar(char c)
{
    return(ui_lcd_write_data(c));
}

bool ui_lcd_print(char *str)
{
    while(*str)
    {
        if(!(ui_lcd_putchar(*str)))
            return false;
        str++;
    }
    return true;
}

bool ui_lcd_printXY(uint8_t col, uint8_t row, char *str)
{
    if(!(ui_lcd_set_cursor(col, row)))
        return false;
    return(ui_lcd_print(str));
}
