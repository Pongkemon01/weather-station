/**
 * @file freertos_lock.c
 * @brief Newlib thread-safe locking — FreeRTOS Strategy #4.
 *
 * Uses taskENTER_CRITICAL_FROM_ISR() / taskEXIT_CRITICAL_FROM_ISR() so that
 * locks may be acquired from both task context and ISR context.
 *
 * Requires -D_RETARGET_LOCKING in build flags so that <sys/lock.h> exposes
 * the struct __lock * interface and the __retarget_lock_* entry points.
 *
 * Equivalent to enabling "Multi-threaded support" with
 * "FreeRTOS Strategy #4 - Allow lock usage from interrupts" in STM32CubeMX.
 */

#include <sys/lock.h>
#include "FreeRTOS.h"
#include "task.h"

/* newlib requires struct __lock to be defined by the application when
   _RETARGET_LOCKING is defined. The flag field carries the saved interrupt
   mask returned by taskENTER_CRITICAL_FROM_ISR() and consumed by
   taskEXIT_CRITICAL_FROM_ISR(). */
struct __lock {
    UBaseType_t flag;
};

/* Static instances required by newlib for its own internal locks. */
struct __lock __lock___sinit_recursive_mutex   = {0};
struct __lock __lock___sfp_recursive_mutex     = {0};
struct __lock __lock___atexit_recursive_mutex  = {0};
struct __lock __lock___at_quick_exit_mutex     = {0};
struct __lock __lock___malloc_recursive_mutex  = {0};
struct __lock __lock___env_recursive_mutex     = {0};
struct __lock __lock___dd_hash_mutex           = {0};
struct __lock __lock___arc4random_mutex        = {0};

/**
 * @brief Initialise a dynamically allocated lock (no-op for static strategy).
 */
void __retarget_lock_init(_LOCK_T *lock)
{
    (void)lock;
}

/**
 * @brief Initialise a dynamically allocated recursive lock (no-op).
 */
void __retarget_lock_init_recursive(_LOCK_T *lock)
{
    (void)lock;
}

/**
 * @brief Close a dynamically allocated lock (no-op).
 */
void __retarget_lock_close(_LOCK_T lock)
{
    (void)lock;
}

/**
 * @brief Close a dynamically allocated recursive lock (no-op).
 */
void __retarget_lock_close_recursive(_LOCK_T lock)
{
    (void)lock;
}

/**
 * @brief Acquire a lock. Safe from both task and ISR context.
 */
void __retarget_lock_acquire(_LOCK_T lock)
{
    lock->flag = taskENTER_CRITICAL_FROM_ISR();
}

/**
 * @brief Acquire a recursive lock. Safe from both task and ISR context.
 */
void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    lock->flag = taskENTER_CRITICAL_FROM_ISR();
}

/**
 * @brief Try to acquire a lock without blocking (always succeeds).
 * @return 1 (success).
 */
int __retarget_lock_try_acquire(_LOCK_T lock)
{
    lock->flag = taskENTER_CRITICAL_FROM_ISR();
    return 1;
}

/**
 * @brief Try to acquire a recursive lock without blocking (always succeeds).
 * @return 1 (success).
 */
int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
    lock->flag = taskENTER_CRITICAL_FROM_ISR();
    return 1;
}

/**
 * @brief Release a lock. Restores the interrupt mask saved at acquire time.
 */
void __retarget_lock_release(_LOCK_T lock)
{
    taskEXIT_CRITICAL_FROM_ISR(lock->flag);
}

/**
 * @brief Release a recursive lock.
 */
void __retarget_lock_release_recursive(_LOCK_T lock)
{
    taskEXIT_CRITICAL_FROM_ISR(lock->flag);
}
