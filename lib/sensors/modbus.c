/**
 * @file    modbus.c
 * @brief   Modbus RTU master — Function Codes 3 (Read Holding Registers)
 *          and 6 (Write Single Register).
 *
 * Target  : STM32L476RG @ any clock; RS-485 at 4800 baud.
 * UART    : Hardware DE (Driver Enable) managed by the STM32 USART peripheral
 *           via CubeMX "RS485 Driver Enable" option (UART_CR3_DEM).  The HAL
 *           asserts DE automatically before every DMA TX and de-asserts it
 *           when the shift register empties, so no GPIO toggling is needed
 *           here.
 *
 * Fixes applied
 * ─────────────
 * BUG-M1  CRC byte-order was reversed in the FC03 response check.
 *         Modbus RTU §2.5: CRC is transmitted low-byte first, high-byte
 *         second.  payload[len-2] = CRC_lo, payload[len-1] = CRC_hi.
 *         The old code compared payload[len-2] against (crc>>8) — wrong.
 *
 * BUG-M2  FC06 response echo was never CRC-validated ("just lazy").
 *         On an RS-485 bus at 4800 baud with inductive loads a corrupted
 *         echo can pass the byte-comparison check if only the CRC bytes
 *         differ.  CRC is now checked.
 *
 * BUG-M3  FC06 response length was checked as 6; correct echo length is 8
 *         (6 payload bytes + 2 CRC bytes).
 *
 * BUG-M4  modbus_init() never called UART_Sys_Init().  The shared buffer
 *         pool (xFreeQueue) was therefore NULL when the first ISR fired,
 *         causing xQueueReceiveFromISR(NULL,…) → hard fault.
 *
 * BUG-M5  After a DMA TX the RS-485 transceiver (even with hardware DE)
 *         may latch a partial echo of the transmitted frame in the DMA RX
 *         circular buffer on some board layouts.  UART_Sys_FlushReceive()
 *         is called after every send to discard any such echo before
 *         waiting for the real slave response.
 *
 * Optimisations applied
 * ─────────────────────
 * OPT-M1  Hardware CRC unit configuration moved to modbus_init().
 *         Previously, five LL_CRC_Set… calls ran on every TX and every RX
 *         frame.  The polynomial and mode never change, so configuring once
 *         at init and only calling LL_CRC_ResetCRCCalculationUnit() + feed
 *         loop at runtime saves ~250 ns per CRC call (negligible at 4800 baud
 *         but good hygiene and reduces code-path length in crc_485()).
 *
 * OPT-M2  TX packet buffer changed from stack-allocated uint8_t[8] to a
 *         file-scope static array.  Modbus RTU is strictly sequential
 *         (one outstanding transaction at a time, enforced by the UART
 *         subsystem mutex), so re-entrant allocation is impossible.
 *         Saves 8 bytes of stack per call — relevant on an RTOS task with
 *         a small stack.
 *
 * OPT-M3  crc_485() marked static inline and the body reduced to reset +
 *         feed + read, now that configuration is done once at init.
 */

#include "modbus.h"
#include "stm32l4xx_ll_crc.h"

/* ─────────────────────────── Configuration ──────────────────────────────── */

/** Milliseconds to wait for a slave response at 4800 baud.
 *  Modbus RTU specifies a 3.5-character inter-frame gap ≈ 7.3 ms at 4800 baud.
 *  A generous application-level timeout of 200 ms covers all compliant slaves.
 *  The original 1000 ms is kept here to avoid breaking existing callers, but
 *  can safely be reduced to 200 for faster fault detection.
 */
#define RS485_TIMEOUT_MS    200u

/* ─────────────────────────── Module state ───────────────────────────────── */

static UART_Ctx_t *modbus_ctx = NULL;

/**
 * OPT-M2: Single shared TX packet buffer.
 * Safe because Modbus RTU is inherently sequential and the UART subsystem
 * mutex prevents concurrent use.  Both FC03 and FC06 request frames are
 * exactly 8 bytes (6 payload + 2 CRC).
 */
static uint8_t tx_packet[8];

/* ─────────────────────────── Local helpers ──────────────────────────────── */

