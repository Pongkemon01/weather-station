/**
 * @file  ota_manager_task.h
 * @brief OTA firmware update state machine — FreeRTOS task + public API.
 *
 * State transitions:
 *   IDLE → POLLING_VERSION (xTaskNotify from ssluploadtask)
 *        → FETCHING_METADATA (server version > FW_VERSION)
 *        → DOWNLOADING       (metadata valid, bitmap cleared)
 *        → DOWNLOAD_COMPLETE (all chunks received)
 *        → VERIFIED          (SHA-256 matches)
 *        → REBOOT_PENDING    (OCB written with ota_pending=1)
 *        [reboot → bootloader → jumps to 0x08008000]
 *        → CONFIRMING        (new firmware running; 60 s confirm window)
 *        → IDLE              (ota_confirmed written)
 *
 * NOTE — network operations (AT+HTTPACTION) may block up to 60 s.
 * The IWDG timeout must exceed this value when OTA is in progress;
 * OtaManagerTask kicks the watchdog only at inter-chunk boundaries.
 */

#ifndef OTA_MANAGER_TASK_H
#define OTA_MANAGER_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_POLLING_VERSION,
    OTA_STATE_FETCHING_METADATA,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_DOWNLOAD_COMPLETE,
    OTA_STATE_VERIFIED,
    OTA_STATE_REBOOT_PENDING,
    OTA_STATE_CONFIRMING,
} OtaState_t;

/** Task handle — defined in freertos.c; used by ssluploadtask to notify OTA check. */
extern TaskHandle_t OtaManagerTaskHandle;

/**
 * @brief FreeRTOS task entry point for the OTA manager state machine.
 * @param params Unused (pass NULL).
 */
void ota_manager_task(void *params);

/**
 * @brief Signal that the running firmware has proved stable.
 *
 * Safe to call from any task.  Shortens the 60 s confirmation window in
 * OTA_STATE_CONFIRMING.  No-op when not in CONFIRMING state.
 */
void ota_confirm_success(void);

/**
 * @brief Return the current OTA state (non-blocking, no mutex required).
 */
OtaState_t ota_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANAGER_TASK_H */
