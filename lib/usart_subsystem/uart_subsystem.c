#include "uart_subsystem.h"
#include <string.h>

/* ─────────────────────────── Module state ───────────────────────────────── */

static UART_Ctx_t  uart_ctx_array[MAX_UARTS];
static uint8_t     registered_count = 0;

/* Shared receive buffer pool */
static uint8_t       static_pool[UART_POOL_BLOCKS][UART_BLOCK_SIZE];
static QueueHandle_t xFreeQueue = NULL;

/* ══════════════════════════ Init ════════════════════════════════════════════*/

void UART_Sys_Init(void)
{
    /*
     * FIX (DESIGN-1): double-init guard.
     *
     * Without this, a second call would overwrite xFreeQueue with a new
     * handle, leaking the old FreeRTOS queue object and re-inserting every
     * pool block a second time. Any block allocated from the new queue could
     * then be "allocated" again from the leaked old queue, causing two callers
     * to memcpy into the same physical buffer simultaneously.
     */
    if (xFreeQueue != NULL)
        return;

    xFreeQueue = xQueueCreate(UART_POOL_BLOCKS, sizeof(uint8_t *));
    configASSERT(xFreeQueue);

    for (int i = 0; i < UART_POOL_BLOCKS; i++)
    {
        uint8_t *ptr = &static_pool[i][0];
        xQueueSend(xFreeQueue, &ptr, 0);
    }
}

/* ══════════════════════════ Register ════════════════════════════════════════*/

UART_Ctx_t *UART_Sys_Register(UART_HandleTypeDef *huart)
{
    if (!huart)
        return NULL;

    if (registered_count >= MAX_UARTS)
        return NULL;

    /*
     * FIX (DESIGN-2): duplicate registration guard.
     *
     * Registering the same huart twice would allocate a second UART_Ctx_t
     * entry, creating rx_queue and tx_mutex objects that the ISR never uses
     * (it always matches the first entry). The leaked objects consume heap,
     * and any task mistakenly using the second context would block forever
     * since its rx_queue never receives data.
     *
     * If the same huart is registered again, return its existing context
     * (idempotent behaviour matching what the ISR actually does).
     */
    for (uint8_t i = 0; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
            return &uart_ctx_array[i];
    }

    UART_Ctx_t *ctx    = &uart_ctx_array[registered_count++];
    ctx->huart         = huart;
    ctx->last_read_ptr = 0;

    ctx->tx_mutex    = xSemaphoreCreateMutex();
    ctx->tx_done_sem = xSemaphoreCreateBinary();  /* FIX BUG-1: per-UART */
    ctx->rx_queue    = xQueueCreate(UART_RX_SIZE, sizeof(UART_Packet_t));

    configASSERT(ctx->tx_mutex);
    configASSERT(ctx->tx_done_sem);
    configASSERT(ctx->rx_queue);

    HAL_UARTEx_ReceiveToIdle_DMA(huart, ctx->dma_rx_buf, UART_DMA_BUF_SIZE);
    return ctx;
}

/* ══════════════════════════ Send ═══════════════════════════════════════════*/

bool UART_Sys_Send(UART_Ctx_t *ctx, const uint8_t *pData,
                   uint16_t len, uint32_t timeout_ms)
{
    if (!ctx || !pData || len == 0)
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
         * FIX (BUG-4): abort the stalled DMA transfer on timeout.
         *
         * Without this, the UART peripheral stays in a HAL_BUSY TX state.
         * The next UART_Sys_Send would call HAL_UART_Transmit_DMA on a busy
         * peripheral (returns HAL_BUSY), silently fail, and the UART would
         * be permanently broken until a system reset.
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

    return (xQueueReceive(ctx->rx_queue, out_packet,
                          pdMS_TO_TICKS(timeout_ms)) == pdPASS);
}

/* ══════════════════════════ FlushReceive ════════════════════════════════════*/

