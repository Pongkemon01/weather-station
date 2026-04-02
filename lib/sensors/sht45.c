#include "sht45.h"

/* Default timeout for I2C */
#define I2C_TIMEOUT                 500

/* SHT45 I2C Address */
#define SHT45_I2C_ADDR             0x44

/* SHT45 Register Address */
#define SHT45_REG_DATA              0xFD
#define SHT45_REG_SOFT_RESET        0x94

/* ----------------------------------------------------------- */
/* Global variables */
static I2C_HandleTypeDef *hsht45;
static const uint16_t dev_addr = (uint16_t)(SHT45_I2C_ADDR << 1);

/* ---------------------------------------------------------------------- */
/* Static local functions */
static bool sht45_get_reg(uint8_t reg_addr, uint8_t *reg_data, uint32_t len)
{
    if (hsht45 == NULL)
        return false;

    /* Read raw data */
    if (HAL_I2C_Master_Transmit(hsht45, dev_addr, &reg_addr, 1, I2C_TIMEOUT) != HAL_OK)
        return false;
    if (HAL_I2C_Master_Receive(hsht45, dev_addr, reg_data, len, I2C_TIMEOUT) != HAL_OK)
        return false;

    return true;
}

/* ---------------------------------------------------------------- */
// Calculates CRC-8 for a 16-bit input
// Polynomial: 0x31 (x^8 + x^5 + x^4 + 1)
// Initial value: 0xFF
// Final XOR: 0x00
static uint8_t sht45_crc8_16bit_calc(uint16_t data)
{
    uint8_t crc = 0xFF; // Initial value

    // Process High Byte (MSB)
    crc ^= (uint8_t)(data >> 8);
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }

    // Process Low Byte (LSB)
    crc ^= (uint8_t)(data & 0xFF);
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }

    return crc; // Final XOR is 0x00, so return as-is
}

/* ---------------------------------------------------------------- */
/* Public API */
bool sht45_init(I2C_HandleTypeDef *hi2c)
{
    uint8_t buff;
    hsht45 = hi2c;

    /* Reset the sensor */
    buff = SHT45_REG_SOFT_RESET;
    if(!(HAL_I2C_Master_Transmit(hsht45, dev_addr, &buff, 1, I2C_TIMEOUT)))
    {
        hsht45 = NULL;
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------- */
bool sht45_get_sensor_data(float *temperature, float *humidity)
{
    uint8_t buf[6];
    uint16_t adc_data;

    *temperature = 0.0f;
    *humidity = 0.0f;

    /* Read raw data */
    if (!(sht45_get_reg(SHT45_REG_DATA, buf, 6)))
        return false;

    /* Parse data */
    adc_data = (uint16_t)((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    if (sht45_crc8_16bit_calc(adc_data) != buf[2])
        return false;
    *humidity = ((float)adc_data) * 0.001907377737f - 6.0f;

    adc_data = (uint16_t)((uint16_t)buf[3] << 8) | (uint16_t)buf[4];
    if (sht45_crc8_16bit_calc(adc_data) != buf[5])
        return false;
    *temperature = ((float)adc_data) * 0.002670328832f - 45.0f;

    return true;
}
