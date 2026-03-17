#ifndef __MODBUS_H
#define __MODBUS_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"
#include "uart_subsystem.h"

/* Device address of sensors that share rs485 connection */
#define ADDR_SEM228P    0x01        // Address of light sensor
#define ADDR_R66S       0x02        // Address of rain guage

#ifdef __cplusplus
extern "C"
{
#endif

    bool modbus_init(UART_HandleTypeDef *huart);
    bool modbus_read_register(uint8_t addr, uint16_t reg_num, uint16_t *data, uint16_t len); // len = Total uint16_t
    bool modbus_write_register(uint8_t addr, uint16_t reg_num, uint16_t data);

#ifdef __cplusplus
}
#endif

#endif  /* __MODBUS_H */