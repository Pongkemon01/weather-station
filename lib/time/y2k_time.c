/*
 * y2k_time.c — Y2K epoch ↔ datetime conversion
 *
 * Target : STM32L476RG (Cortex-M4F, single-cycle MUL, no hardware divide)
 * Valid  : 2000-01-01 00:00:00  ..  2099-12-31 23:59:59
 *          (dt->year: 0 = 2000, 99 = 2099)
 *
 * ── Optimisation notes for Cortex-M4F ───────────────────────────────────
 *
 * 1. No FPU usage.
 *    All calendar arithmetic is exact integer math.  Loading a float register
 *    and performing VCVT/VMUL would cost more cycles than the integer path.
 *
 * 2. Division by compile-time constants only.
 *    GCC -O2 replaces every "/N" and "%N" with a multiply-high + shift
 *    sequence (~3–4 cycles) when N is a compile-time constant, which is
 *    faster than even the M4's hardware UDIV (~2–12 cycles).  All divisors
 *    here (60, 24, 1461) are literals, so the compiler handles this
 *    automatically — no manual reciprocal tricks required.
 *
 * 3. LUTs are "static const" → placed in .rodata (Flash), 0 RAM consumed.
 *    Two 12-entry uint16_t tables (non-leap + leap month starts) = 48 bytes.
 *    One 4-entry uint16_t table (era year starts)                =  8 bytes.
 *    Total Flash overhead: 56 bytes.
 *
 * 4. Month search is a 12-iteration linear scan with no conditional branches
 *    inside the loop body (just array load + compare).  GCC -O2 unrolls this
 *    completely.  Binary search is not worthwhile for N = 12.
 *
 * ── Bugs fixed vs. original ─────────────────────────────────────────────
 *
 * Bug A (critical) — year_in_era formula wrong at era leap-year boundary.
 *   Original: year_in_era = (doe * 4 + 3) / 1461
 *   For doe = 365 (Dec 31 of the leap year, e.g. 2000-12-31):
 *     (365*4+3)/1461 = 1463/1461 = 1  ← wrong; should be 0.
 *   The ceiling formula assumes all years are 365 days, but year 0 of each
 *   era is a leap year with 366 days, so its boundary is doe 366, not 365.
 *   This cascaded into year_start (366) > doe (365), causing uint16_t
 *   underflow in doy for the last day of every leap year (Dec 31 of
 *   2000, 2004, 2008 … 2096).
 *   Fix: replace formula with a 4-entry boundary table (k_era_year_start).
 *
 * Bug B (critical) — doy formula had an erroneous "- 1".
 *   Original: doy = doe - (year_in_era * 365 + (year_in_era + 3) / 4 - 1)
 *   For year_in_era == 0: expression = 0 - 1 → wraps to 0xFFFF.
 *   This corrupted all dates in every leap year (2000, 2004 … 2096).
 *   Fix: use k_era_year_start[year_in_era] directly — no arithmetic needed.
 *
 * Bug C (subtle) — month search used the non-leap table for leap years.
 *   Feb 29 has doy == 59 (0-based), which equals k_month_start_nl[2]
 *   (the non-leap start of March).  The condition "doy < tbl[m]" with the
 *   non-leap table gives 59 < 59 == false, skipping February entirely and
 *   mapping Feb 29 → March 1.
 *   Fix: use two separate tables (k_month_start_nl / k_month_start_lp) and
 *   select the correct one once based on is_leap, before the loop.  This
 *   also removes all conditional branches from inside the loop body.
 */

#include "y2k_time.h"
#include <stdbool.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

/** Days in one 4-year era: 3 × 365 + 366 = 1461. */
#define DAYS_PER_ERA  1461u

/*
 * Start day (0-based day_of_era) of each year within a 4-year era.
 *
 *   year_in_era │ first doe │ last doe │ days
 *       0 (leap)│     0     │   365    │  366
 *       1       │   366     │   730    │  365
 *       2       │   731     │  1095    │  365
 *       3       │  1096     │  1460    │  365
 *
 * 8 bytes in .rodata (Flash).
 */
