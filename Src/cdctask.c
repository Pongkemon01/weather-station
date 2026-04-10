#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "nv_database.h"
#include "weather_data.h"
#include "datetime.h"
#include "main.h"

/*-------------------------------------------------------------------
 * USB Protocol:
    * CDC class with a single interface (interface 0).
    * Message format: Binary format, max 64 bytes (CDC packet size).
    * Field definitions:
        * MAGIC (2 bytes): 0xDCB0 (big-endian) for host->device and 0x55AA for device->host to identify the valid message.
        * DATETIME (6 bytes): RTC_DateTime_t struct representing the RTC time.
        * WEATHER_DATA (18 bytes): Weather_Data_Packed_t struct representing the weather data.
        * CONFIG (220 bytes): Meta_Data_t struct representing the station configuration.
        * STATUS (12 bytes): System_Ready_Status_t struct representing the system ready status.
        * CMD (1 byte): Command code from host to device.
            * 0x01: Request current weather data (device responds with a message containing the latest data).
            * 0x02: Request current configuration (device responds with the configuration in the CONFIG field).
            * 0x03: Request to update configuration (device expects a message with the new configuration in the CONFIG field).
            * 0x04: Request to set RTC time (device expects a message with the new RTC time in the DATETIME field).
            * 0x05: Request current system status (device responds with the status in the STATUS field).
            * 0x06: Request to flush the database (no additional data expected; device clears all stored weather data).
            * 0x07: Request to reset the device (no additional data expected; device performs a reset).
            * All other command codes are reserved and should be ignored by the device.
    * Packet format:
        [MAGIC (2 bytes)] [CMD (1 byte)] [PAYLOAD(optional) (up to 220 bytes)][~MAGIC (2 bytes)]
    * Any errors in the message format (invalid magic, unknown command, incorrect payload length)
        should be responded to with a NAK message containing an error code.
 *--------------------------------------------------------------------*/

/* -------------------------------------------------------------------------- */
/* Protocol Constants                                                         */
/* -------------------------------------------------------------------------- */
#define MAGIC_H2D_H 0xDC
#define MAGIC_H2D_L 0xB0
#define MAGIC_D2H_H 0x55
#define MAGIC_D2H_L 0xAA

#define CMD_REQ_WEATHER 0x01 // Weather data only
#define CMD_REQ_META 0x02    // Config only
#define CMD_SET_META 0x03    // Update Config
#define CMD_SET_RTC 0x04     // Set RTC time
#define CMD_REQ_STATUS 0x05  // Get System Status
#define CMD_DB_FLUSH 0x06    // Persistent Save
#define CMD_SYS_RESET 0x07   // Reboot
#define CMD_NAK 0xFF         // Error feedback

#define ERR_UNKNOWN_CMD 0x01
#define ERR_INVALID_FOOTER 0x02
#define ERR_WRITE_FAILED 0x03
#define ERR_INVALID_DATA 0x04

/* -------------------------------------------------------------------------- */
/* Memory Optimization: Static Shared Buffer                                  */
/* -------------------------------------------------------------------------- */
typedef enum
{
    STATE_MAGIC_H,
    STATE_MAGIC_L,
    STATE_CMD,
    STATE_PAYLOAD,
    STATE_FOOTER_H,
    STATE_FOOTER_L
} usb_state_t;

// Union allows us to handle the largest possible payload (Meta_Data_t, 220 bytes)
// without allocating multiple buffers or using large stack space.
static union
{
    Meta_Data_t meta;
    RTC_DateTime_t rtc;
    uint8_t raw[220];
} pg_buf;

static TaskHandle_t cdc_task_handle = NULL;
static usb_state_t parser_state = STATE_MAGIC_H;

/* -------------------------------------------------------------------------- */
/* Helper Functions (Stream-based)                                            */
/* -------------------------------------------------------------------------- */

static void usb_send_header(uint8_t cmd)
{
    uint8_t header[3] = {MAGIC_D2H_H, MAGIC_D2H_L, cmd};
    tud_cdc_write(header, 3);
}

static void usb_send_footer(void)
{
    uint8_t footer[2] = {(uint8_t)~MAGIC_D2H_H, (uint8_t)~MAGIC_D2H_L};
    tud_cdc_write(footer, 2);
    tud_cdc_write_flush();
}

static void usb_send_nak(uint8_t error_code)
{
    usb_send_header(CMD_NAK);
    tud_cdc_write(&error_code, 1);
    usb_send_footer();
}

