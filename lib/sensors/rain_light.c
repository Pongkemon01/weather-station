#include "rain_light.h"

bool get_light(uint16_t *umol)
{
    *umol = 0;

    if (modbus_read_register(ADDR_SEM228P, 0x0000, umol, 1))
        return true;
    return false;
}

bool get_rain(float *mmhr)
{
    *mmhr = 0.0f;

    {
        uint16_t raw;

        if (modbus_read_register(ADDR_R66S, 0x0000, &raw, 1))
        {
            *mmhr = (float)raw / 10.0f;      // Sensor scales the raw data by 10

            // Clear accum data
            if(modbus_write_register(ADDR_R66S, 0x0000, 0x005A))
                return true;
        }
    }
    return false;
}
