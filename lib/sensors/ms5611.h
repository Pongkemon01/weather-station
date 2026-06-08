#ifndef MS5611_H
#define MS5611_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif
    bool ms5611_init(I2C_HandleTypeDef *hi2c);
    bool ms5611_soft_reset(void);
    bool ms5611_get_sensor_data(float *temperature, float *pressure); /* Temperature in °C, Pressure in Pa */
    bool ms5611_ping(I2C_HandleTypeDef *hi2c);
#ifdef __cplusplus
}
#endif

#endif /* MS5611_H */