#ifndef CY15B116QN_H
#define CY15B116QN_H

/**
 * @file    cy15b116qn.h
 * @brief   Driver interface for Cypress CY15B116QN 2Mb SPI F-RAM
 */

#include <stdbool.h>
#include <stdint.h>
#include "main.h"

/* ---- Board-specific GPIO wiring -------------------------------- */
/*  Define FRAM_CS_GPIO_Port and FRAM_CS_Pin in main.h / CubeMX.   */
#define FRAM_CS_PORT  FRAM_CS_GPIO_Port
#define FRAM_CS_PIN   FRAM_CS_Pin

/* ---- Device parameters ---------------------------------------- */
#define FRAM_TIMEOUT    100u                                /* ms per SPI transaction       */
#define FRAM_ADDR_BITS  21u                                 /* 2 Mb = 2^21 addressable bytes */
#define FRAM_MAX_ADDR   ((uint32_t)((1UL << FRAM_ADDR_BITS) - 1UL))   /* 0x001FFFFF          */
#define FRAM_SPECIAL_SECTOR_SIZE  256u                      /* Special sector is exactly 256 bytes */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Public API ----------------------------------------------- */

bool     fram_init(SPI_HandleTypeDef *hframspi);

bool     fram_write_enable(void);
bool     fram_write_disable(void);

uint32_t fram_write(uint32_t addr, const uint8_t *data, uint32_t len);
uint32_t fram_read (uint32_t addr,       uint8_t *data, uint32_t len);

uint16_t fram_write_special_sector(uint8_t addr, const uint8_t *data, uint16_t len);
uint16_t fram_read_special_sector (uint8_t addr,       uint8_t *data, uint16_t len);

uint8_t  fram_read_status_register(void);
bool     fram_write_status_register(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* CY15B116QN_H */
