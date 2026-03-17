#ifndef __CY15B116QN_H

#define __CY15B116QN_H

#include <stdbool.h>
#include <stdint.h>

#include "main.h"

// Modify these GPIO settings according to FRAM connectivity
#define FRAM_CS_PORT FRAM_CS_Port
#define FRAM_CS_PIN FRAM_CS_Pin

// FRAM specification
#define FRAM_TIMEOUT 100 // Timeout for each SPI transaction
#define FRAM_ADDR_BIT 21 // 2MB addressable space
#define FRAM_MAX_ADDR ((uint32_t)((1UL << FRAM_ADDR_BIT) - 1UL))

#ifdef __cplusplus
extern "C"
{
#endif

    bool fram_init(SPI_HandleTypeDef *hframspi);
    bool fram_write_enable(void);
    bool fram_write_disable(void);
    uint32_t fram_write(uint32_t addr, uint8_t *data, uint32_t len);
    uint32_t fram_read(uint32_t addr, uint8_t *data, uint32_t len);
    uint16_t fram_write_special_sector(uint8_t addr, uint8_t *data, uint16_t len);
    uint16_t fram_read_special_sector(uint8_t addr, uint8_t *data, uint16_t len);
    uint8_t fram_read_status_register(void);
    bool fram_write_status_register(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif //__CY15B116QN_H