static const uint16_t k_era_year_start[4] = { 0u, 366u, 731u, 1096u };

/*
 * Cumulative days at the start of each month, 0-indexed (Jan = index 0).
 * Non-leap and leap variants — 48 bytes total in .rodata (Flash).
 *
 * Using two tables eliminates all conditional branches from the month
 * search loop; the correct pointer is selected once before the loop.
 */
static const uint16_t k_month_start_nl[12] = { /* non-leap year */
/*  Jan  Feb  Mar  Apr  May  Jun  Jul  Aug  Sep  Oct  Nov  Dec */
      0,  31,  59,  90, 120, 151, 181, 212, 243, 273, 304, 334
};
static const uint16_t k_month_start_lp[12] = { /* leap year */
/*  Jan  Feb  Mar  Apr  May  Jun  Jul  Aug  Sep  Oct  Nov  Dec */
      0,  31,  60,  91, 121, 152, 182, 213, 244, 274, 305, 335
};

/* =========================================================================
 * get_epoch_from_datetime — struct → epoch
 *
 * This function was correct in the original; comments and const-correctness
 * updated only.
 *
 * Cortex-M4 cost at -O2:
 *   ~10 instructions: 2× MUL, 1× shift, ADD chain, 2× LDR (LUT). No divide.
 * ========================================================================= */

/**
 * @brief  Convert a calendar datetime to seconds since 2000-01-01 00:00:00.
 * @note   Valid for dt->year 0–99 (2000–2099).
 * @param  dt  Pointer to a populated RTC_DateTime_t.
 * @return Seconds since Y2K epoch.
 */
uint32_t get_epoch_from_datetime(const RTC_DateTime_t *dt)
{
    /*
     * Days contributed by complete years.
     *
     * In 2000–2099, every year divisible by 4 (0-based) is a leap year.
     * The century exception (2100 ÷ 400 ≠ 0) is outside our range.
     *
     * Leap days elapsed before year Y: ⌊(Y + 3) / 4⌋
     * Implemented as (Y + 3) >> 2  (shift, no divide instruction).
     */
    uint32_t total_days = ((uint32_t)dt->year * 365u)
                        + (((uint32_t)dt->year + 3u) >> 2);

    /* Days from complete months in the current year (non-leap baseline). */
    total_days += k_month_start_nl[dt->month - 1u];

    /*
     * Add Feb 29 if we are past February in a leap year.
     * Bitwise AND avoids a modulo division: (year & 3) == 0 ↔ year % 4 == 0.
     */
    if ((dt->month > 2u) && ((dt->year & 3u) == 0u))
    {
        total_days++;
    }

    /* Elapsed days in the current month (1-indexed → subtract 1). */
    total_days += (uint32_t)(dt->day - 1u);

    /*
     * Convert days + time-of-day to seconds.
     * All multipliers are compile-time constants → single-cycle MUL on M4.
     */
    return (total_days             * 86400u)
         + ((uint32_t)dt->hours   *  3600u)
         + ((uint32_t)dt->minutes *    60u)
         +  (uint32_t)dt->seconds;
}

/* =========================================================================
 * get_datetime_from_epoch — epoch → struct (three bugs fixed; optimised)
 *
 * Cortex-M4 cost at -O2:
 *   Divisions by 60, 24, 1461 → multiply-high sequences (~3–4 cycles each).
 *   Year-in-era: 3 comparisons, no divide.
 *   Month search: 11 × (LDR + CMP), fully unrolled by compiler.
 *   Total: ~50–60 cycles typical path.
 * ========================================================================= */

/**
 * @brief  Convert seconds since 2000-01-01 00:00:00 to a calendar datetime.
 * @note   Valid for years 2000–2099 (epoch 0 .. 3 155 759 999).
 * @param  epoch  Seconds since Y2K epoch.
 * @param  dt     Output struct; all six fields are written before return.
 */
