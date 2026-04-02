/**
 * @file    modbus.h
 * @brief   Modbus RTU master — public interface.
 *
 * Supported function codes
 * ────────────────────────
 *   0x03  Read Holding Registers  (modbus_read_register)
 *   0x06  Write Single Register   (modbus_write_register)
 *
 * Slave devices on this RS-485 bus
 * ──────────────────────────────────
 *   ADDR_SEM228P  0x01  — Light sensor
 *   ADDR_R66S     0x02  — Rain gauge
 *
 * Usage
 * ─────
 *   // In your FreeRTOS task initialisation:
 *   modbus_init(&huart2);
 *
 *   uint16_t lux[2];
 *   if (modbus_read_register(ADDR_SEM228P, 0x0000, lux, 2))
 *       process(lux);
 *
 *   modbus_write_register(ADDR_R66S, 0x0010, 0x0001);
 */

#ifndef __MODBUS_H
#define __MODBUS_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"
#include "uart_subsystem.h"

/* ─────────────────────────── Public API ─────────────────────────────────── */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief  Initialise the Modbus module.
     *
     * Calls UART_Sys_Init() internally; safe to call even if UART_Sys_Init()
     * was already called elsewhere.  Also configures the STM32L4 hardware CRC
     * unit for Modbus RTU CRC-16 (polynomial 0x8005, init 0xFFFF).
     *
     * @param  huart  HAL UART handle for the RS-485 peripheral.
     *                The UART must have "RS485 Driver Enable" (DE) configured
     *                in CubeMX; the HAL asserts DE automatically via UART_CR3_DEM.
     * @return true on success, false if huart is NULL or UART registration fails.
     */
    bool modbus_init(UART_HandleTypeDef *huart);

    /**
     * @brief  Read one or more holding registers from a slave (FC03).
     *
     * @param  addr     Slave address (1–247).
     * @param  reg_num  Starting register address (0-based, big-endian on wire).
     * @param  data     Caller-supplied buffer; must hold at least @p len uint16_t.
     * @param  len      Number of 16-bit registers to read.
     * @return true if the response was received and passed address + CRC checks.
     */
    bool modbus_read_register(uint8_t addr, uint16_t reg_num,
                              uint16_t *data, uint16_t len);

    /**
     * @brief  Write a single holding register on a slave (FC06).
     *
     * The slave echoes the request unchanged; this function validates the echo
     * including the CRC before returning true.
     *
     * @param  addr     Slave address (1–247).
     * @param  reg_num  Register address (0-based).
     * @param  data     16-bit value to write.
     * @return true if the slave echo was received and passed all validation checks.
     */
    bool modbus_write_register(uint8_t addr, uint16_t reg_num, uint16_t data);

#ifdef __cplusplus
}
#endif

#endif /* __MODBUS_H */
