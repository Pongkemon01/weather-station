#include "bmp390.h"

/* Default timeout for I2C */
#define I2C_TIMEOUT 500

/* BNP390 I2C Address */
#define BNP390_I2C_ADDR 0x76
#define BMP390_CHIP_ID 0x60

/* BMP390 Register Address */
#define BMP390_REG_CHIP_ID 0x00
#define BMP390_REG_REV_ID 0x01
#define BMP390_REG_ERR 0x02
#define BMP390_REG_SENS_STATUS 0x03
#define BMP390_REG_DATA 0x04
#define BMP390_REG_EVENT 0x10
#define BMP390_REG_INT_STATUS 0x11
#define BMP390_REG_FIFO_LENGTH 0x12
#define BMP390_REG_FIFO_DATA 0x14
#define BMP390_REG_FIFO_WTM_0 0x15
#define BMP390_REG_FIFO_WTM_1 0x16
#define BMP390_REG_FIFO_CONFIG_1 0x17
#define BMP390_REG_FIFO_CONFIG_2 0x18
#define BMP390_REG_INT_CTRL 0x19
#define BMP390_REG_IF_CONF 0x1A
#define BMP390_REG_PWR_CTRL 0x1B
#define BMP390_REG_OSR 0X1C
#define BMP390_REG_ODR 0x1D
#define BMP390_REG_CONFIG 0x1F
#define BMP390_REG_CALIB_DATA 0x31
#define BMP390_REG_CMD 0x7E

/* Register fields */
/* Error */
#define BMP390_ERR_FATAL 0x01
#define BMP390_ERR_CMD 0x02
#define BMP390_ERR_CONF 0x04

/* Status */
#define BMP390_CMD_RDY 0x10
#define BMP390_DRDY_PRESS 0x20
#define BMP390_DRDY_TEMP 0x40

/* Power mode */
#define BMP390_MODE_SLEEP 0x00
#define BMP390_MODE_FORCED 0x10
#define BMP390_MODE_NORMAL 0x30

/* Sensors control */
#define BMP390_SENSOR_ENABLE_PRESS 0x01
#define BMP390_SENSOR_DISABLE_PRESS 0x00
#define BMP390_SENSOR_ENABLE_TEMP 0x02
#define BMP390_SENSOR_DISABLE_TEMP 0x00

/* FIFO related macros */
/* FIFO CONFIG1  */
#define BMP390_FIFO_ENABLE 0x01
#define BMP390_FIFO_DISABLE 0x00
#define BMP390_FIFO_STOP_ON_FULL 0x02
#define BMP390_FIFO_ENABLE_TIME 0x04
#define BMP390_FIFO_ENABLE_PRESSURE 0x08
#define BMP390_FIFO_ENABLE_TEMP 0x10

/* FIFO CONFIG2 */
/* FIFO Sub-sampling */
#define BMP390_FIFO_NO_SUBSAMPLING 0x00
#define BMP390_FIFO_SUBSAMPLING_2X 0x01
#define BMP390_FIFO_SUBSAMPLING_4X 0x02
#define BMP390_FIFO_SUBSAMPLING_8X 0x03
#define BMP390_FIFO_SUBSAMPLING_16X 0x04
#define BMP390_FIFO_SUBSAMPLING_32X 0x05
#define BMP390_FIFO_SUBSAMPLING_64X 0x06
#define BMP390_FIFO_SUBSAMPLING_128X 0x07

/* Data source for FIFO */
#define BMP390_FIFO_USE_FILTERED 0x04
#define BMP390_FIFO_USE_RAW 0x00

/* Interrupt pin configuration macros */
/* Open drain */
#define BMP390_INT_PIN_OPEN_DRAIN 0x01
#define BMP390_INT_PIN_PUSH_PULL 0x00

/* Level */
#define BMP390_INT_PIN_ACTIVE_HIGH 0x02
#define BMP390_INT_PIN_ACTIVE_LOW 0x00

/* Latch */
#define BMP390_INT_PIN_LATCH 0x04
#define BMP390_INT_PIN_NON_LATCH 0x00

/* FIFO issues*/
#define BMP390_INT_FIFO_WATERMARK 0x08
#define BMP390_INT_FIFO_FULL 0x10

/* Data in FIFO is ready to read */
#define BMP390_INT_DATA_READY 0x40

/* Advance settings  */
/* I2C watch dog timer enable */
#define BMP390_I2C_WDT_ENABLE 0x02

/* I2C watch dog timer period selection */
#define BMP390_I2C_WDT_SHORT_1_25_MS 0x00
#define BMP390_I2C_WDT_LONG_40_MS 0x04

