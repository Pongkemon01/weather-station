/**
 * @file    watchdog_task.c
 * @brief   Per-task heartbeat monitor and IWDG (independent watchdog) refresh.
 *
 * IWDG configuration
 * ------------------
 * Clock source : LSI (~32 kHz, enabled automatically by IWDG peripheral)
 * Prescaler    : /64  →  counter frequency ≈ 500 Hz
 * Reload value : 999  →  timeout = (999 + 1) / 500 = 2.000 s
 *
 * The watchdog task runs every WDT_PERIOD_MS (500 ms).  If all registered
 * tasks have kicked since the last interval, the IWDG is refreshed and the
 * alive-bitmask is cleared.  A single missed kick within any 500 ms window
 * suppresses the refresh; the MCU resets after ≤ 2 s of starvation.
 */

#include "watchdog_task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#include "stm32l4xx_hal.h"   /* IWDG_HandleTypeDef, HAL_IWDG_* */

/* ---- IWDG parameters ------------------------------------------ */
/*
 * LSI ≈ 32 000 Hz / 64 = 500 Hz counter frequency.
 * Reload 999 → timeout = 1000 / 500 = 2.000 s.
 * Adjust if LSI calibration data shows significant drift.
 */
#define IWDG_PRESCALER_VAL  IWDG_PRESCALER_64
#define IWDG_RELOAD_VAL     999u

/* ---- Module state --------------------------------------------- */

/** Names of registered tasks (pointers — not copied). */
static const char *g_task_names[WDT_MAX_TASKS];

/** Bitmask of occupied heartbeat slots. */
static uint32_t g_registered_mask = 0u;

/** Bitmask of tasks that have kicked in the current interval.
 *  Written under taskENTER_CRITICAL() / taskEXIT_CRITICAL(). */
static volatile uint32_t g_alive_mask = 0u;

/** Number of slots in use. */
static uint8_t g_slot_count = 0u;

static IWDG_HandleTypeDef hiwdg;

/* ================================================================ */
/* Public API                                                        */
/* ================================================================ */

/**
 * @brief  Register a task with the watchdog monitor.
 */
int8_t wdt_register(const char *name)
{
    if (g_slot_count >= WDT_MAX_TASKS)
        return -1;

    uint8_t id = g_slot_count++;
    g_task_names[id] = name;
    g_registered_mask |= (1u << id);
    return (int8_t)id;
}

/**
 * @brief  Record that the calling task is alive for this interval.
 */
void wdt_kick(int8_t id)
{
    if (id < 0 || (uint8_t)id >= WDT_MAX_TASKS)
        return;

    taskENTER_CRITICAL();
    g_alive_mask |= (1u << (uint8_t)id);
    taskEXIT_CRITICAL();
}

/* ================================================================ */
/* FreeRTOS task                                                     */
/* ================================================================ */

/**
 * @brief  Watchdog monitor task — initialises IWDG then polls heartbeats.
 */
void watchdog_task(void *params)
{
    (void)params;

    /* Register this task with itself so it is counted in g_registered_mask. */
    int8_t self_id = wdt_register("watchdog");

    /* ---- Initialise IWDG ---- */
    hiwdg.Instance       = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_VAL;
    hiwdg.Init.Reload    = IWDG_RELOAD_VAL;
    hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
    {
        /* IWDG init failure is unrecoverable — spin and let reset occur. */
        for (;;) { }
    }

    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        wdt_kick(self_id);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WDT_PERIOD_MS));

        /*
         * Snapshot the alive mask atomically, then clear it so tasks
         * must re-kick within the next interval.
         */
        taskENTER_CRITICAL();
        uint32_t alive = g_alive_mask;
        g_alive_mask   = 0u;
        taskEXIT_CRITICAL();

        if (alive == g_registered_mask)
        {
            /* Every registered task checked in — pet the dog. */
            HAL_IWDG_Refresh(&hiwdg);
        }
        /*
         * If any task missed its kick, we deliberately do NOT refresh.
         * The IWDG will expire within its hardware timeout (~2 s) and
         * reset the MCU, which will boot into the bootloader.
         */
    }
}
