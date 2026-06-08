#include "ms5611.h"
#include "main.h"

/* Default timeout for I2C */
#define I2C_TIMEOUT 500

/* MS5611 I2C Address */
#define MS5611_I2C_ADDR 0x77

/* MS5611 Commands */
#define MS5611_CMD_RESET 0x1E
#define MS5611_CMD_CONV_D1_OSR_256 0x40
#define MS5611_CMD_CONV_D1_OSR_512 0x42
#define MS5611_CMD_CONV_D1_OSR_1024 0x44
#define MS5611_CMD_CONV_D1_OSR_2048 0x46
#define MS5611_CMD_CONV_D1_OSR_4096 0x48
#define MS5611_CMD_CONV_D2_OSR_256 0x50
#define MS5611_CMD_CONV_D2_OSR_512 0x52
#define MS5611_CMD_CONV_D2_OSR_1024 0x54
#define MS5611_CMD_CONV_D2_OSR_2048 0x56
#define MS5611_CMD_CONV_D2_OSR_4096 0x58
#define MS5611_CMD_ADC_READ 0x00
#define MS5611_CMD_PROM_READ 0xA0

/* ---------------------------------------------------------------------- */
/* Global variables */
static I2C_HandleTypeDef *hms5611;
static const uint16_t dev_addr = (uint16_t)(MS5611_I2C_ADDR << 1);
static uint16_t calib_data[8];

/* Coefficients for pressure and temperature calculations */
static float SENS_T1;
static float OFF_T1;
static float TCS;
static float TCO;
static float T_REF;
static float TEMPSENS;

/* ---------------------------------------------------------------------- */
/* Static local functions */
static uint8_t ms5611_crc4(uint16_t *n_prom)
{
    uint16_t n_rem = 0;
    uint16_t crc_read = n_prom[7]; // CRC from PROM is stored in the lower 4 bits of the last coefficient
    int n_bit;

    n_prom[7] &= 0xFF00; // Clear the CRC byte for calculation

    for (int i = 0; i < 16; i++)
    {
        if (i % 2 == 1)
            n_rem ^= (n_prom[i >> 1] & 0x00FF);
        else
            n_rem ^= (n_prom[i >> 1] >> 8);

        for (n_bit = 8; n_bit > 0; n_bit--)
        {
            if (n_rem & 0x8000)
                n_rem = (n_rem << 1) ^ 0x3000;
            else
                n_rem <<= 1;
        }
    }

    n_rem = (n_rem >> 12) & 0xF; // Finalize the CRC value
    n_prom[7] = crc_read;        // Restore the original CRC byte

    return (n_rem); // Return the calculated CRC
}

/* ---------------------------------------------------------------- */
static bool ms5611_get_calib_data(void)
{
    uint8_t buff[2];

    if (hms5611 == NULL)
        return false;

    for (int i = 0; i < 8; i++)
    {
        buff[0] = MS5611_CMD_PROM_READ + (i << 1); // Each calibration coefficient is 2 bytes, so we multiply index by 2
        if (HAL_I2C_Master_Transmit(hms5611, dev_addr, buff, 1, I2C_TIMEOUT) != HAL_OK)
            return false;
        if (HAL_I2C_Master_Receive(hms5611, dev_addr, buff, 2, I2C_TIMEOUT) != HAL_OK)
            return false;

        calib_data[i] = (buff[0] << 8) | buff[1];
    }

    /* Verify CRC */
    if (ms5611_crc4(calib_data) != (calib_data[7] & 0xF))
        return false;

    return true;
}

/* ---------------------------------------------------------------- */
static void ms5611_calculate_coefficients(void)
{
    SENS_T1 = (float)(calib_data[1]) * 32768.0f;    // C1 * 2^15
    OFF_T1 = (float)(calib_data[2]) * 65536.0f;     // C2 * 2^16
    TCS = (float)(calib_data[3]) / 256.0f;          // C3 / 2^8
    TCO = (float)(calib_data[4]) / 128.0f;          // C4 / 2^7
    T_REF = (float)(calib_data[5]) * 256.0f;        // C5 * 2^8
    TEMPSENS = (float)(calib_data[6]) / 8388608.0f; // C6 / 2^23
}