/* Over sampling macros */
#define BMP390_PRESS_OVERSAMPLING_1X 0x00
#define BMP390_PRESS_OVERSAMPLING_2X 0x01
#define BMP390_PRESS_OVERSAMPLING_4X 0x02
#define BMP390_PRESS_OVERSAMPLING_8X 0x03
#define BMP390_PRESS_OVERSAMPLING_16X 0x04
#define BMP390_PRESS_OVERSAMPLING_32X 0x05

#define BMP390_TEMP_OVERSAMPLING_1X 0x00
#define BMP390_TEMP_OVERSAMPLING_2X 0x08
#define BMP390_TEMP_OVERSAMPLING_4X 0x10
#define BMP390_TEMP_OVERSAMPLING_8X 0x18
#define BMP390_TEMP_OVERSAMPLING_16X 0x20
#define BMP390_TEMP_OVERSAMPLING_32X 0x28

/* Filter setting */
#define BMP390_IIR_FILTER_DISABLE 0x00
#define BMP390_IIR_FILTER_COEFF_1 0x02
#define BMP390_IIR_FILTER_COEFF_3 0x04
#define BMP390_IIR_FILTER_COEFF_7 0x06
#define BMP390_IIR_FILTER_COEFF_15 0x08
#define BMP390_IIR_FILTER_COEFF_31 0x0A
#define BMP390_IIR_FILTER_COEFF_63 0x0C
#define BMP390_IIR_FILTER_COEFF_127 0x0E

/* ODR (Output Data Rate) setting */
#define BMP390_ODR_200_HZ 0x00
#define BMP390_ODR_100_HZ 0x01
#define BMP390_ODR_50_HZ 0x02
#define BMP390_ODR_25_HZ 0x03
#define BMP390_ODR_12_5_HZ 0x04
#define BMP390_ODR_6_25_HZ 0x05
#define BMP390_ODR_3_1_HZ 0x06
#define BMP390_ODR_1_5_HZ 0x07
#define BMP390_ODR_0_78_HZ 0x08
#define BMP390_ODR_0_39_HZ 0x09
#define BMP390_ODR_0_2_HZ 0x0A
#define BMP390_ODR_0_1_HZ 0x0B
#define BMP390_ODR_0_05_HZ 0x0C
#define BMP390_ODR_0_02_HZ 0x0D
#define BMP390_ODR_0_01_HZ 0x0E
#define BMP390_ODR_0_006_HZ 0x0F
#define BMP390_ODR_0_003_HZ 0x10
#define BMP390_ODR_0_001_HZ 0x11

/* Command input to CMD */
/* Soft reset command */
#define BMP390_CMD_SOFT_RESET 0xB6
/* FIFO flush command */
#define BMP3_CMD_FIFO_FLUSH 0xB0

/* Register Trim Variables */
typedef struct
{
    uint16_t par_t1;
    uint16_t par_t2;
    int8_t par_t3;
    int16_t par_p1;
    int16_t par_p2;
    int8_t par_p3;
    int8_t par_p4;
    uint16_t par_p5;
    uint16_t par_p6;
    int8_t par_p7;
    int8_t par_p8;
    int16_t par_p9;
    int8_t par_p10;
    int8_t par_p11;
} Bmp390_Calib_Reg_t;

/* ----------------------------------------------------------- */
/* Global variables */
static I2C_HandleTypeDef *hbmp390;
static const uint16_t dev_addr = (uint16_t)(BNP390_I2C_ADDR << 1);

static struct
{
    /*! Quantized Trim Variables */

    float par_t1;
    float par_t2;
    float par_t3;
    float par_p1;
    float par_p2;
    float par_p3;
    float par_p4;
    float par_p5;
    float par_p6;
    float par_p7;
    float par_p8;
    float par_p9;
    float par_p10;
    float par_p11;
    float t_lin;
} bmp390_calib_data;

/* ---------------------------------------------------------------------- */
/* Static local functions */
static bool bmp390_get_reg(uint8_t reg_addr, uint8_t *reg_data, uint32_t len)
{
    if (hbmp390 == NULL)
        return false;

    /* Read raw data */
    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, &reg_addr, 1, I2C_TIMEOUT) != HAL_OK)
        return false;
    if (HAL_I2C_Master_Receive(hbmp390, dev_addr, reg_data, len, I2C_TIMEOUT) != HAL_OK)
        return false;

    return true;
}

