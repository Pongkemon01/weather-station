# USART Subsystem Migration: DMA → Interrupt-Based

## Context

The current USART subsystem uses DMA for all RX (circular) and TX transfers. For a system with only two active USART consumers — USART1 at 4800 baud (Modbus RTU) and USART3 at 115200 baud (A7670 LTE modem) — DMA adds unnecessary complexity (circular buffer bookkeeping, ReceiveToIdle coordination, pool-block bookkeeping per half-transfer) that an 80 MHz MCU handles trivially via per-byte interrupts. The new subsystem must be API-compatible — same public functions, same `UART_Ctx_t` consumers, same consumer code unchanged.

**Scope**: Only USART1 (Modbus, 4800 baud) and USART3 (Modem, 115200 baud) are registered via the subsystem. USART2 is the debug console using blocking `HAL_UART_Transmit` — completely outside the subsystem.

---

## Files Modified

| File | Change |
|------|--------|
| `lib/usart_subsystem/uart_subsystem.h` | Swap DMA-specific struct fields for interrupt equivalents |
| `lib/usart_subsystem/uart_subsystem.c` | Replace DMA API calls with interrupt equivalents |

**Not modified** (left exactly as-is):
- `Src/stm32l4xx_it.c` — DMA IRQ handlers stay (dormant but harmless)
- `Src/usart.c` — CubeMX DMA config stays (handles allocated but unused)
- `Src/dma.c` — DMA clocks and NVIC stay
- All consumer files (`modbus.c`, `a7670.c`, `a7670_at_channel.c`, etc.)

---

## Step 1: Backup

Copy `lib/usart_subsystem/` → `lib/usart_subsystem_dma_backup/`.

---

## Step 2: Header Changes (`uart_subsystem.h`)

**Only `UART_Ctx_t` struct fields change**. All constants (`MAX_UARTS`, `UART_DMA_BUF_SIZE`, `UART_BLOCK_SIZE`, `UART_POOL_BLOCKS`, `UART_RX_SIZE`), `UART_Packet_t`, and all public API signatures stay identical.

### Replace in `UART_Ctx_t`:

```c
// REMOVE these two fields:
uint8_t  dma_rx_buf[UART_DMA_BUF_SIZE];
uint16_t last_read_ptr;

// ADD these two fields:
uint8_t  rx_ring_buf[UART_DMA_BUF_SIZE];   // software ring buffer (same 256 B)
uint16_t rx_ring_wp;                         // next write position (0..255)
```

### Add TX tracking fields:

```c
const uint8_t *tx_buf_ptr;    // points into caller's buffer during TX
uint16_t       tx_remaining;  // bytes still to transmit
```

---

## Step 3: Source Changes (`uart_subsystem.c`)

### 3a. `UART_Sys_Register()` — replace DMA start with IT start

```c
// REPLACE:
if (HAL_UARTEx_ReceiveToIdle_DMA(huart, ctx->dma_rx_buf,
                                 UART_DMA_BUF_SIZE) != HAL_OK)
// WITH:
ctx->rx_ring_wp = 0;
ctx->tx_buf_ptr = NULL;
ctx->tx_remaining = 0;
if (HAL_UART_Receive_IT(huart, ctx->rx_ring_buf, 1u) != HAL_OK)
```

### 3b. `UART_Sys_Send()` — replace DMA TX with interrupt TX

```c
// REPLACE:
if (HAL_UART_Transmit_DMA(ctx->huart, pData, len) != HAL_OK)
// WITH:
ctx->tx_buf_ptr  = pData;       // const-cast: we only read from it
ctx->tx_remaining = len;
if (HAL_UART_Transmit_IT(ctx->huart, (uint8_t *)pData, 1u) != HAL_OK)
```

```c
// REPLACE timeout cleanup:
HAL_UART_DMAStop(ctx->huart);
// WITH:
HAL_UART_AbortTransmit(ctx->huart);
ctx->tx_remaining = 0;
```

### 3c. `UART_Sys_UnRegister()` — replace DMA stop with abort

```c
// REPLACE:
HAL_UART_DMAStop(ctx->huart);
// WITH:
HAL_UART_Abort(ctx->huart);
```

### 3d. ISR Callbacks

**`HAL_UART_TxCpltCallback`** — fires after each 1-byte TX chunk. Re-arms until all bytes sent, then signals semaphore:

```c
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (xFreeQueue == NULL)
        return;

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

    BaseType_t xWoken = pdFALSE;

    if (ctx->tx_remaining > 1u)          // more bytes to send
    {
        ctx->tx_remaining--;
        ctx->tx_buf_ptr++;
        HAL_UART_Transmit_IT(huart, (uint8_t *)ctx->tx_buf_ptr, 1u);
    }
    else                                  // last byte just completed
    {
        ctx->tx_remaining = 0u;
        xSemaphoreGiveFromISR(ctx->tx_done_sem, &xWoken);
    }

    portYIELD_FROM_ISR(xWoken);
}
```

