/**
 * @file    boot_fram.h
 * @brief   Minimal polling SPI1 driver for CY15B116QN FRAM — bootloader only.
 *
 * This driver is a stripped-down subset of the application's cy15b116qn
 * driver.  It uses only HAL_SPI_Transmit / HAL_SPI_Receive in blocking
 * (polling) mode — no DMA, no interrupts, no FreeRTOS.
 *
 * Supported operations:
 *   boot_fram_init()   — store SPI handle, verify bus with WREN cycle
 *   boot_fram_read()   — CMD_READ (0x03) + 24-bit address
 *   boot_fram_write()  — CMD_WREN (0x06) + CMD_WRITE (0x02) + 24-bit address
 *
 * Pin assignment (matches application; defined in bootloader/Inc/main.h):
 *   CS  = PA4, SCK = PA5, MISO = PA6, MOSI = PA7  (SPI1, AF5)
 *
 * CONCURRENCY: No mutex used.  The bootloader is single-threaded; the
 * caller must not invoke these functions from an ISR.
 */

#ifndef BOOT_FRAM_H
#define BOOT_FRAM_H

#include <stdbool.h>
#include <stdint.h>
#include "main.h"   /* SPI_HandleTypeDef, FRAM_CS_Pin, FRAM_CS_GPIO_Port */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise the FRAM driver with the given SPI handle.
 *
 * Verifies the bus is alive by performing a Write-Enable cycle.
 *
 * @param  hspi  Pointer to a fully configured HAL SPI1 handle.
 * @return true on success, false if the SPI transaction fails.
 */
bool boot_fram_init(SPI_HandleTypeDef *hspi);

/**
 * @brief  Read bytes from the FRAM main array.
 *
 * @param  addr  Byte address in the main array (21-bit, 0x000000–0x1FFFFF).
 * @param  data  Destination buffer.
 * @param  len   Number of bytes to read.
 * @return Number of bytes actually read (equals len on success, 0 on error).
 */
uint32_t boot_fram_read(uint32_t addr, uint8_t *data, uint32_t len);

/**
 * @brief  Write bytes to the FRAM main array.
 *
 * Issues WREN before the WRITE command, as the CY15B116QN requires.
 *
 * @param  addr  Byte address in the main array (21-bit).
 * @param  data  Source buffer.
 * @param  len   Number of bytes to write.
 * @return Number of bytes actually written (equals len on success, 0 on error).
 */
uint32_t boot_fram_write(uint32_t addr, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_FRAM_H */
