#include "datetime.h"
#include "y2k_time.h"
#include "rtc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "a7670.h"

/* ─────────────────────────── Public API ─────────────────────────────────── */

/* datetime string format is "yy/MM/dd,hh:mm:ss±zz" (20 chars), where
 * characters indicate year (two last digits), month, day, hour, minutes,
 * seconds and time zone (indicates the difference, expressed in
 * quarters of an hour, between the local time and GMT; three last digits
 * are mandatory, range (-96 to 96). E.g. 6th of May 2008, 14:28:10
 * GMT+7 equals to "08/05/06,14:28:10+28"
 *
 * For this system, time zone is fixed to 28 (GMT+7).
 **/
bool datetime_from_string(RTC_DateTime_t *dt, const char *str)
{
    char tz_sign;
    int tz_offset, year, month, day, hour, minute, second;
    size_t t;

    if (dt == NULL || str == NULL || strlen(str) != 20)
        return false;

    t = sscanf(str, "%d/%d/%d,%d:%d:%d%c%d",
               &year, &month, &day, &hour, &minute, &second, &tz_sign, &tz_offset);

    /* Verify the output value */
    if (t != 8)
        return false;
    if (tz_sign != '+' && tz_sign != '-')
        return false;
    if (tz_offset < -48 || tz_offset > 48)
        return false;

    /* Casting data to output*/
    dt->year = (uint8_t)year;
    dt->month = (uint8_t)month;
    dt->day = (uint8_t)day;
    dt->hours = (uint8_t)hour;
    dt->minutes = (uint8_t)minute;
    dt->seconds = (uint8_t)second;

    return true;
}

/* -------------------------------------------------------------------------- */

bool datetime_to_string(const RTC_DateTime_t *dt, char *str)
{
    if (dt == NULL || str == NULL)
        return false;

    sprintf(str, "%02d/%02d/%02d,%02d:%02d:%02d+28",
            dt->year, dt->month, dt->day, dt->hours, dt->minutes, dt->seconds);

    return true;
}

/* -------------------------------------------------------------------------- */

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
uint8_t datetime_day_of_week(uint8_t dd, uint8_t mm, uint8_t yy)
{
    static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    yy += 28; /* +28 years ≡ 0 mod 7 (28yr = 7×1461 days) */
    if (mm < 3)
        yy--; /* Jan/Feb belong to previous year           */
    return (yy + (yy >> 2) + t[mm - 1] + dd - 14) % 7;
}

/* -------------------------------------------------------------------------- */

bool datetime_set_rtc(const RTC_DateTime_t *dt)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};

    sTime.Hours = dt->hours;
    sTime.Minutes = dt->minutes;
    sTime.Seconds = dt->seconds;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;

    sDate.WeekDay = datetime_day_of_week(dt->day, dt->month, dt->year);
    sDate.Month = dt->month;
    sDate.Date = dt->day;
    sDate.Year = dt->year;

    return ((HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) == HAL_OK) &&
            (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) == HAL_OK));
}

/* -------------------------------------------------------------------------- */

bool datetime_get_datetime_from_rtc(RTC_DateTime_t *dt)
{
    RTC_DateTypeDef gDate;
    RTC_TimeTypeDef gTime;

    /* Get the RTC current Time */
    if (HAL_RTC_GetTime(&hrtc, &gTime, RTC_FORMAT_BIN) != HAL_OK)
        return false;

    /* Get the RTC current Date */
    if (HAL_RTC_GetDate(&hrtc, &gDate, RTC_FORMAT_BIN) != HAL_OK)
        return false;

    dt->year = gDate.Year;
    dt->month = gDate.Month;
    dt->day = gDate.Date;
    dt->hours = gTime.Hours;
    dt->minutes = gTime.Minutes;
    dt->seconds = gTime.Seconds;

    return true;
}

/* -------------------------------------------------------------------------- */

bool datetime_sync_rtc_from_modem(void)
{
    char buf[32] = {0};
    RTC_DateTime_t dt = {0};

    /* Retrieve the current time string from the modem */
    if (!modem_get_datetime(buf))
        return false;

    /* Parse the string into the datetime structure */
    if (!datetime_from_string(&dt, buf))
        return false;

    /* Update the hardware RTC */
    return datetime_set_rtc(&dt);
}

/* -------------------------------------------------------------------------- */

bool datetime_sync_modem_from_rtc(void)
{
    char buf[32] = {0};
    RTC_DateTime_t dt = {0};

    /* Retrieve the current time string from the rtc */
    if(!datetime_get_datetime_from_rtc(&dt))
        return false;

    /* Parse the string into the datetime structure */
    if (!datetime_to_string(&dt, buf))
        return false;

    /* Update the hardware RTC */
    return modem_set_datetime(buf);
}

/* -------------------------------------------------------------------------- */

bool datetime_is_plausible(const RTC_DateTime_t *dt)
{
    if (dt == NULL)
        return false;

    return (dt->year    >= DATETIME_YEAR_MIN &&
            dt->year    <= DATETIME_YEAR_MAX &&
            dt->month   >= 1u  && dt->month   <= 12u &&
            dt->day     >= 1u  && dt->day     <= 31u &&
            dt->hours        <= 23u &&
            dt->minutes      <= 59u &&
            dt->seconds      <= 59u);
}

/* -------------------------------------------------------------------------- */

TimeSourceResult_t datetime_sync_with_best_source(void)
{
    RTC_DateTime_t rtc_dt   = {0};
    RTC_DateTime_t modem_dt = {0};

    /* ── Read MCU RTC ────────────────────────────────────────────────────── */
    const bool rtc_ok = datetime_get_datetime_from_rtc(&rtc_dt) &&
                        datetime_is_plausible(&rtc_dt);

    /* ── Read modem clock ────────────────────────────────────────────────── */
    bool modem_ok = false;
    if (modem_is_init())
    {
        char buf[32];
        memset(buf, 0, sizeof(buf));
        modem_ok = modem_get_datetime(buf)              &&
                   datetime_from_string(&modem_dt, buf) &&
                   datetime_is_plausible(&modem_dt);
    }

    /* ── Arbitration ─────────────────────────────────────────────────────── */

    if (modem_ok)
    {
        /*
         * Modem (NTP) is the authority.  Discipline the MCU RTC regardless
         * of whether it was valid — keeps it fresh for the next cold-start
         * where the modem may not be available.
         */
        (void)datetime_set_rtc(&modem_dt);
        return rtc_ok ? TIME_SOURCE_BOTH : TIME_SOURCE_MODEM_ONLY;
    }

    if (rtc_ok)
    {
        /*
         * RTC is the only valid source.  Push its value to the modem so that
         * AT+CCLK? returns a meaningful time until the next NTP sync.
         */
        char buf[32];
        if (datetime_to_string(&rtc_dt, buf))
            (void)modem_set_datetime(buf);

        return TIME_SOURCE_RTC_ONLY;
    }

    /* No valid source — caller must not use the timestamp. */
    return TIME_SOURCE_NONE;
}