#ifndef __Y2K_TIME_H
#define __Y2K_TIME_H

#include <stdint.h>
#include "datetime.h"

#ifdef __cplusplus
extern "C"
{
#endif

    void get_datetime_from_epoch(uint32_t epoch, RTC_DateTime_t *dt);
    uint32_t get_epoch_from_datetime(const RTC_DateTime_t *dt);

#ifdef __cplusplus
}
#endif

#endif // __Y2K_TIME_H