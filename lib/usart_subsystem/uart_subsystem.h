/**
 * @file    uart_subsystem.h
 * @brief   UART Subsystem — STM32 HAL DMA/Idle-line, FreeRTOS buffer pool.
 *
 * =============================================================================
 * Set every UART's Rx DMA to "Circular" mode in STM32CubeMX.
 * For RS-485 UARTs, enable "RS485 Driver Enable" (DE) in CubeMX so the
 * hardware UART_CR3_DEM bit asserts the DE pin automatically on each TX;
 * no GPIO toggling is required in application code.
 *
 * Typical usage
 * ─────────────
 *  void ModemTask(void *arg) {
 *      UART_Sys_Init();
 *      UART_Ctx_t *modem = UART_Sys_Register(&huart1);
 *
 *      UART_Packet_t pkt;
 *      for (;;) {
 *          if (UART_Sys_Receive(modem, &pkt, portMAX_DELAY)) {
 *              process(pkt.payload, pkt.length);
 *              UART_Sys_ReleaseBuffer(pkt.payload);  // mandatory
 *          }
 *      }
 *  }
 *
 * Bug-fix history
 * ───────────────
 *  BUG-1   Per-UART tx_done_sem; TxCpltCallback matches by Instance.
 *  BUG-2   len clamped to UART_BLOCK_SIZE in ISR before any memcpy.
 *  BUG-4   HAL_UART_DMAStop() called on TX timeout to clear HAL_BUSY.
 *  BUG-5   last_read_ptr type changed uint32_t → uint16_t.
 *  BUG-6   ReleaseBuffer: NULL guard + pool-range check.
 *  DESIGN-1 xFreeQueue NULL guard prevents double-init.
 *  DESIGN-2 Duplicate-register guard returns existing context.
 *  BUG-U1  FreeRTOS objects created and validated before slot committed;
 *          objects cleaned up on any allocation failure (release-safe).
 *  BUG-U2  HAL_UARTEx_ReceiveToIdle_DMA return value checked; NULL returned
 *          on failure so callers are not given a dead context.
 * =============================================================================
 */

#ifndef UART_SUBSYSTEM_H
#define UART_SUBSYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

/* ─────────────────────────── Configuration ──────────────────────────────── */

/** Number of UART peripherals that can be registered simultaneously. */
#define MAX_UARTS           3u

/**
 * RX queue depth per UART.
 * 6 slots handles the typical A7670E burst of 5–6 response lines during a
 * TLS handshake without pool exhaustion.  For Modbus RTU (strictly one
 * outstanding frame) 2 would suffice, but 6 is fine and costs only
 * 6 × sizeof(UART_Packet_t) = 6 × 8 = 48 bytes of FreeRTOS heap per UART.
 */
#define UART_RX_SIZE        6u

/** Total pool blocks shared across all registered UARTs. */
#define UART_POOL_BLOCKS    (MAX_UARTS * UART_RX_SIZE)   /* = 18 */

/**
 * Maximum payload bytes per pool block.
 *
 * UART_DMA_BUF_SIZE must be >= 2 × UART_BLOCK_SIZE so that a full circular
 * buffer fill never exceeds one pool block in a single ISR callback.
 * With 4800 baud and Modbus RTU, the largest legal frame is 256 bytes
 * (FC03 response, 125 registers × 2 bytes + 5 overhead) — larger than one
 * block.  In practice SEM228P and R66S return at most a handful of registers,
 * so 128 bytes is ample.  If you add slaves with longer responses, increase
 * both constants proportionally.
 */
#define UART_BLOCK_SIZE     128u

/** Per-UART circular DMA hardware receive buffer (2 × UART_BLOCK_SIZE). */
#define UART_DMA_BUF_SIZE   256u

/* ─────────────────────────── Data structures ────────────────────────────── */

/**
 * Received data frame delivered to application tasks.
 * payload must be returned to the pool with UART_Sys_ReleaseBuffer() after use.
 */
typedef struct {
    uint8_t            *payload;   /**< Points into the static pool. */
    uint16_t            length;    /**< Number of valid bytes in payload. */
    UART_HandleTypeDef *huart;     /**< Which UART this packet arrived on. */
} UART_Packet_t;

