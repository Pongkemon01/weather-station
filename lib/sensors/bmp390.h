#ifndef __BMP390_H
#define __BMP390_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif
    bool bmp390_init(I2C_HandleTypeDef *hi2c);
    bool bmp390_soft_reset(void);
    bool bmp390_get_sensor_data(float *temperature, float *pressure);
#ifdef __cplusplus
}
#endif

#endif  /* __BMP390_H */