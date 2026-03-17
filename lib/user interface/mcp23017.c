#include "mcp23017.h"

/* Default timeout for I2C */
#define I2C_TIMEOUT                 500

/* SHT45 I2C Address */
#define MCP23017_I2C_ADDR           0x20

/* SHT45 Register Address (for IOCON.BANK = 0)*/
#define MCP23017_REG_IODIRA         0x00
#define MCP23017_REG_IODIRB         0x01
#define MCP23017_REG_IPOLA          0x02
#define MCP23017_REG_IPOLB          0x03
#define MCP23017_REG_GPINTENA       0x04
#define MCP23017_REG_GPINTENB       0x05
#define MCP23017_REG_DEFVALA        0x06
#define MCP23017_REG_DEFVALB        0x07
#define MCP23017_REG_INTCONA        0x08
#define MCP23017_REG_INTCONB        0x09
#define MCP23017_REG_IOCON          0x0A
#define MCP23017_REG_GPPUA          0x0C
#define MCP23017_REG_GPPUB          0x0D
#define MCP23017_REG_INTFA          0x0E
#define MCP23017_REG_INTFB          0x0F
#define MCP23017_REG_INTCAPA        0x10
#define MCP23017_REG_INTCAPB        0x11
#define MCP23017_REG_GPIOA          0x12
#define MCP23017_REG_GPIOB          0x13
#define MCP23017_REG_OLATA          0x14
#define MCP23017_REG_OLATB          0x15

/* ----------------------------------------------------------- */
/* Global variables */
static I2C_HandleTypeDef *hmcp23017;
static const uint16_t dev_addr = (uint16_t)(SHT45_I2C_ADDR << 1);

/* ---------------------------------------------------------------------- */
/* Static local functions */
static bool mcp23017_get_reg(uint8_t reg_addr, uint8_t *reg_data, uint32_t len)
{
    if (hmcp23017 == NULL)
        return false;

    /* Read raw data */
    if (HAL_I2C_Master_Transmit(hmcp23017, dev_addr, &reg_addr, 1, I2C_TIMEOUT) != HAL_OK)
        return false;
    if (HAL_I2C_Master_Receive(hmcp23017, dev_addr, reg_data, len, I2C_TIMEOUT) != HAL_OK)
        return false;
    
    return true;
}

/* ---------------------------------------------------------------- */
/* Public API */
bool mcp23017_init(I2C_HandleTypeDef *hi2c)
{
    uint8_t buff[5];
    hmcp23017 = hi2c;

    /* Set port control registers */
    buff[0] = MCP23017_REG_IODIRA;
    buff[1] = 0x00;     // IODIRA = 0x00 (All are output)
    buff[2] = 0x0F;     // IODIRB = 0x0F (Bit 0 - 3 are input, bit 4 - 7 are output)
    buff[3] = 0x00;     // IPOLA = 0x00 (No polarity inversion)
    buff[4] = 0x0F;     // IPOLB = 0x0F (Bit 0 - 3 are inverted from the actual state)
    if(!(HAL_I2C_Master_Transmit(hmcp23017, dev_addr, buff, 5, I2C_TIMEOUT)))
    {
        hmcp23017 = NULL;
        return false;
    }

    /* Set pull-up control */
    buff[0] = MCP23017_REG_GPPUA;
    buff[1] = 0x00;     // GPPUA = 0x00 (No pull-up)
    buff[2] = 0x0F;     // GPPUB = 0x0F (Pull-up on all input pins)
    if(!(HAL_I2C_Master_Transmit(hmcp23017, dev_addr, buff, 3, I2C_TIMEOUT)))
    {
        hmcp23017 = NULL;
        return false;
    }

    /* Disable address auto-incrementing */
    buff[0] = MCP23017_REG_IOCON;
    buff[1] = 0x02;     // IOCON.BANK = 0
    if(!(HAL_I2C_Master_Transmit(hmcp23017, dev_addr, buff, 2, I2C_TIMEOUT)))
    {
        hmcp23017 = NULL;
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------- */
bool mcp23017_bitbanging_write_data(uint8_t *data, uint16_t len)
{
    if(hmcp23017 == NULL)
        return false;
    
    if(!(HAL_I2C_Master_Transmit(hmcp23017, dev_addr, data, len, I2C_TIMEOUT)))
        return false;
    
    return true;
}

/* ---------------------------------------------------------------- */
bool mcp23017_write_port_a(uint8_t data)
{
    uint8_t buff[2];

    if(hmcp23017 == NULL)
        return false;
    
    buff[0] = MCP23017_REG_GPIOA;
    buff[1] = data;
    if(!(HAL_I2C_Master_Transmit(hmcp23017, dev_addr, buff, 2, I2C_TIMEOUT)))
        return false;
    
    return true;
}

/* ---------------------------------------------------------------- */
bool mcp23017_write_port_b(uint8_t data)
{
    uint8_t buff[2];

    if(hmcp23017 == NULL)
        return false;
    
    buff[0] = MCP23017_REG_GPIOB;
    buff[1] = data;
    if(!(HAL_I2C_Master_Transmit(hmcp23017, dev_addr, buff, 2, I2C_TIMEOUT)))
        return false;
    
    return true;
}

/* ---------------------------------------------------------------- */
uint8_t mcp23017_read_port_a(void)
{
    uint8_t ret;

    if(hmcp23017 == NULL)
        return 0;
    
    if(!(mcp23017_get_reg(MCP23017_REG_GPIOA, &ret, 1)))
        return 0;
    
    return ret;
}

/* ---------------------------------------------------------------- */
uint8_t mcp23017_read_port_b(void)
{
    uint8_t ret;

    if(hmcp23017 == NULL)
        return 0;
    
    if(!(mcp23017_get_reg(MCP23017_REG_GPIOB, &ret, 1)))
        return 0;
    
    return ret;
}