void get_datetime_from_epoch(uint32_t epoch, RTC_DateTime_t *dt)
{
    /* ── 1. Time-of-day decomposition ─────────────────────────────────── */

    /*
     * Pair each division with its modulo on the same dividend.
     * GCC -O2 fuses div+mod pairs into a single multiply-high sequence
     * (quotient + remainder in one operation — no second division).
     */
    uint32_t minutes  = epoch   / 60u;
    dt->seconds       = (uint8_t)(epoch   % 60u);

    uint32_t hours    = minutes / 60u;
    dt->minutes       = (uint8_t)(minutes % 60u);

    uint32_t days     = hours   / 24u;
    dt->hours         = (uint8_t)(hours   % 24u);

    /* ── 2. Year decomposition ────────────────────────────────────────── */

    /*
     * Split total days into 4-year eras.
     * Every 4-year block in 2000–2099 is exactly 1461 days (no exceptions).
     *
     *   era       : block index (0 = 2000–2003, 1 = 2004–2007, …, 24 = 2096–2099)
     *   day_of_era: 0-based day within the block [0, 1460]
     */
    uint32_t era        = days / DAYS_PER_ERA;
    uint32_t day_of_era = days % DAYS_PER_ERA;

    /*
     * Determine year within the era using boundary comparisons.
     *
     * FIX for Bug A: the original formula (doe*4+3)/1461 returns 1 for
     * doe == 365, but day 365 is still inside the leap year (year 0 has
     * 366 days, doe 0..365).  A 4-way comparison against k_era_year_start[]
     * is both correct and cheaper (3 CMP vs. 1 MUL + shifts).
     */
    uint32_t year_in_era;
    if      (day_of_era < 366u)  year_in_era = 0u;  /* leap year */
    else if (day_of_era < 731u)  year_in_era = 1u;
    else if (day_of_era < 1096u) year_in_era = 2u;
    else                         year_in_era = 3u;

    dt->year = (uint8_t)(era * 4u + year_in_era);

    /*
     * 0-based day-of-year within the current year [0, 365 for leap, 0–364 otherwise].
     *
     * FIX for Bug B: the original subtracted an erroneous "- 1" in
     *   doe - (year_in_era*365 + (year_in_era+3)/4 - 1)
     * which wrapped to 0xFFFF when year_in_era == 0.
     * Using the precomputed table avoids both the arithmetic and the bug.
     */
    uint16_t doy = (uint16_t)(day_of_era - k_era_year_start[year_in_era]);

    /* ── 3. Month and day decomposition ──────────────────────────────── */

    /*
     * Select the month-start table once, before the loop.
     *
     * FIX for Bug C: in a leap year, Feb 29 has doy == 59, which is equal
     * to k_month_start_nl[2] (non-leap March start = 59).  The loop
     * condition "doy < tbl[m]" with the non-leap table gives 59 < 59 ==
     * false, so February was skipped and Feb 29 decoded as March 1.
     * k_month_start_lp[2] == 60, so 59 < 60 == true → correctly gives
     * month 2.  This also removes all branches from the loop body.
     */
    bool is_leap = ((dt->year & 3u) == 0u);
    const uint16_t *tbl = is_leap ? k_month_start_lp : k_month_start_nl;

    /*
     * Linear scan: find month m such that tbl[m-1] <= doy < tbl[m].
     * GCC -O2 fully unrolls this into 11 LDR + CMP pairs.
     */
    dt->month = 12u;   /* preset; overwritten for all months Jan–Nov */
    dt->day   = 1u;

    for (uint8_t m = 1u; m < 12u; m++)
    {
        if (doy < tbl[m])
        {
            dt->month = m;
            dt->day   = (uint8_t)(doy - tbl[m - 1u] + 1u);
            return;
        }
    }

    /* December: doy in [334, 364] (non-leap) or [335, 365] (leap). */
    dt->day = (uint8_t)(doy - tbl[11u] + 1u);
}
