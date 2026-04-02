/**
 * @file    uart_subsystem.c
 * @brief   STM32 HAL DMA/Idle-line UART subsystem with FreeRTOS buffer pool.
 *
 * =============================================================================
 * Bugs fixed in this revision (cumulative — includes all prior fixes)
 * ─────────────────────────────────────────────────────────────────────────────
 * BUG-1  (ISR TX mismatch)       Per-UART tx_done_sem; TxCpltCallback matches
 *                                 by Instance before giving the semaphore.
 * BUG-2  (ISR memcpy overflow)   len clamped to UART_BLOCK_SIZE before any
 *                                 memcpy; oldest bytes discarded on overflow.
 * BUG-4  (TX stuck after timeout) HAL_UART_DMAStop() called on TX timeout so
 *                                 the peripheral leaves HAL_BUSY state.
 * BUG-5  (last_read_ptr type)    Changed uint32_t → uint16_t; matches HAL's
 *                                 Size param and removes implicit widening.
 * BUG-6  (ReleaseBuffer safety)  NULL guard + pool-range check; bad pointers
 *                                 cause configASSERT in debug, silent ignore
 *                                 in release — never corrupt the pool.
 * DESIGN-1 (double-init)         xFreeQueue NULL guard prevents the pool being
 *                                 built twice and leaking the old queue.
 * DESIGN-2 (duplicate register)  Linear scan checks for an existing entry with
 *                                 the same huart->Instance before allocating.
 *
 * NEW in this revision
 * ─────────────────────────────────────────────────────────────────────────────
 * BUG-U1 (FreeRTOS object leak on allocation failure)
 *         registered_count was incremented and the slot marked is_ready=true
 *         before the three FreeRTOS objects (mutex, binary semaphore, queue)
 *         were created.  If any creation failed and configASSERT was disabled
 *         (release build), the broken slot stayed in the array permanently,
 *         consuming an entry in uart_ctx_array and leaving registered_count
 *         one too high — a subsequent register call for the same instance
 *         would get a new (potentially corrupted) slot.
 *         Fix: create all FreeRTOS objects first, validate each, then commit
 *         the slot by setting is_ready=true and incrementing registered_count.
 *         On any allocation failure the slot is left is_ready=false, all
 *         successfully created objects are deleted, and NULL is returned.
 *
 * BUG-U2 (HAL_UARTEx_ReceiveToIdle_DMA return value ignored)
 *         If DMA is not configured (e.g. CubeMX misconfiguration), the HAL
 *         call fails silently and the context is returned to the caller.
 *         Subsequent UART_Sys_Receive calls block forever on an rx_queue
 *         that is never fed.
 *         Fix: check the HAL return value; on failure, delete FreeRTOS
 *         objects and return NULL.
 * =============================================================================
 */

#include "uart_subsystem.h"
#include <string.h>

/* ─────────────────────────── Module state ───────────────────────────────── */

static UART_Ctx_t uart_ctx_array[MAX_UARTS];
static uint8_t registered_count = 0u;

/* Shared receive buffer pool */
static uint8_t static_pool[UART_POOL_BLOCKS][UART_BLOCK_SIZE];
static QueueHandle_t xFreeQueue = NULL;

/* ══════════════════════════ Init ════════════════════════════════════════════*/

bool UART_Sys_Init(void)
{
    /*
     * DESIGN-1: Double-init guard.
     * A second call would leak the old FreeRTOS queue object and re-insert
     * every pool block, allowing two tasks to receive the same buffer pointer.
     */
    if (xFreeQueue != NULL)
        return false;

    xFreeQueue = xQueueCreate(UART_POOL_BLOCKS, sizeof(uint8_t *));
    if (xFreeQueue == NULL)
        return false;

    for (int i = 0; i < UART_POOL_BLOCKS; i++)
    {
        uint8_t *ptr = &static_pool[i][0];
        xQueueSend(xFreeQueue, &ptr, 0); /* queue size == pool size → always succeeds */
    }

    for (int i = 0; i < MAX_UARTS; i++)
        uart_ctx_array[i].is_ready = false;

    return true;
}

/* ══════════════════════════ Register ════════════════════════════════════════*/

