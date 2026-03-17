#ifndef __Y2K_TIME_H__
#define __Y2K_TIME_H__

#include <stdint.h>

typedef struct
{
    uint8_t year;    // 0-99 (represents 2000-2099)
    uint8_t month;   // 1-12
    uint8_t day;     // 1-31
    uint8_t hours;   // 0-23
    uint8_t minutes; // 0-59
    uint8_t seconds; // 0-59
} RTC_DateTime_t;

#ifdef __cplusplus
extern "C"
{
#endif

    void get_datetime_from_epoch(uint32_t epoch, RTC_DateTime_t *dt);
    uint32_t get_epoch_from_datetime(RTC_DateTime_t *dt);

#ifdef __cplusplus
}
#endif

#endif // __Y2K_TIME_H__