**`HAL_UART_RxCpltCallback`** — fires on every single received byte. Ring-buffered, flushes when full:

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    UART_Ctx_t *ctx = NULL;
    for (int i = 0; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
        {
            ctx = &uart_ctx_array[i];
            break;
        }
    }
    if (!ctx || xFreeQueue == NULL)
        return;

    /* Store received byte into ring buffer */
    uint16_t wp = ctx->rx_ring_wp;
    ctx->rx_ring_buf[wp] = ctx->rx_ring_buf[0];  // HAL wrote byte to [0]
    wp = (wp + 1u) % UART_DMA_BUF_SIZE;
    ctx->rx_ring_wp = wp;

    /* Re-arm for next byte (HAL state is READY after callback) */
    HAL_UART_Receive_IT(huart, ctx->rx_ring_buf, 1u);

    /* If ring is full, flush now — can't wait for IDLE */
    if (ctx->rx_ring_wp == 0u)
        rx_ring_flush(ctx);
}
```

**`HAL_UARTEx_RxEventCallback`** — fires on IDLE line detection. Flushes accumulated ring data to the RX queue:

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    UART_Ctx_t *ctx = NULL;
    for (int i = 0; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
        {
            ctx = &uart_ctx_array[i];
            break;
        }
    }
    if (!ctx || xFreeQueue == NULL)
        return;

    (void)Size;  /* unused — we track position via rx_ring_wp */

    if (ctx->rx_ring_wp > 0u)
    {
        rx_ring_flush(ctx);
        ctx->rx_ring_wp = 0u;
    }

    /* Re-arm for next bytes */
    HAL_UART_Receive_IT(huart, ctx->rx_ring_buf, 1u);
}
```

**`rx_ring_flush`** — static helper replacing the inline memcpy logic:

```c
static void rx_ring_flush(UART_Ctx_t *ctx)
{
    uint16_t len = ctx->rx_ring_wp;
    if (len == 0u)
        return;

    /* BUG-2: clamp to UART_BLOCK_SIZE */
    if (len > UART_BLOCK_SIZE)
        len = UART_BLOCK_SIZE;

    BaseType_t xWoken = pdFALSE;
    uint8_t *pBuf = NULL;

    if (xQueueReceiveFromISR(xFreeQueue, &pBuf, &xWoken) != pdPASS)
    {
        /* Pool exhausted — drop data */
        portYIELD_FROM_ISR(xWoken);
        return;
    }

    memcpy(pBuf, ctx->rx_ring_buf, len);

    UART_Packet_t pkt = {.payload = pBuf, .length = len, .huart = ctx->huart};
    if (xQueueSendFromISR(ctx->rx_queue, &pkt, &xWoken) != pdPASS)
    {
        /* Queue full — return buffer to pool */
        xQueueSendFromISR(xFreeQueue, &pBuf, &xWoken);
    }

    portYIELD_FROM_ISR(xWoken);
}
```

**`HAL_UART_ErrorCallback`** — restart RX receive on error:

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    UART_Ctx_t *ctx = NULL;
    for (int i = 0; i < registered_count; i++)
    {
        if (uart_ctx_array[i].huart->Instance == huart->Instance)
        {
            ctx = &uart_ctx_array[i];
            break;
        }
    }
    if (!ctx || xFreeQueue == NULL)
        return;

    ctx->rx_ring_wp = 0u;
    HAL_UART_Receive_IT(ctx->huart, ctx->rx_ring_buf, 1u);
}
```

---

## Step 4: Verification

### Build
```bash
platformio run
```
Expected: 0 errors, 0 warnings.

### Functional test (flash + serial monitor)
1. `platformio run -t upload`
2. `pio device monitor -b 115200` — verify console output
3. Modbus reads succeed — light/rain sensor data appears in logs
4. Modem init succeeds — A7670 AT OK, NTP sync, cert injection
5. Sensor data written to FRAM
6. HTTPS upload succeeds at noon/midnight

### Memory
```bash
scripts/monitor_ram.sh
```
Expected: SRAM usage unchanged or slightly lower (DMA buffers dormant, ring buffers same size).

### Rollback
Copy `lib/usart_subsystem_dma_backup/` → `lib/usart_subsystem/`, rebuild.

---

## Design Decisions

1. **1-byte-at-a-time RX** (`HAL_UART_Receive_IT`, len=1): HAL state machine allows re-arm from `RxCpltCallback`. At 115200 baud, one interrupt every 87 μs — negligible on 80 MHz Cortex-M4.

2. **Software ring buffer** (256 B): Same capacity as old DMA buffer. Full-ring triggers immediate flush (matches old BUG-2 overflow behavior). IDLE detection triggers normal flush.

3. **1-byte-at-a-time TX** with re-arm from `TxCpltCallback`: Safe because `UART_Sys_Send` blocks on `tx_done_sem` — caller's buffer is alive for the entire transfer.

4. **CubeMX config untouched**: DMA handles allocated but no transfer is initiated. Rollback is a simple file copy.

5. **USART2 not in scope**: Debug console uses blocking `HAL_UART_Transmit` — never touches the subsystem.