/**
 * Per-UART runtime context.
 *
 * last_read_ptr  uint16_t — matches HAL's Size parameter type exactly,
 *                avoiding implicit promotion in ISR arithmetic (BUG-5).
 *
 * tx_done_sem    Per-context binary semaphore — ensures TxCpltCallback
 *                signals only the UART that actually completed (BUG-1).
 */
typedef struct {
    UART_HandleTypeDef *huart;
    bool                is_ready;
    uint8_t             dma_rx_buf[UART_DMA_BUF_SIZE];
    uint16_t            last_read_ptr;      /**< Last-consumed DMA write position. */
    QueueHandle_t       rx_queue;
    SemaphoreHandle_t   tx_mutex;
    SemaphoreHandle_t   tx_done_sem;        /**< Signalled by HAL_UART_TxCpltCallback. */
} UART_Ctx_t;

/* ─────────────────────────── Public API ─────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise the shared buffer pool.
 *
 * Must be called once before any UART_Sys_Register call.  Safe to call
 * multiple times — returns false without side-effects on subsequent calls
 * (DESIGN-1).
 *
 * @return true on first successful initialisation, false otherwise.
 */
bool        UART_Sys_Init(void);

/**
 * @brief  Register a UART peripheral and start its circular DMA receive.
 *
 * Registering the same huart twice returns the existing context (idempotent,
 * DESIGN-2).  All three FreeRTOS objects (mutex, semaphore, queue) are
 * created and validated before the slot is committed; on any failure the
 * objects are freed and NULL is returned (BUG-U1).  The HAL DMA start
 * return value is checked; NULL is returned if DMA fails to start (BUG-U2).
 *
 * @param  huart  Initialised HAL UART handle with DMA Rx in Circular mode.
 * @return Pointer to the allocated context, or NULL on failure.
 */
UART_Ctx_t *UART_Sys_Register(UART_HandleTypeDef *huart);

/**
 * @brief  Un-register a UART, stop DMA receive, and free all FreeRTOS objects.
 *
 * Any pending packets in the RX queue are flushed and their pool buffers
 * returned before the queue is deleted.
 *
 * @param  ctx  Context returned by UART_Sys_Register.
 */
void        UART_Sys_UnRegister(UART_Ctx_t *ctx);

/**
 * @brief  Transmit @p len bytes over @p ctx's UART using DMA.
 *
 * Blocks until the transfer completes or @p timeout_ms elapses.
 * On timeout the stalled DMA transfer is aborted so that subsequent calls
 * are not permanently blocked by HAL_BUSY (BUG-4).
 * pData is const — callers may pass string literals or read-only buffers.
 *
 * @param  ctx         Registered UART context.
 * @param  pData       Data to transmit.
 * @param  len         Number of bytes.
 * @param  timeout_ms  Maximum wait time in milliseconds.
 * @return true on success, false on timeout or HAL error.
 */
bool        UART_Sys_Send(UART_Ctx_t *ctx, const uint8_t *pData,
                          uint16_t len, uint32_t timeout_ms);

/**
 * @brief  Block until a packet arrives on @p ctx's RX queue.
 *
 * @param  ctx         Registered UART context.
 * @param  out_packet  Packet structure to fill; out_packet->payload must be
 *                     released with UART_Sys_ReleaseBuffer after use.
 * @param  timeout_ms  Maximum wait time in milliseconds.
 * @return true if a packet was received within the timeout.
 */
bool        UART_Sys_Receive(UART_Ctx_t *ctx, UART_Packet_t *out_packet,
                             uint32_t timeout_ms);

/**
 * @brief  Discard all pending packets on @p ctx's RX queue.
 *
 * Returns each discarded packet's pool buffer.  Safe to call at any time;
 * does not affect other registered UARTs.
 *
 * @param  ctx  Registered UART context.
 * @return true on success, false if ctx is NULL.
 */
bool        UART_Sys_FlushReceive(UART_Ctx_t *ctx);

/**
 * @brief  Return a payload buffer to the shared free pool.
 *
 * Must be called exactly once per received packet after the application
 * has finished reading it.  NULL and out-of-pool pointers are silently
 * rejected in release builds and trapped by configASSERT in debug (BUG-6).
 *
 * @param  pBuffer  Pointer previously obtained from a UART_Packet_t payload.
 */
void        UART_Sys_ReleaseBuffer(uint8_t *pBuffer);

#ifdef __cplusplus
}
#endif

#endif /* UART_SUBSYSTEM_H */
