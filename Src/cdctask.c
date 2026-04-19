#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "nv_database.h"
#include "weather_data.h"
#include "datetime.h"
#include "main.h"
#include "watchdog_task.h"

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
    * All successful responses to host commands should be acknowledged with an ACK message containing the echoed command code.
 *--------------------------------------------------------------------*/

/* -------------------------------------------------------------------------- */
/* Protocol Constants                                                         */
/* -------------------------------------------------------------------------- */
#define MAGIC_H2D_H    0xDCu
#define MAGIC_H2D_L    0xB0u
#define MAGIC_D2H_H    0x55u
#define MAGIC_D2H_L    0xAAu

/* BUG FIX #4: Pre-compute footer bytes as named constants to avoid
 * relying on integer-promotion behaviour of ~ at every use site.
 * ~0xDC as int = 0xFFFFFF23; cast to uint8_t = 0x23.
 * Having these as explicit uint8_t macros makes every comparison
 * unambiguously unsigned and self-documenting. */
#define MAGIC_H2D_FOOT_H  ((uint8_t)(~MAGIC_H2D_H))   /* 0x23 */
#define MAGIC_H2D_FOOT_L  ((uint8_t)(~MAGIC_H2D_L))   /* 0x4F */

#define CMD_REQ_WEATHER  0x01u  /* Weather data only  */
#define CMD_REQ_META     0x02u  /* Config only        */
#define CMD_SET_META     0x03u  /* Update Config      */
#define CMD_SET_RTC      0x04u  /* Set RTC time       */
#define CMD_REQ_STATUS   0x05u  /* Get System Status  */
#define CMD_DB_FLUSH     0x06u  /* Flush database     */
#define CMD_SYS_RESET    0x07u  /* Reboot             */
#define CMD_ACK          0xFEu  /* Generic success    */
#define CMD_NAK          0xFFu  /* Error feedback     */

/* BUG FIX #1: CMD_MAX is the highest *valid* command value.
 * Any byte > CMD_MAX is unknown.  CMD_NAK (0xFF) must never be
 * treated as a valid host command — the old guard
 *   (current_cmd > CMD_SYS_RESET && current_cmd != CMD_NAK)
 * allowed 0xFF through because of the != exclusion. */
#define CMD_MAX          CMD_SYS_RESET

#define ERR_UNKNOWN_CMD    0x01u
#define ERR_INVALID_FOOTER 0x02u
#define ERR_WRITE_FAILED   0x03u
#define ERR_INVALID_DATA   0x04u

/* BUG FIX #8: Named constant for the pre-reset TX-drain delay. */
#define RESET_DRAIN_MS     50u

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

/*
 * Compile-time guard: the union must be large enough for every payload type.
 * If a new payload type larger than 220 bytes is ever added, this will catch
 * it at build time before it becomes a buffer-overflow at runtime.
 * (BUG FIX #5 — static defence against future overflow)
 */
#define PG_BUF_SIZE  220u
_Static_assert(sizeof(Meta_Data_t)    <= PG_BUF_SIZE, "Meta_Data_t exceeds pg_buf");
_Static_assert(sizeof(RTC_DateTime_t) <= PG_BUF_SIZE, "RTC_DateTime_t exceeds pg_buf");

/* Union allows us to handle the largest possible payload (Meta_Data_t, 220 bytes)
 * without allocating multiple buffers or using large stack space. */
static union
{
    Meta_Data_t    meta;
    RTC_DateTime_t rtc;
    uint8_t        raw[PG_BUF_SIZE];
} pg_buf;

/* BUG FIX #9: volatile so the compiler does not cache this in a register
 * across the ISR boundary.  The ISR reads it; the task body writes it once. */
static volatile TaskHandle_t cdc_task_handle = NULL;

static usb_state_t parser_state = STATE_MAGIC_H;

/* BUG FIX #3: Move the extern declaration to file scope so it is not
 * re-declared on every call inside the switch-case. */
extern Weather_Data_t weather_data; /* Defined in main / sensor task */

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

/* BUG FIX #2 / #10: ACK helper for write commands so the host always receives
 * an explicit confirmation (or NAK) and never has to time out guessing. */
static void usb_send_ack(uint8_t echo_cmd)
{
    usb_send_header(CMD_ACK);
    tud_cdc_write(&echo_cmd, 1);
    usb_send_footer();
}

/* -------------------------------------------------------------------------- */
/* Main Task Loop                                                             */
/* -------------------------------------------------------------------------- */

