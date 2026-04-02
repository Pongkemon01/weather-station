#ifndef __A7670_H
#define __A7670_H

#include <stdint.h>
#include <stdbool.h>
#include "uart_subsystem.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Core API */
bool modem_init(UART_HandleTypeDef *huart);
void modem_deinit(void);
bool modem_is_init(void);
QueueHandle_t modem_get_urc_queue(void);
bool modem_get_datetime(char *buf);
bool modem_set_datetime(const char *buf);

#ifdef __cplusplus
}
#endif

#endif // __A7670_H