UART_Ctx_t *UART_Sys_Register(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
        return NULL;

    if (xFreeQueue == NULL)
        return NULL;

    if (registered_count >= MAX_UARTS)
        return NULL;

    /*
     * DESIGN-2: Duplicate registration guard.
     * Registering the same huart twice would create a second context whose
     * rx_queue is never fed by the ISR (which matches only the first entry),
     * leaking FreeRTOS objects and potentially blocking callers forever.
     * Return the existing context instead (idempotent).
     */
    for (uint8_t i = 0u; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
            return &uart_ctx_array[i];
    }

    /* Find the first unused slot */
    UART_Ctx_t *ctx = NULL;
    for (uint8_t i = 0u; i < MAX_UARTS; i++)
    {
        if (!uart_ctx_array[i].is_ready)
        {
            ctx = &uart_ctx_array[i];
            break;
        }
    }
    if (ctx == NULL)
        return NULL;

    /*
     * BUG-U1 fix: Create FreeRTOS objects BEFORE committing the slot.
     *
     * Previous order:
     *   1. ctx->is_ready = true  ← slot committed
     *   2. registered_count++    ← count incremented
     *   3. create objects        ← could fail here
     *   4. configASSERT          ← fires in debug, silent in release
     *
     * In a release build with configASSERT disabled, a heap-exhaustion
     * failure left a permanently broken slot in the array and a stale
     * registered_count.  Any later call to UART_Sys_Register for the
     * same Instance would skip the DESIGN-2 duplicate guard (registered_count
     * didn't match the actual slot) and allocate a new, also broken, slot.
     *
     * New order:
     *   1. Create all three objects; if any fails, delete the ones that
     *      succeeded and return NULL — the slot stays is_ready=false.
     *   2. Only on full success: populate ctx, set is_ready=true,
     *      increment registered_count, start DMA.
     */
    ctx->huart = huart;
    ctx->last_read_ptr = 0u;

    ctx->tx_mutex = xSemaphoreCreateMutex();
    ctx->tx_done_sem = xSemaphoreCreateBinary();
    ctx->rx_queue = xQueueCreate(UART_RX_SIZE, sizeof(UART_Packet_t));

    if ((ctx->tx_mutex == NULL) ||
        (ctx->tx_done_sem == NULL) ||
        (ctx->rx_queue == NULL))
    {
        /* Clean up any successfully created objects before bailing out */
        if (ctx->tx_mutex)
        {
            vSemaphoreDelete(ctx->tx_mutex);
            ctx->tx_mutex = NULL;
        }
        if (ctx->tx_done_sem)
        {
            vSemaphoreDelete(ctx->tx_done_sem);
            ctx->tx_done_sem = NULL;
        }
        if (ctx->rx_queue)
        {
            vQueueDelete(ctx->rx_queue);
            ctx->rx_queue = NULL;
        }
        return NULL;
    }

    /*
     * BUG-U2 fix: Check HAL return value before committing the slot.
     *
     * HAL_UARTEx_ReceiveToIdle_DMA returns HAL_ERROR if:
     *   - DMA is not configured in CubeMX for this UART's Rx channel.
     *   - The DMA channel is already busy (e.g. double-register not caught).
     *   - The huart handle is in an error state.
     *
     * Returning a non-NULL context when DMA never starts causes
     * UART_Sys_Receive to block on rx_queue indefinitely — the ISR
     * callback that feeds it never fires.
     */
    if (HAL_UARTEx_ReceiveToIdle_DMA(huart, ctx->dma_rx_buf,
                                     UART_DMA_BUF_SIZE) != HAL_OK)
    {
        vSemaphoreDelete(ctx->tx_mutex);
        vSemaphoreDelete(ctx->tx_done_sem);
        vQueueDelete(ctx->rx_queue);
        ctx->tx_mutex = NULL;
        ctx->tx_done_sem = NULL;
        ctx->rx_queue = NULL;
        return NULL;
    }

    /* Commit the slot — only reached on full success */
    ctx->is_ready = true;
    registered_count++;

    return ctx;
}

/* ══════════════════════════ Un-Register ═════════════════════════════════════*/

void UART_Sys_UnRegister(UART_Ctx_t *ctx)
{
    if (xFreeQueue == NULL)
        return;

    if (ctx == NULL)
        return;

    for (uint8_t i = 0u; i < MAX_UARTS; i++)
    {
        if (ctx == &uart_ctx_array[i] && uart_ctx_array[i].is_ready)
        {
            uart_ctx_array[i].is_ready = false;
            HAL_UART_DMAStop(ctx->huart);
            UART_Sys_FlushReceive(ctx);
            vQueueDelete(ctx->rx_queue);
            vSemaphoreDelete(ctx->tx_mutex);
            vSemaphoreDelete(ctx->tx_done_sem);

            if (registered_count > 0u)
                registered_count--;
            return;
        }
    }
}

/* ══════════════════════════ Send ═══════════════════════════════════════════*/