bool UART_Sys_FlushReceive(UART_Ctx_t *ctx)
{
    if (!ctx)
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
     * FIX (BUG-6): NULL guard and pool-membership check.
     *
     * Original code:  xQueueSend(xFreeQueue, &pBuffer, 0)  with no checks.
     *
     * NULL case: a NULL pointer would be enqueued into xFreeQueue. The next
     * ISR xQueueReceiveFromISR would dequeue it, then memcpy(NULL, ...) → hard fault.
     *
     * Out-of-pool pointer (wild pointer, double-free): inserting an invalid
     * address corrupts the pool — future ISR allocations would memcpy into
     * arbitrary memory.
     *
     * The range check compares against the first byte of the first and last
     * rows. It does not validate alignment within a row, but it catches the
     * most common misuse cases. In debug builds the assert fires immediately;
     * in release builds the bad pointer is silently ignored rather than
     * corrupting the pool.
     */
    if (!pBuffer)
        return;

    const uint8_t *pool_start = &static_pool[0][0];
    const uint8_t *pool_end   = &static_pool[UART_POOL_BLOCKS - 1][0];
    if (pBuffer < pool_start || pBuffer > pool_end)
    {
        configASSERT(0);  /* trap in debug builds */
        return;           /* ignore silently in release builds */
    }

    xQueueSend(xFreeQueue, &pBuffer, 0);
}

/* ══════════════════════════ ISR Callbacks ════════════════════════════════════*/

/*
 * HAL_UARTEx_RxEventCallback — called by HAL on: DMA half-transfer, DMA
 * full-transfer, or UART idle-line event (whichever comes first).
 *
 * Size is the current DMA write position (number of bytes written to the
 * circular buffer since the last DMA restart, 1-based in circular mode).
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    /* Find the registered context for this peripheral instance */
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

    /*
     * Compute the number of new bytes since last_read_ptr.
     */
    uint16_t len;
    if (Size >= ctx->last_read_ptr)
        len = Size - ctx->last_read_ptr;
    else
        len = (uint16_t)(UART_DMA_BUF_SIZE - ctx->last_read_ptr + Size);

    if (len == 0)
        return;

    /*
     * FIX (BUG-2): clamp len to UART_BLOCK_SIZE before any memcpy.
     *
     * UART_DMA_BUF_SIZE (256) is intentionally 2× UART_BLOCK_SIZE (128) so
     * normal bursts always fit. However if the application task stalls and
     * the DMA fills more than 128 bytes between callbacks, len > 128. The
     * original code would then memcpy past the end of the 256-byte pool block
     * into the adjacent block (or BSS), silently corrupting memory.
     *
     * We keep the most-recently arrived bytes (the tail of the new data) by
     * advancing last_read_ptr forward before copying. This discards the
     * oldest overflow bytes, which is preferable to a memory corruption.
     */
    if (len > UART_BLOCK_SIZE)
    {
        uint16_t overflow = len - UART_BLOCK_SIZE;
        /* Advance the logical read pointer past the bytes we must discard */
        ctx->last_read_ptr = (uint16_t)((ctx->last_read_ptr + overflow)
                                        % UART_DMA_BUF_SIZE);
        len = UART_BLOCK_SIZE;
    }

    BaseType_t xWoken = pdFALSE;
    uint8_t   *pBuf   = NULL;

    if (xQueueReceiveFromISR(xFreeQueue, &pBuf, &xWoken) != pdPASS)
    {
        /* Pool exhausted — data is lost. Update pointer and yield. */
        ctx->last_read_ptr = (uint16_t)(Size % UART_DMA_BUF_SIZE);
        portYIELD_FROM_ISR(xWoken);
        return;
    }

    /* Copy new data, handling circular DMA buffer wrap-around */
    uint16_t src = ctx->last_read_ptr;
    if ((uint32_t)src + len <= UART_DMA_BUF_SIZE)
    {
        memcpy(pBuf, &ctx->dma_rx_buf[src], len);
    }
    else
    {
        uint16_t head = UART_DMA_BUF_SIZE - src;
        memcpy(pBuf,        &ctx->dma_rx_buf[src], head);
        memcpy(pBuf + head, &ctx->dma_rx_buf[0],   len - head);
    }

    UART_Packet_t pkt = { .payload = pBuf, .length = len, .huart = huart };
    if (xQueueSendFromISR(ctx->rx_queue, &pkt, &xWoken) != pdPASS)
    {
        /* RX queue full — return buffer rather than leaking it */
        xQueueSendFromISR(xFreeQueue, &pBuf, &xWoken);
    }

    ctx->last_read_ptr = (uint16_t)(Size % UART_DMA_BUF_SIZE);
    portYIELD_FROM_ISR(xWoken);
}

/* -------------------------------------------------------------------------- */

/*
 * HAL_UART_TxCpltCallback — called by HAL when DMA TX completes.
 *
 * FIX (BUG-1): identify the UART that completed and give only its
 * ctx->tx_done_sem. The original code gave a single global xTxDoneSem
 * without checking huart, so a fast UART completing first would wake a
 * task that was waiting on a completely different, still-running UART.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    BaseType_t xWoken = pdFALSE;

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
