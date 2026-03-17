#ifndef __A7670_H
#define __A7670_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Core API */
bool modem_init(UART_Ctx_t *ctx);
void modem_deinit(void);
bool modem_is_init(void);
QueueHandle_t modem_get_urc_queue(void);

#ifdef __cplusplus
}
#endif

#endif // __A7670_H