/* -------------------------------------------------------------------------- */
/* Main Task Loop                                                             */
/* -------------------------------------------------------------------------- */

void cdc_task(void *params)
{
    (void)params;
    cdc_task_handle = xTaskGetCurrentTaskHandle();

    uint8_t current_cmd = 0;
    uint16_t rx_idx = 0;
    uint16_t expected_len = 0;

    while (1)
    {
        // Wait indefinitely for a notification from Callbacks (ISR)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (tud_cdc_available())
        {
            uint8_t b;
            tud_cdc_read(&b, 1);

            switch (parser_state)
            {
            case STATE_MAGIC_H:
                if (b == MAGIC_H2D_H)
                    parser_state = STATE_MAGIC_L;
                break;

            case STATE_MAGIC_L:
                parser_state = (b == MAGIC_H2D_L) ? STATE_CMD : STATE_MAGIC_H;
                break;

            case STATE_CMD:
                current_cmd = b;
                rx_idx = 0;

                if (current_cmd == CMD_SET_META)
                    expected_len = sizeof(Meta_Data_t);
                else if (current_cmd == CMD_SET_RTC)
                    expected_len = sizeof(RTC_DateTime_t);
                else
                    expected_len = 0;

                // Bounds check: if unknown command, NAK immediately
                if (current_cmd > CMD_SYS_RESET && current_cmd != CMD_NAK)
                {
                    usb_send_nak(ERR_UNKNOWN_CMD);
                    parser_state = STATE_MAGIC_H;
                }
                else
                {
                    parser_state = (expected_len > 0) ? STATE_PAYLOAD : STATE_FOOTER_H;
                }
                break;

            case STATE_PAYLOAD:
                pg_buf.raw[rx_idx++] = b;
                if (rx_idx >= expected_len)
                    parser_state = STATE_FOOTER_H;
                break;

            case STATE_FOOTER_H:
                if (b == (uint8_t)~MAGIC_H2D_H)
                    parser_state = STATE_FOOTER_L;
                else
                {
                    usb_send_nak(ERR_INVALID_FOOTER);
                    parser_state = STATE_MAGIC_H;
                }
                break;

            case STATE_FOOTER_L:
                if (b == (uint8_t)~MAGIC_H2D_L)
                {
                    /* --- COMMAND EXECUTION --- */
                    switch (current_cmd)
                    {
                    case CMD_REQ_WEATHER:
                    {
                        Weather_Data_Packed_t wp;
                        extern Weather_Data_t weather_data; // Defined in main/sensor task

                        PackData(&weather_data, &wp);

                        usb_send_header(CMD_REQ_WEATHER);
                        tud_cdc_write(&wp, sizeof(wp));
                        usb_send_footer();
                        break;
                    }

                    case CMD_REQ_META:
                        DB_GetMeta(&pg_buf.meta);
                        usb_send_header(CMD_REQ_META);
                        tud_cdc_write(&(pg_buf.meta), sizeof(Meta_Data_t));
                        usb_send_footer();
                        break;

                    case CMD_SET_META:
                        if (!DB_SetMeta(&(pg_buf.meta)))
                            usb_send_nak(ERR_WRITE_FAILED);
                        break;

                    case CMD_SET_RTC:
                        if (!datetime_set_rtc(&(pg_buf.rtc)))
                            usb_send_nak(ERR_INVALID_DATA);
                        break;

                    case CMD_REQ_STATUS:
                        usb_send_header(CMD_REQ_STATUS);
                        tud_cdc_write(&system_ready_status, sizeof(System_Ready_Status_t));
                        usb_send_footer();
                        break;

                    case CMD_DB_FLUSH:
                        if (!DB_Flush())
                            usb_send_nak(ERR_WRITE_FAILED);
                        break;

                    case CMD_SYS_RESET:
                        vTaskDelay(pdMS_TO_TICKS(50)); // Buffer flush time
                        HAL_NVIC_SystemReset();
                        break;
                    }
                }
                else
                {
                    usb_send_nak(ERR_INVALID_FOOTER);
                }
                parser_state = STATE_MAGIC_H;
                break;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* TinyUSB Callbacks (ISR Context)                                            */
/* -------------------------------------------------------------------------- */

void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    if (cdc_task_handle != NULL)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(cdc_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    // Reset parser if host disconnects/reconnects to avoid "stale" state
    parser_state = STATE_MAGIC_H;

    if (dtr && (cdc_task_handle != NULL))
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(cdc_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}