/* ---------------------------------------------------------------- */
static bool bmp390_get_calib_data(void)
{
    static Bmp390_Calib_Reg_t bmp390_raw_calib_data;

    /* Read calibration data from device */
    if (!(bmp390_get_reg(BMP390_REG_CALIB_DATA, (uint8_t *)&bmp390_raw_calib_data, sizeof(bmp390_raw_calib_data))))
        return false;

    /*Transform raw data to actual coefficient data (formulas from datasheet)*/
    bmp390_calib_data.par_t1 = (float)(bmp390_raw_calib_data.par_t1) * 256.0f;             // par_t1 / (2^-8) = par_1 * (2^8)
    bmp390_calib_data.par_t2 = (float)(bmp390_raw_calib_data.par_t2) / 1073741824.0f;      // par_2 / (2^30)
    bmp390_calib_data.par_t3 = (float)(bmp390_raw_calib_data.par_t3) / 281474976710656.0f; // par_3 / (2^48)

    bmp390_calib_data.par_p1 = ((float)(bmp390_raw_calib_data.par_p1) - 16384.0f) / 1048576.0f;   // (par_p1 - 2^14)/ (2^20)
    bmp390_calib_data.par_p2 = ((float)(bmp390_raw_calib_data.par_p2) - 16384.0f) / 536870912.0f; // (par_p2 - 2^14)/ (2^29)
    bmp390_calib_data.par_p3 = (float)(bmp390_raw_calib_data.par_p3) / 4294967296.0f;             // par_p3 / (2^32)
    bmp390_calib_data.par_p4 = (float)(bmp390_raw_calib_data.par_p4) / 137438953472.0f;           // par_p4 / (2^37)
    bmp390_calib_data.par_p5 = (float)(bmp390_raw_calib_data.par_p5) * 8.0f;                      // par_p5 / (2^-3) = par_p5 * (2^3)
    bmp390_calib_data.par_p6 = (float)(bmp390_raw_calib_data.par_p6) / 64.0f;                     // par_p6 / (2^6)
    bmp390_calib_data.par_p7 = (float)(bmp390_raw_calib_data.par_p7) / 156.0f;                    // par_p7 / (2^8)
    bmp390_calib_data.par_p8 = (float)(bmp390_raw_calib_data.par_p8) / 32768.0f;                  // par_p8 / (2^15)
    bmp390_calib_data.par_p9 = (float)(bmp390_raw_calib_data.par_p9) / 281474976710656.0f;        // par_p9 / (2^48)
    bmp390_calib_data.par_p10 = (float)(bmp390_raw_calib_data.par_p10) / 281474976710656.0f;      // par_p10 / (2^48)
    bmp390_calib_data.par_p11 = (float)(bmp390_raw_calib_data.par_p11) / 36893488147419103232.0f; // par_p11 / (2^65)

    return true;
}

/* ---------------------------------------------------------------- */
static float bmp390_compensate_temperature(uint32_t adc_temp)
{
    float partial_data1, partial_data2;

    partial_data1 = (float)adc_temp - bmp390_calib_data.par_t1;
    partial_data2 = partial_data1 * bmp390_calib_data.par_t2;

    bmp390_calib_data.t_lin = partial_data2 + ((partial_data1 * partial_data1) * bmp390_calib_data.par_t3);

    return bmp390_calib_data.t_lin;
}

/* ---------------------------------------------------------------- */
static float bmp390_compensate_pressure(uint32_t adc_press)
{
    /* Variable to store adc_press in floating-point format */
    float raw_press;

    /* Variable to store the compensated pressure */
    float comp_press;

    /* Temporary variables used for compensation */
    float partial_data1;
    float partial_data2;
    float partial_data3;
    float partial_data4;
    float partial_out1;
    float partial_out2;

    raw_press = (float)adc_press;

    partial_data1 = bmp390_calib_data.par_p6 * bmp390_calib_data.t_lin;
    partial_data2 = bmp390_calib_data.par_p7 * (bmp390_calib_data.t_lin * bmp390_calib_data.t_lin);
    partial_data3 = bmp390_calib_data.par_p8 * (bmp390_calib_data.t_lin * bmp390_calib_data.t_lin * bmp390_calib_data.t_lin);
    partial_out1 = bmp390_calib_data.par_p5 + partial_data1 + partial_data2 + partial_data3;

    partial_data1 = bmp390_calib_data.par_p2 * bmp390_calib_data.t_lin;
    partial_data2 = bmp390_calib_data.par_p3 * (bmp390_calib_data.t_lin * bmp390_calib_data.t_lin);
    partial_data3 = bmp390_calib_data.par_p4 * (bmp390_calib_data.t_lin * bmp390_calib_data.t_lin * bmp390_calib_data.t_lin);
    partial_out2 = raw_press * (bmp390_calib_data.par_p1 + partial_data1 + partial_data2 + partial_data3);

    partial_data1 = raw_press * raw_press;
    partial_data2 = bmp390_calib_data.par_p9 + bmp390_calib_data.par_p10 * bmp390_calib_data.t_lin;
    partial_data3 = partial_data1 * partial_data2;
    partial_data4 = partial_data3 + (raw_press * raw_press * raw_press) * bmp390_calib_data.par_p11;
    comp_press = partial_out1 + partial_out2 + partial_data4;

    return comp_press;
}

