#ifndef SHT45_H
#define SHT45_H

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

#endif  /* SHT45_H */