/* ---------------------------------------------------------------- */
static uint32_t ms5611_read_adc(void)
{
    uint8_t buff[3];
    uint32_t adc_value;

    if (hms5611 == NULL)
        return 0;

    if (HAL_I2C_Master_Transmit(hms5611, dev_addr, (uint8_t[]){MS5611_CMD_ADC_READ}, 1, I2C_TIMEOUT) != HAL_OK)
        return 0;
    if (HAL_I2C_Master_Receive(hms5611, dev_addr, buff, 3, I2C_TIMEOUT) != HAL_OK)
        return 0;

    adc_value = ((uint32_t)(buff[0]) << 16) | ((uint32_t)(buff[1]) << 8) | (uint32_t)(buff[2]);

    return adc_value;
}

/* ---------------------------------------------------------------- */
/* Global functions */
bool ms5611_init(I2C_HandleTypeDef *hi2c)
{
    hms5611 = hi2c;

    /* Check if device is responding */
    if (ms5611_ping(hi2c) == false)
    {
        hms5611 = NULL;
        return false;
    }

    /* Perform a soft reset */
    if (ms5611_soft_reset() == false)
    {
        hms5611 = NULL;
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------- */
bool ms5611_soft_reset(void)
{
    uint8_t cmd = MS5611_CMD_RESET;

    if (hms5611 == NULL)
        return false;

    if (HAL_I2C_Master_Transmit(hms5611, dev_addr, &cmd, 1, I2C_TIMEOUT) != HAL_OK)
    {
        hms5611 = NULL;
        return false;
    }

    /* Wait for the reset to complete */
    my_delay(3);

    /* Get calibration data */
    if (!(ms5611_get_calib_data()))
    {
        hms5611 = NULL;
        return false;
    }

    /* Calculate coefficients for pressure and temperature compensation */
    ms5611_calculate_coefficients();

    return true;
}

/* ---------------------------------------------------------------- */
bool ms5611_ping(I2C_HandleTypeDef *hi2c)
{
    uint8_t cmd = MS5611_CMD_ADC_READ;
    uint8_t buff[3];

    if (hi2c == NULL)
        return false;

    if (HAL_I2C_Master_Transmit(hi2c, dev_addr, &cmd, 1, I2C_TIMEOUT) != HAL_OK)
        return false;

    if (HAL_I2C_Master_Receive(hi2c, dev_addr, buff, 3, I2C_TIMEOUT) != HAL_OK)
        return false;

    return true;
}

/* ---------------------------------------------------------------- */
bool ms5611_get_sensor_data(float *temperature, float *pressure)
{
    uint32_t adc_temp;
    uint32_t adc_press;

    float dT, OFF, SENS, TEMP;

    if (temperature == NULL || pressure == NULL)
        return false;

    *temperature = 0.0f;
    *pressure = 0.0f;
    if (hms5611 == NULL)
        return false;

    /* Read raw temperature and pressure data */
    if (HAL_I2C_Master_Transmit(hms5611, dev_addr, (uint8_t[]){MS5611_CMD_CONV_D2_OSR_4096}, 1, I2C_TIMEOUT) != HAL_OK)
        return false;
    my_delay(10); // Max conversion time for OSR 4096 is 9.04 ms
    adc_temp = ms5611_read_adc();

    if (HAL_I2C_Master_Transmit(hms5611, dev_addr, (uint8_t[]){MS5611_CMD_CONV_D1_OSR_4096}, 1, I2C_TIMEOUT) != HAL_OK)
        return false;
    my_delay(10); // Max conversion time for OSR 4096 is 9.04 ms
    adc_press = ms5611_read_adc();

    /* Calculate temperature */
    dT = adc_temp - T_REF;
    TEMP = 2000.0f + dT * TEMPSENS; /* 0.01 °C resolution */

    /* Calculate pressure */
    OFF = OFF_T1 + TCO * dT;
    SENS = SENS_T1 + TCS * dT;
    *pressure = (adc_press * SENS / 2097152.0f - OFF) / 32768.0f; /* 0.01 mBar resolution (already in Pa, 1 mBar = 100 Pa) */

    /* Calculate compensated temperature */
    *temperature = TEMP / 100.0f; // Convert to °C

    return true;
}
