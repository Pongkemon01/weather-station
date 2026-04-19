/**
 * @file    watchdog_task.h
 * @brief   Per-task heartbeat monitor and IWDG (independent watchdog) refresh.
 *
 * Design
 * ------
 * Each FreeRTOS task registers once at startup with wdt_register(), receiving
 * a unique slot ID (0–WDT_MAX_TASKS-1).  Within every 500 ms period the task
 * must call wdt_kick() with that ID.
 *
 * WatchdogTask runs every WDT_PERIOD_MS (500 ms).  If every registered task
 * has called wdt_kick() since the last check, the IWDG is refreshed and the
 * heartbeat flags are cleared for the next interval.  If any task misses its
 * kick, the IWDG is NOT refreshed; after the hardware timeout (~2 s) the MCU
 * resets into the bootloader.
 *
 * Mutex / ordering rules
 * ----------------------
 * wdt_kick() uses a FreeRTOS critical section for an atomic bit-set.  It must
 * NOT be called from an ISR.  wdt_register() must be called before the
 * scheduler starts (single-threaded context).
 */

#ifndef WATCHDOG_TASK_H
#define WATCHDOG_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of concurrently registered tasks. */
#define WDT_MAX_TASKS 16u

/** Watchdog check period in milliseconds (must match IWDG timeout). */
#define WDT_PERIOD_MS 500u

/**
 * @brief  Register a task with the watchdog monitor.
 *
 * Must be called before the RTOS scheduler starts (or with external
 * serialisation if called later).  Each call reserves one heartbeat slot.
 *
 * @param  name  Human-readable task name (stored for debug; not copied,
 *               must remain valid for the lifetime of the task).
 * @retval Slot ID (0 … WDT_MAX_TASKS-1) on success.
 * @retval -1 if all slots are occupied.
 */
int8_t wdt_register(const char *name);

/**
 * @brief  Record that the calling task is alive for this interval.
 *
 * Each task must call this at least once every WDT_PERIOD_MS milliseconds.
 * Safe to call multiple times per interval (idempotent bit-set).
 *
 * @param  id  Slot ID returned by wdt_register().
 */
void wdt_kick(int8_t id);

/**
 * @brief  FreeRTOS task entry point for the watchdog monitor.
 *
 * Registered in MX_FREERTOS_Init() at priority osPriorityHigh.
 * Initialises the IWDG hardware then enters the check loop.
 *
 * @param  params  Unused (pass NULL).
 */
void watchdog_task(void *params);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_TASK_H */