void cdc_task(void *params)
{
    (void)params;
    /* Safe: written once here, then only read by ISR callbacks. */
    cdc_task_handle = xTaskGetCurrentTaskHandle();

    static int8_t wdt_id;
    wdt_id = wdt_register("cdctask");

    uint8_t  current_cmd  = 0;
    uint16_t rx_idx       = 0;
    uint16_t expected_len = 0;

    while (1)
    {
        /* Wait for USB CDC notification; kick watchdog every 500 ms while idle. */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WDT_PERIOD_MS)) == 0u)
            wdt_kick(wdt_id);
        wdt_kick(wdt_id);

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
                rx_idx      = 0;

                if (current_cmd == CMD_SET_META)
                    expected_len = (uint16_t)sizeof(Meta_Data_t);
                else if (current_cmd == CMD_SET_RTC)
                    expected_len = (uint16_t)sizeof(RTC_DateTime_t);
                else
                    expected_len = 0;

                /* BUG FIX #1: Reject anything above CMD_MAX unconditionally.
                 * The old check (> CMD_SYS_RESET && != CMD_NAK) let CMD_NAK
                 * (0xFF) fall through to the execution switch with no handler,
                 * executing nothing silently instead of sending an error. */
                if (current_cmd > CMD_MAX)
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
                /* BUG FIX #5: Runtime guard — rx_idx is bounded by expected_len
                 * which is already checked against PG_BUF_SIZE via _Static_assert,
                 * but belt-and-suspenders: cap at PG_BUF_SIZE to be safe against
                 * any future code path that forgets to set expected_len correctly. */
                if (rx_idx < PG_BUF_SIZE)
                    pg_buf.raw[rx_idx] = b;

                rx_idx++;
                if (rx_idx >= expected_len)
                    parser_state = STATE_FOOTER_H;
                break;

            case STATE_FOOTER_H:
                /* BUG FIX #4: Compare against the pre-computed uint8_t constant,
                 * not a raw ~ expression that widens to int before comparison. */
                if (b == MAGIC_H2D_FOOT_H)
                    parser_state = STATE_FOOTER_L;
                else
                {
                    usb_send_nak(ERR_INVALID_FOOTER);
                    parser_state = STATE_MAGIC_H;
                }
                break;

            case STATE_FOOTER_L:
                /* BUG FIX #4: Same — use pre-computed constant. */
                if (b == MAGIC_H2D_FOOT_L)
                {
                    /* --- COMMAND EXECUTION --- */
                    switch (current_cmd)
                    {
                    case CMD_REQ_WEATHER:
                    {
                        Weather_Data_Packed_t wp;
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
                        /* BUG FIX #2/#10: ACK on success so host is not left guessing. */
                        if (DB_SetMeta(&(pg_buf.meta)))
                            usb_send_ack(CMD_SET_META);
                        else
                            usb_send_nak(ERR_WRITE_FAILED);
                        break;

                    case CMD_SET_RTC:
                        /* BUG FIX #2/#10: ACK on success. */
                        if (datetime_set_rtc(&(pg_buf.rtc)))
                            usb_send_ack(CMD_SET_RTC);
                        else
                            usb_send_nak(ERR_INVALID_DATA);
                        break;

                    case CMD_REQ_STATUS:
                        usb_send_header(CMD_REQ_STATUS);
                        tud_cdc_write(&system_ready_status, sizeof(System_Ready_Status_t));
                        usb_send_footer();
                        break;

                    case CMD_DB_FLUSH:
                        /* BUG FIX #2/#10: ACK on success. */
                        if (DB_Flush())
                            usb_send_ack(CMD_DB_FLUSH);
                        else
                            usb_send_nak(ERR_WRITE_FAILED);
                        break;

                    case CMD_SYS_RESET:
                        /* ACK before reset so host sees confirmation before link drops. */
                        usb_send_ack(CMD_SYS_RESET);
                        /* BUG FIX #8: Named constant instead of magic number. */
                        vTaskDelay(pdMS_TO_TICKS(RESET_DRAIN_MS));
                        HAL_NVIC_SystemReset();
                        break;
                    }
                }
                else
                {
                    usb_send_nak(ERR_INVALID_FOOTER);
                }
                /* BUG FIX #6: Reset the parser state unconditionally at the end
                 * of FOOTER_L, whether the footer was valid or not.  This was
                 * already done in the original code, but is kept here explicitly
                 * as the authoritative "end of packet" reset. */
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
    /* BUG FIX #9: Cast away volatile for the FreeRTOS API call.
     * The handle value itself is safe to read; volatile only prevents
     * the compiler from caching it across function boundaries. */
    TaskHandle_t h = (TaskHandle_t)cdc_task_handle;
    if (h != NULL)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(h, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    /* Reset parser if host disconnects/reconnects to avoid stale state. */
    parser_state = STATE_MAGIC_H;

    if (dtr)
    {
        TaskHandle_t h = (TaskHandle_t)cdc_task_handle;
        if (h != NULL)
        {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            vTaskNotifyGiveFromISR(h, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }
}
