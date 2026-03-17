#ifndef UART_SUBSYSTEM_H
#define UART_SUBSYSTEM_H

/* =============================================================================
 * UART Subsystem — STM32 HAL DMA/Idle-line, FreeRTOS buffer pool
 * =============================================================================
 * Set every UART's Rx DMA to "Circular" mode in STM32CubeMX.
 *
 * Typical usage
 * ─────────────
 *  void ModemTask(void *arg) {
 *      UART_Sys_Init();
 *      UART_Ctx_t *modem = UART_Sys_Register(&huart1);
 *      UART_Ctx_t *gps   = UART_Sys_Register(&huart4);
 *
 *      UART_Packet_t pkt;
 *      for (;;) {
 *          if (UART_Sys_Receive(modem, &pkt, portMAX_DELAY)) {
 *              process(pkt.payload, pkt.length);
 *              UART_Sys_ReleaseBuffer(pkt.payload);  // mandatory
 *          }
 *      }
 *  }
 * =============================================================================
 */

#include <stdint.h>
#include <stdbool.h>
#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

/* ─────────────────────────── Configuration ──────────────────────────────── */

/** Number of UART peripherals that can be registered. */
#define MAX_UARTS           3u

/*
 * The A7670E modem emits 5-6 lines back-to-back during TLS handshake (AT
 * echo + status lines + URC). With 3 UARTs × 6 slots = 18 total pool blocks,
 * a burst of 6 modem lines before the AT task drains the queue exhausted the
 * pool and caused the ISR to silently discard data.
 */
#define UART_RX_SIZE        6u

/** Total pool blocks shared across all UARTs (MAX_UARTS × UART_RX_SIZE). */
#define UART_POOL_BLOCKS    (MAX_UARTS * UART_RX_SIZE)   /* = 18 */

/**
 * Maximum bytes per pool block.
 * UART_DMA_BUF_SIZE must be ≥ 2 × UART_BLOCK_SIZE so a full circular
 * buffer fill never exceeds one pool block.
 */
#define UART_BLOCK_SIZE     128u

/** Per-UART circular DMA hardware receive buffer. */
#define UART_DMA_BUF_SIZE   256u

/* ─────────────────────────── Data structures ────────────────────────────── */

/** Received data frame delivered to application tasks. */
typedef struct {
    uint8_t            *payload;   /* points into static pool; release after use */
    uint16_t            length;
    UART_HandleTypeDef *huart;
} UART_Packet_t;

/**
 * Per-UART runtime context.
 *
 * Changes from original:
 *
 *   last_read_ptr: uint32_t → uint16_t   (FIX BUG-5)
 *     HAL's Size parameter and all DMA indices are uint16_t. Storing the
 *     value in uint32_t was misleading, wasted 2 bytes per context, and
 *     caused implicit promotion in every ISR arithmetic expression.
 *
 *   tx_done_sem: added per-context   (FIX BUG-1)
 *     The original code had one global xTxDoneSem shared by all UARTs.
 *     HAL_UART_TxCpltCallback fired for every UART and gave the single
 *     semaphore regardless of which UART completed, allowing a fast UART to
 *     unblock a task waiting on a different, still-in-progress UART.
 *     Moving the semaphore into each context lets TxCpltCallback signal
 *     exactly the right waiter.
 */
typedef struct {
    UART_HandleTypeDef *huart;
    uint8_t             dma_rx_buf[UART_DMA_BUF_SIZE];
    uint16_t            last_read_ptr;  /* last-consumed DMA write position   */
    QueueHandle_t       rx_queue;
    SemaphoreHandle_t   tx_mutex;
    SemaphoreHandle_t   tx_done_sem;    /* signalled by HAL_UART_TxCpltCallback */
} UART_Ctx_t;

/* ─────────────────────────── Public API ─────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise the shared buffer pool. Call once before UART_Sys_Register. */
void        UART_Sys_Init(void);

/**
 * Register a UART and start its circular DMA receive.
 * Returns the context pointer used in all subsequent calls, or NULL if
 * MAX_UARTS has been reached. Registering the same huart twice returns the
 * existing context (idempotent).
 */
UART_Ctx_t *UART_Sys_Register(UART_HandleTypeDef *huart);

/**
 * Transmit len bytes over ctx's UART using DMA, blocking until the transfer
 * completes or timeout_ms elapses. Returns true on success.
 *
 * pData is const — callers may pass string literals without casting.  (FIX DESIGN-4)
 */
bool UART_Sys_Send(UART_Ctx_t *ctx, const uint8_t *pData,
                   uint16_t len, uint32_t timeout_ms);

/**
 * Block until a packet arrives on ctx's RX queue (up to timeout_ms ticks).
 * On success, out_packet->payload must later be passed to UART_Sys_ReleaseBuffer.
 */
bool UART_Sys_Receive(UART_Ctx_t *ctx, UART_Packet_t *out_packet,
                      uint32_t timeout_ms);

/**
 * Discard all pending packets on ctx's RX queue, returning their pool
 * buffers. Safe to call at any time; does not affect other UARTs.
 */
bool UART_Sys_FlushReceive(UART_Ctx_t *ctx);

/**
 * Return a payload buffer to the free pool. Must be called exactly once per
 * received packet after the application has finished reading it.
 */
void UART_Sys_ReleaseBuffer(uint8_t *pBuffer);

#ifdef __cplusplus
}
#endif

#endif /* UART_SUBSYSTEM_H */
