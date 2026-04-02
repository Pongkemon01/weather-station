#ifndef __DATETIME_H
#define __DATETIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ─────────────────────────── Tuning ─────────────────────────────────────── */

/** Earliest plausible 2-digit year (hardware cannot predate 2024). */
#define DATETIME_YEAR_MIN 24u

/** Far-future sanity cap. */
#define DATETIME_YEAR_MAX 30u

    /* ─────────────────────────── Result codes ───────────────────────────────── */

    typedef enum
    {
        TIME_SOURCE_NONE = 0,       /**< No plausible source — do not trust time  */
        TIME_SOURCE_RTC_ONLY = 1,   /**< RTC valid; modem unavailable             */
        TIME_SOURCE_MODEM_ONLY = 2, /**< Modem valid; RTC was stale — now updated */
        TIME_SOURCE_BOTH = 3,       /**< Both valid; RTC disciplined from modem   */
    } TimeSourceResult_t;

    typedef struct
    {
        uint8_t year;    // 0-99 (represents 2000-2099)
        uint8_t month;   // 1-12
        uint8_t day;     // 1-31
        uint8_t hours;   // 0-23
        uint8_t minutes; // 0-59
        uint8_t seconds; // 0-59
    } RTC_DateTime_t;

    /* datetime string format is "yy/MM/dd,hh:mm:ss±zz" (20 chars), where
     * characters indicate year (two last digits), month, day, hour, minutes,
     * seconds and time zone (indicates the difference, expressed in
     * quarters of an hour, between the local time and GMT); three last digits
     * are mandatory, range (-96 to 96). E.g. 6th of May 2008, 14:28:10
     * GMT+7 equals to "08/05/06,14:28:10+28"
     *
     * For this system, time zone is fixed to 28 (GMT+7).
     **/
    bool datetime_from_string(RTC_DateTime_t *dt, const char *str); // str format must follow the above description
    bool datetime_to_string(const RTC_DateTime_t *dt, char *str);   // str must have space at least 21 bytes

    /*
     * Returns day of week for a date in years 2000–2099.
     *
     *   yy    : 2-digit year, 0–99  (uint8_t)
     *   month : 1=Jan..12=Dec       (uint8_t)
     *   day   : 1–31                (uint8_t)
     *
     * Returns: 0=Sun 1=Mon 2=Tue 3=Wed 4=Thu 5=Fri 6=Sat  (uint8_t)
     *
     * Anchor: 1-Jan-2000 = Saturday (6)
     */
    uint8_t datetime_day_of_week(uint8_t dd, uint8_t mm, uint8_t yy);

    bool datetime_set_rtc(const RTC_DateTime_t *dt);
    bool datetime_get_datetime_from_rtc(RTC_DateTime_t *dt);
    bool datetime_sync_rtc_from_modem(void);
    bool datetime_sync_modem_from_rtc(void);

    /* ─────────────────────────── Public API ─────────────────────────────────── */

    /**
     * @brief  Check whether all fields of a datetime are within valid ranges.
     * @param  dt   Struct to validate (NULL → false).
     * @return true if plausible, false otherwise.
     */
    bool datetime_is_plausible(const RTC_DateTime_t *dt);

    /**
     * @brief  Select the best time source and sync the stale one.
     *
     * Fffects:
     *   - Modem valid  → MCU RTC is updated via datetime_set_rtc().
     *   - RTC valid only → modem clock is updated via modem_set_datetime().
     *
     * @return TimeSourceResult_t.
     */
    TimeSourceResult_t datetime_sync_with_best_source(void);

#ifdef __cplusplus
}
#endif

#endif // __DATETIME_H