/* ---------------------------------------------------------------- */
/* Public API */
bool bmp390_init(I2C_HandleTypeDef *hi2c)
{
    uint8_t buff[2];
    hbmp390 = hi2c;

    /* Verify the device */
    if (!(bmp390_get_reg(BMP390_REG_CHIP_ID, buff, 1)))
    {
        hbmp390 = NULL;
        return false;
    }
    if (buff[0] != BMP390_CHIP_ID)
    {
        hbmp390 = NULL;
        return false;
    }

    /* Reset the sensor */
    if (!(bmp390_soft_reset()))
    {
        hbmp390 = NULL;
        return false;
    }

    /* Mode for this project :
        - Mode = Normal
        - Oversampling = High-Resolution
           - osr_p = x8 (pressure)
           - osr_t = x1 (temperature)
        - ODR (Output Data Rate) = 12.5 Hz
        - IIR Coefficent = 3
        - FIFO = Disable
    */

    /* Set FIFO_CONFIG_1 */
    buff[0] = BMP390_REG_FIFO_CONFIG_1;
    buff[1] = BMP390_FIFO_DISABLE;
    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, buff, 2, I2C_TIMEOUT) != HAL_OK)
    {
        hbmp390 = NULL;
        return false;
    }

    /* Set PWR_CTRL */
    buff[0] = BMP390_REG_PWR_CTRL;
    buff[1] = BMP390_MODE_NORMAL | BMP390_SENSOR_ENABLE_PRESS | BMP390_SENSOR_ENABLE_TEMP;
    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, buff, 2, I2C_TIMEOUT) != HAL_OK)
    {
        hbmp390 = NULL;
        return false;
    }

    /* Set OSR */
    buff[0] = BMP390_REG_OSR;
    buff[1] = BMP390_PRESS_OVERSAMPLING_8X | BMP390_TEMP_OVERSAMPLING_1X;
    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, buff, 2, I2C_TIMEOUT) != HAL_OK)
    {
        hbmp390 = NULL;
        return false;
    }

    /* Set ODR */
    buff[0] = BMP390_REG_ODR;
    buff[1] = BMP390_ODR_12_5_HZ;
    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, buff, 2, I2C_TIMEOUT) != HAL_OK)
    {
        hbmp390 = NULL;
        return false;
    }

    /* Set IIR Coefficient */
    buff[0] = BMP390_REG_CONFIG;
    buff[1] = BMP390_IIR_FILTER_COEFF_3;
    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, buff, 2, I2C_TIMEOUT) != HAL_OK)
    {
        hbmp390 = NULL;
        return false;
    }

    /* Get calibration data */
    if (!(bmp390_get_calib_data()))
    {
        hbmp390 = NULL;
        return false;
    }

    return true;
}

/* ---------------------------------------------------------------- */
bool bmp390_soft_reset(void)
{
    static uint8_t rst_cmd[2] = {BMP390_REG_CMD, BMP390_CMD_SOFT_RESET};

    if (hbmp390 == NULL)
        return false;

    if (HAL_I2C_Master_Transmit(hbmp390, dev_addr, rst_cmd, 2, I2C_TIMEOUT) != HAL_OK)
        return false;

    return true;
}

/* ---------------------------------------------------------------- */
bool bmp390_get_sensor_data(float *temperature, float *pressure)
{
    uint8_t buf[6];
    uint32_t adc_temp;
    uint32_t adc_press;

    /* Read raw data */
    if (!(bmp390_get_reg(BMP390_REG_DATA, buf, 6)))
    {
        *temperature = 0.0f;
        *pressure = 0.0f;

        return false;
    }

    /* Parse data */
    adc_press = (uint32_t)((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | (uint32_t)buf[2];
    adc_temp = (uint32_t)((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | (uint32_t)buf[5];

    /* Compensate data */
    *temperature = bmp390_compensate_temperature(adc_temp);
    *pressure = bmp390_compensate_pressure(adc_press);

    return true;
}
