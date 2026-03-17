#include "y2k_time.h"
#include <stdbool.h>

/**
 * @brief Convert Y2K epoch back to date-time format
 * @note  Valid for years 2000-2099.
 * @param epoch Epoch since Jan 1, 2000.
 * @param dt Pointer to Y2K_DateTime struct
 * @return None.
 */
void get_datetime_from_epoch(uint32_t epoch, RTC_DateTime_t *dt)
{
    // 1. Extract time components
    uint32_t minutes = epoch / 60;
    dt->seconds = epoch % 60;

    uint32_t hours = minutes / 60;
    dt->minutes = minutes % 60;

    uint32_t days = hours / 24;
    dt->hours = hours % 24;

    // 2. Calculate Year
    // In the 2000-2099 range, every 4th year is a leap year (2000, 2004...)
    // A 4-year cycle has (365*3 + 366) = 1461 days.
    uint32_t era = days / 1461; // Number of 4-year blocks
    uint32_t day_of_era = days % 1461;

    // Estimate year within the 4-year block
    // 0-365 (Yr 0), 366-730 (Yr 1), 731-1095 (Yr 2), 1096-1460 (Yr 3)
    uint32_t year_in_era = (day_of_era * 4 + 3) / 1461;
    dt->year = (era * 4) + year_in_era;

    uint16_t doy = day_of_era - (year_in_era * 365 + (year_in_era + 3) / 4 - 1);

    // 3. Calculate Month and Day
    static const uint16_t month_offsets[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 366};

    bool is_leap = (dt->year % 4 == 0);
    dt->month = 12;

    for (uint8_t m = 0; m < 12; m++)
    {
        uint16_t days_in_prev_months = month_offsets[m];

        // Adjust for leap year if past February
        if (is_leap && m > 1)
            days_in_prev_months++;

        if (doy < days_in_prev_months)
        {
            dt->month = m;
            // Subtract previous month offset to get day of month
            uint16_t prev_offset = month_offsets[m - 1];
            if (is_leap && m > 2)
                prev_offset++;
            dt->day = doy - prev_offset + 1;
            return;
        }
    }

    // Edge case for December
    uint16_t dec_offset = month_offsets[11];
    if (is_leap)
        dec_offset++;
    dt->day = doy - dec_offset + 1;
}

// ----------------------------------------------------------------------------
/**
 * @brief Optimized Y2K Epoch calculation for STM32L4 (Cortex-M4)
 * @note  Valid for years 2000-2099.
 * @param dt Pointer to Y2K_DateTime struct
 * @return uint32_t Seconds since Jan 1, 2000.
 */
uint32_t get_epoch_from_datetime(RTC_DateTime_t *dt)
{
    // Cumulative days at the start of each month (non-leap year)
    static const uint16_t month_offsets[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    // 1. Calculate days from full years passed.
    // Every 4th year is a leap year. Since 2000 was a leap year,
    // the number of leap days is (y + 3) / 4.
    uint32_t total_days = (dt->year * 365) + ((dt->year + 3) >> 2);

    // 2. Add days from months in the current year.
    total_days += month_offsets[dt->month - 1];

    // 3. If current year is leap and we are past February, add the leap day.
    // A year is leap if (y % 4 == 0).
    if ((dt->month > 2) && ((dt->year & 3) == 0))
    {
        total_days++;
    }

    // 4. Add days in the current month (d is 1-indexed)
    total_days += (dt->day - 1);

    // 5. Convert to seconds using optimized multiplication.
    // Cortex-M4 can perform 32-bit mul in 1 cycle.
    return (total_days * 86400) + (dt->hours * 3600) + (dt->minutes * 60) + dt->seconds;
}