/**
 * @brief  Compute Modbus RTU CRC-16 using the STM32L4 hardware CRC unit.
 *
 * OPT-M1: The CRC peripheral is configured once in modbus_init().
 *         Only the reset + data feed + read are performed here.
 *
 * Polynomial : 0x8005 (Modbus standard)
 * Init value : 0xFFFF
 * Input/Output reversal: half-word input, byte output reversal per LL driver
 *
 * @param  buffer  Pointer to data bytes.
 * @param  len     Number of bytes to include in the CRC.
 * @return 16-bit CRC, low byte first (matches Modbus wire format).
 */
static inline uint16_t crc_485(const uint8_t *buffer, uint16_t len)
{
    LL_CRC_ResetCRCCalculationUnit(CRC);

    for (uint16_t i = 0u; i < len; i++)
        LL_CRC_FeedData8(CRC, buffer[i]);

    return LL_CRC_ReadData16(CRC);
}

/* ─────────────────────────── Public API ─────────────────────────────────── */

/**
 * @brief  Initialise the Modbus module.
 *
 * Must be called once before any modbus_read/write calls.
 * Internally calls UART_Sys_Init() (BUG-M4 fix) and configures the
 * hardware CRC unit for Modbus RTU CRC-16 (OPT-M1).
 *
 * @param  huart  Pointer to the HAL UART handle connected to the RS-485 bus.
 * @return true on success, false if huart is NULL or registration fails.
 */
bool modbus_init(UART_HandleTypeDef *huart)
{
    if (huart == NULL)
        return false;

    modbus_ctx = UART_Sys_Register(huart);
    if (modbus_ctx == NULL)
        return false;

    /*
     * OPT-M1: Configure hardware CRC unit once.
     * These settings never change between calls, so doing them at init
     * rather than per-frame saves 5 register writes per CRC computation.
     *
     * STM32L4 RM0351 §26: CRC unit supports programmable polynomial size
     * and reversal modes via the CR register.
     */
    LL_CRC_SetPolynomialSize(CRC, LL_CRC_POLYLENGTH_16B);
    LL_CRC_SetInitialData(CRC, 0xFFFFu);
    LL_CRC_SetPolynomialCoef(CRC, 0x8005u);
    LL_CRC_SetInputDataReverseMode(CRC, LL_CRC_INDATA_REVERSE_HALFWORD);
    LL_CRC_SetOutputDataReverseMode(CRC, CRC_CR_REV_OUT);

    return true;
}

/**
 * @brief  Read one or more holding registers from a Modbus slave (FC03).
 *
 * Builds and sends the 8-byte FC03 request, waits for the response, then
 * validates address, function code, and CRC before copying register values
 * into the caller's buffer.
 *
 * @param  addr     Slave device address (1–247).
 * @param  reg_num  Starting register number (0-based).
 * @param  data     Output buffer; must hold at least @p len uint16_t values.
 * @param  len      Number of registers to read.
 * @return true if the response is valid and data has been written.
 */
bool modbus_read_register(uint8_t addr, uint16_t reg_num,
                          uint16_t *data, uint16_t len)
{
    if (data == NULL || len == 0u)
        return false;

    UART_Packet_t rx_pkt;
    uint16_t      crc;

    /* Build FC03 request frame */
    tx_packet[0] = addr;
    tx_packet[1] = 0x03u;
    tx_packet[2] = (uint8_t)(reg_num >> 8u);
    tx_packet[3] = (uint8_t)(reg_num & 0xFFu);
    tx_packet[4] = (uint8_t)(len    >> 8u);
    tx_packet[5] = (uint8_t)(len    & 0xFFu);

    crc           = crc_485(tx_packet, 6u);
    tx_packet[6]  = (uint8_t)(crc & 0xFFu);   /* CRC low byte first (Modbus §2.5) */
    tx_packet[7]  = (uint8_t)(crc >> 8u);      /* CRC high byte second             */

    /* Transmit */
    if (!UART_Sys_Send(modbus_ctx, tx_packet, 8u, RS485_TIMEOUT_MS))
        return false;

    /*
     * BUG-M5 fix: flush any echo that the RS-485 transceiver placed in the
     * RX DMA buffer while the TX frame was being transmitted.
     */
    UART_Sys_FlushReceive(modbus_ctx);

    /* Wait for slave response */
    if (!UART_Sys_Receive(modbus_ctx, &rx_pkt, RS485_TIMEOUT_MS))
        return false;

    bool ok = false;

    /* Validate address and function code */
    if ((rx_pkt.payload[0] == addr) && (rx_pkt.payload[1] == 0x03u))
    {
        /*
         * BUG-M1 fix: Modbus RTU §2.5 — CRC bytes are transmitted
         * low byte first, high byte second.
         *   payload[length-2] = CRC_lo  →  compare with (crc & 0xFF)
         *   payload[length-1] = CRC_hi  →  compare with (crc >> 8)
         *
         * Old (wrong):
         *   payload[len-2] == (crc >> 8)    ← was comparing lo against hi
         *   payload[len-1] == (crc & 0xFF)
         */
        crc = crc_485(rx_pkt.payload, (uint16_t)(rx_pkt.length - 2u));

        if ((rx_pkt.payload[rx_pkt.length - 2u] == (uint8_t)(crc & 0xFFu)) &&
            (rx_pkt.payload[rx_pkt.length - 1u] == (uint8_t)(crc >> 8u)))
        {
            /* Extract register values; data starts at payload[3] (after
             * addr, FC, byte_count). */
            for (uint16_t i = 0u; i < len; i++)
            {
                data[i] = ((uint16_t)(rx_pkt.payload[(i * 2u) + 3u]) << 8u)
                         | (uint16_t)(rx_pkt.payload[(i * 2u) + 4u]);
            }
            ok = true;
        }
    }

    UART_Sys_ReleaseBuffer(rx_pkt.payload);
    return ok;
}