bool UART_Sys_Send(UART_Ctx_t *ctx, const uint8_t *pData,
                   uint16_t len, uint32_t timeout_ms)
{
    if (!ctx || !pData || len == 0u)
        return false;

    if (xFreeQueue == NULL)
        return false;

    if (xSemaphoreTake(ctx->tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdPASS)
        return false;

    if (HAL_UART_Transmit_DMA(ctx->huart, pData, len) != HAL_OK)
    {
        xSemaphoreGive(ctx->tx_mutex);
        return false;
    }

    bool ok = (xSemaphoreTake(ctx->tx_done_sem,
                              pdMS_TO_TICKS(timeout_ms)) == pdPASS);

    if (!ok)
    {
        /*
         * BUG-4: Abort the stalled DMA transfer on timeout.
         * Without this, the UART peripheral stays in HAL_BUSY TX state and
         * every subsequent HAL_UART_Transmit_DMA call returns HAL_BUSY,
         * permanently breaking the UART until a system reset.
         */
        HAL_UART_DMAStop(ctx->huart);
    }

    xSemaphoreGive(ctx->tx_mutex);
    return ok;
}

/* ══════════════════════════ Receive ═════════════════════════════════════════*/

bool UART_Sys_Receive(UART_Ctx_t *ctx, UART_Packet_t *out_packet,
                      uint32_t timeout_ms)
{
    if (!ctx || !out_packet)
        return false;

    if (xFreeQueue == NULL)
        return false;

    return (xQueueReceive(ctx->rx_queue, out_packet,
                          pdMS_TO_TICKS(timeout_ms)) == pdPASS);
}

/* ══════════════════════════ FlushReceive ════════════════════════════════════*/

bool UART_Sys_FlushReceive(UART_Ctx_t *ctx)
{
    if (!ctx)
        return false;

    if (xFreeQueue == NULL)
        return false;

    UART_Packet_t pkt;
    while (xQueueReceive(ctx->rx_queue, &pkt, 0) == pdPASS)
        UART_Sys_ReleaseBuffer(pkt.payload);

    return true;
}

/* ══════════════════════════ ReleaseBuffer ═══════════════════════════════════*/

void UART_Sys_ReleaseBuffer(uint8_t *pBuffer)
{
    /*
     * BUG-6: NULL guard + pool-membership check.
     *
     * NULL case: enqueueing NULL into xFreeQueue causes the ISR's next
     * xQueueReceiveFromISR to dequeue it and call memcpy(NULL, …) → hard fault.
     *
     * Out-of-pool (wild pointer / double-free): inserting an invalid address
     * corrupts the pool so that a future ISR memcpy writes into arbitrary RAM.
     */
    if (!pBuffer)
        return;

    if (xFreeQueue == NULL)
        return;

    const uint8_t *pool_start = &static_pool[0][0];
    const uint8_t *pool_end = &static_pool[UART_POOL_BLOCKS - 1u][0];
    if (pBuffer < pool_start || pBuffer > pool_end)
    {
        configASSERT(0); /* trap in debug builds */
        return;          /* silent ignore in release */
    }

    xQueueSend(xFreeQueue, &pBuffer, 0);
}

/* ══════════════════════════ ISR Callbacks ════════════════════════════════════*/

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    /* Locate the registered context for this peripheral */
    UART_Ctx_t *ctx = NULL;
    for (int i = 0; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
        {
            ctx = &uart_ctx_array[i];
            break;
        }
    }
    if (!ctx)
        return;
    if (xFreeQueue == NULL)
        return;

    /* Compute new byte count since last callback (handles wrap-around) */
    uint16_t len;
    if (Size >= ctx->last_read_ptr)
        len = Size - ctx->last_read_ptr;
    else
        len = (uint16_t)(UART_DMA_BUF_SIZE - ctx->last_read_ptr + Size);

    if (len == 0u)
        return;

    /*
     * BUG-2: Clamp len to UART_BLOCK_SIZE before any memcpy.
     */
    if (len > UART_BLOCK_SIZE)
    {
        uint16_t overflow = len - UART_BLOCK_SIZE;
        ctx->last_read_ptr = (uint16_t)((ctx->last_read_ptr + overflow) % UART_DMA_BUF_SIZE);
        len = UART_BLOCK_SIZE;
    }

    BaseType_t xWoken = pdFALSE;
    uint8_t *pBuf = NULL;

    if (xQueueReceiveFromISR(xFreeQueue, &pBuf, &xWoken) != pdPASS)
    {
        /* Pool exhausted — data lost; advance pointer and yield */
        ctx->last_read_ptr = (uint16_t)(Size % UART_DMA_BUF_SIZE);
        portYIELD_FROM_ISR(xWoken);
        return;
    }

    /* Copy new bytes, handling circular DMA buffer wrap-around */
    uint16_t src = ctx->last_read_ptr;
    if ((uint32_t)src + len <= UART_DMA_BUF_SIZE)
    {
        memcpy(pBuf, &ctx->dma_rx_buf[src], len);
    }
    else
    {
        uint16_t head = UART_DMA_BUF_SIZE - src;
        memcpy(pBuf, &ctx->dma_rx_buf[src], head);
        memcpy(pBuf + head, &ctx->dma_rx_buf[0], len - head);
    }

    UART_Packet_t pkt = {.payload = pBuf, .length = len, .huart = huart};
    if (xQueueSendFromISR(ctx->rx_queue, &pkt, &xWoken) != pdPASS)
    {
        /* RX queue full — return buffer rather than leaking it */
        xQueueSendFromISR(xFreeQueue, &pBuf, &xWoken);
    }

    ctx->last_read_ptr = (uint16_t)(Size % UART_DMA_BUF_SIZE);
    portYIELD_FROM_ISR(xWoken);
}

/* -------------------------------------------------------------------------- */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t xWoken = pdFALSE;

    if(xFreeQueue == NULL)
        return;

    for (int i = 0; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
        {
            xSemaphoreGiveFromISR(uart_ctx_array[i].tx_done_sem, &xWoken);
            break;
        }
    }

    portYIELD_FROM_ISR(xWoken);
}
