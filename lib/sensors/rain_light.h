#ifndef __RAIN_LIGHT_H
#define __RAIN_LIGHT_H

#include <stdint.h>
#include <stdbool.h>
#include "modbus.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ─────────────────────────── Slave addresses ────────────────────────────── */

#define ADDR_SEM228P 0x01u /**< Light sensor  */
#define ADDR_R66S 0x02u    /**< Rain gauge    */

    /* ─────────────────────────── Public API ─────────────────────────────────── */
    bool get_light(uint16_t *umol);
    bool get_rain(float *mmhr);

#ifdef __cplusplus
}
#endif

#endif //__RAIN_LIGHT_H