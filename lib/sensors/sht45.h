#ifndef __SHT40_H
#define __SHT40_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif

    bool sht45_init(I2C_HandleTypeDef *hi2c);
    bool sht45_get_sensor_data(float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif

#endif  /* __SHT40_H */