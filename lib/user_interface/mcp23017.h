#ifndef __MCP23017_H
#define __MCP23017_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif
    bool mcp23017_init(I2C_HandleTypeDef *hi2c);
    bool mcp23017_bitbanging_write_data(uint8_t *data, uint16_t len);
    bool mcp23017_write_port_a(uint8_t data);
    bool mcp23017_write_port_b(uint8_t data);
    uint8_t mcp23017_read_port_a(void);
    uint8_t mcp23017_read_port_b(void);
    uint8_t mcp23017_read_latch_a(void);
    uint8_t mcp23017_read_latch_b(void);
#ifdef __cplusplus
}
#endif

#endif  /* __MCP23017_H */