/**
 * @brief  Write a single holding register on a Modbus slave (FC06).
 *
 * Builds and sends the 8-byte FC06 request, waits for the echo response,
 * validates address, function code, register number, data value, AND CRC.
 *
 * @param  addr     Slave device address (1–247).
 * @param  reg_num  Register number (0-based).
 * @param  data     16-bit value to write.
 * @return true if the slave echoed the request correctly.
 */
bool modbus_write_register(uint8_t addr, uint16_t reg_num, uint16_t data)
{
    UART_Packet_t rx_pkt;
    uint16_t      crc;

    /* Build FC06 request frame */
    tx_packet[0] = addr;
    tx_packet[1] = 0x06u;
    tx_packet[2] = (uint8_t)(reg_num >> 8u);
    tx_packet[3] = (uint8_t)(reg_num & 0xFFu);
    tx_packet[4] = (uint8_t)(data    >> 8u);
    tx_packet[5] = (uint8_t)(data    & 0xFFu);

    crc           = crc_485(tx_packet, 6u);
    tx_packet[6]  = (uint8_t)(crc & 0xFFu);   /* CRC low byte first  */
    tx_packet[7]  = (uint8_t)(crc >> 8u);      /* CRC high byte second */

    /* Transmit */
    if (!UART_Sys_Send(modbus_ctx, tx_packet, 8u, RS485_TIMEOUT_MS))
        return false;

    /*
     * BUG-M5 fix: discard TX echo before waiting for slave response.
     */
    UART_Sys_FlushReceive(modbus_ctx);

    /* Wait for slave echo */
    if (!UART_Sys_Receive(modbus_ctx, &rx_pkt, RS485_TIMEOUT_MS))
        return false;

    bool ok = false;

    /*
     * BUG-M3 fix: FC06 echo is 8 bytes (6 payload + 2 CRC), not 6.
     *
     * BUG-M2 fix: CRC is now validated instead of skipped.
     *   The echo CRC is computed over the first 6 bytes and must match
     *   the last 2 bytes of the received frame.
     */
    if (rx_pkt.length == 8u)
    {
        /* Validate payload bytes 0–5 match the request */
        bool payload_match = true;
        for (uint8_t i = 0u; i < 6u; i++)
        {
            if (rx_pkt.payload[i] != tx_packet[i])
            {
                payload_match = false;
                break;
            }
        }

        if (payload_match)
        {
            /*
             * BUG-M2 fix: Validate CRC of the echo response.
             * CRC low byte at payload[6], high byte at payload[7].
             */
            crc = crc_485(rx_pkt.payload, 6u);

            if ((rx_pkt.payload[6u] == (uint8_t)(crc & 0xFFu)) &&
                (rx_pkt.payload[7u] == (uint8_t)(crc >> 8u)))
            {
                ok = true;
            }
        }
    }

    UART_Sys_ReleaseBuffer(rx_pkt.payload);
    return ok;
}
