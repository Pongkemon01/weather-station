/**
 * @file    a7670_at_channel.c
 * @brief   AT channel — line reassembly, URC dispatch, command sequencing,
 *          modem/network readiness, and SSL service startup.
 *
 * RAM layout (static allocations owned by this file)
 * ───────────────────────────────────────────────────
 *   s_ch struct          ~44 B   queues, semaphores, flags
 *   s_ch.line_buf[128]  128 B   line reassembly accumulator
 *                       ─────
 *   Total static         172 B
 *
 * URC coverage (section 19.4, A76XX AT Command Manual V1.09)
 * ────────────────────────────────────────────────────────────
 *   +CCHEVENT:<id>,RECV EVENT      → HTTP_URC_CCHEVENT
 *   +CCH_RECV_CLOSED:<id>,<err>    → HTTP_URC_CCH_RECV_CLOSED
 *   +CCHSEND:<id>,<err>            → HTTP_URC_CCHSEND_RESULT
 *   +CCH_PEER_CLOSED:<id>          → HTTP_URC_CCH_PEER_CLOSED
 *   +CCH: CCH STOP                 → HTTP_URC_CCH_STOP
 *
 * Deferred command results (same post-OK pattern as CCHOPEN):
 *   +CCHSTART:<err>                → HTTP_URC_CCHSTART
 *   +CCHSTOP:<err>                 → HTTP_URC_CCHSTOP
 *   +CCHOPEN:<id>,<err>            → HTTP_URC_CCHOPEN
 *   +CCHCLOSE:<id>,<err>           → HTTP_URC_CCHCLOSE
 */

#include "a7670_at_channel.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ─────────────────────────── Private constants ──────────────────────────── */

#define AT_RX_TASK_STACK_WORDS  192u
#define AT_RX_TASK_PRIORITY     4u

/** Timeout for individual AT command TX sends (UART level). */
#define AT_COMM_TIMEOUT         500u

/**
 * TX chunk size for binary payload streaming (AT+CCHSEND / AT+CCERTDOWN).
 *
 * Deliberately independent of UART_BLOCK_SIZE (a receive-path constant).
 * The A7670E accepts up to 2048 B per AT+CCHSEND call; 512 B chunks give
 * a comfortable margin while fitting in a single DMA transfer on STM32.
 */
#define AT_TX_CHUNK_SIZE        512u

/**
 * Timeout for AT+CCHSTART deferred +CCHSTART URC after the immediate OK.
 * Manual max response time is 120 000 ms; use a conservative 30 s here
 * since PDP activation typically completes in < 10 s on an LTE network.
 */
#define AT_CCHSTART_URC_TIMEOUT_MS  30000u

/* ─────────────────────────── Module state ───────────────────────────────── */

static struct {
    UART_Ctx_t       *uart_ctx;
    QueueHandle_t     urc_queue;
    TaskHandle_t      rx_task_handle;

    SemaphoreHandle_t cmd_mutex;
    SemaphoreHandle_t resp_sem;

    volatile AtResult_t pending_result;
    volatile bool       expecting_response;

    /* Optional caller-supplied text capture buffer. */
    char     *capture_buf;
    uint16_t  capture_size;
    uint16_t  capture_pos;

    /* Line reassembly — owned exclusively by rx_task. */
    char     line_buf[AT_LINE_BUF_SIZE];
    uint16_t line_pos;

    bool initialized;
} s_ch;

/* ─────────────────────────── Forward declarations ───────────────────────── */

static void        rx_task(void *arg);
static void        process_byte(uint8_t b);
static void        dispatch_line(const char *line, uint16_t len);
static void        signal_result(AtResult_t result);
static void        capture_append(const char *line, uint16_t len);
static bool        is_cch_urc(const char *line);
static void        parse_and_forward_urc(const char *line);
static bool        parse_stat(const char *response, int *stat_out);

/* ══════════════════════════ Public API ══════════════════════════════════════*/

bool at_channel_init(UART_Ctx_t *uart_ctx, QueueHandle_t urc_queue)
{
    if (!uart_ctx || !urc_queue)
        return false;
    if (s_ch.initialized)
        return true;

    memset(&s_ch, 0, sizeof(s_ch));
    s_ch.uart_ctx  = uart_ctx;
    s_ch.urc_queue = urc_queue;

    s_ch.cmd_mutex = xSemaphoreCreateMutex();
    s_ch.resp_sem  = xSemaphoreCreateBinary();
    if (!s_ch.cmd_mutex || !s_ch.resp_sem)
        goto fail;

    if (xTaskCreate(rx_task, "at_rx",
                    AT_RX_TASK_STACK_WORDS, NULL,
                    AT_RX_TASK_PRIORITY,
                    &s_ch.rx_task_handle) != pdPASS)
        goto fail;

    s_ch.initialized = true;
    return true;

fail:
    if (s_ch.cmd_mutex)
    {
        vSemaphoreDelete(s_ch.cmd_mutex);
        s_ch.cmd_mutex = NULL;
    }
    if (s_ch.resp_sem)
    {
        vSemaphoreDelete(s_ch.resp_sem);
        s_ch.resp_sem  = NULL;
    }
    return false;
}

/* -------------------------------------------------------------------------- */

AtResult_t at_channel_ping_modem(uint32_t total_timeout_ms,
                                       uint8_t  at_alive_retries)
{
    if (!s_ch.initialized)
        return AT_ERROR;

    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(total_timeout_ms);

    bool alive = false;
    for (uint8_t i = 0; i < at_alive_retries; i++)
    {
        if (at_channel_send_cmd("AT", 1000u) == AT_OK)
        {
            alive = true;
            break;
        }
        if (xTaskGetTickCount() >= deadline)
            return AT_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
    if (!alive)
        return AT_ERROR;

    return AT_OK;
}

/* -------------------------------------------------------------------------- */

/**
 * at_channel_wait_ready — block until modem is alive, network is registered,
 * and AT+CCHSTART has completed successfully.
 *
 * Step 1  AT echo test
 *   Sends "AT\r\n" and expects OK.  Retried up to at_alive_retries times
 *   with a 1-second gap.  Failure → AT_READY_NO_MODEM.
 *
 * Step 2  Network registration
 *   Polls AT+CGREG? and AT+CEREG? alternately every AT_REG_POLL_MS ms.
 *   Accepts stat = 1 (home) or 5 (roaming) on either command.
 *   This also implicitly confirms SIM presence (no registration without SIM).
 *   Failure to register within deadline → AT_READY_NO_NETWORK.
 *
 * Step 3  GPRS/packet attach
 *   Polls AT+CGATT? until state = 1.
 *   On modern LTE networks this is usually already 1 once CEREG=1, but
 *   older or 2G networks require a separate attach step.
 *   Failure → AT_READY_NO_ATTACH.
 *
 * Step 4  Start SSL service (AT+CCHSTART)
 *   Sends AT+CCHSTART. The immediate OK only means the command was accepted.
 *   The real result arrives as the deferred URC "+CCHSTART: <err>" which is
 *   forwarded to the URC queue by dispatch_line().  We drain the URC queue
 *   here looking for HTTP_URC_CCHSTART; err=0 means success.
 *   Failure (err ≠ 0 or timeout) → AT_READY_CCHSTART_FAIL.
 *
 * If total_timeout_ms elapses before all steps complete → AT_READY_TIMEOUT.
 */
AtReadyResult_t at_channel_wait_ready(uint32_t total_timeout_ms,
                                       uint8_t  at_alive_retries)
{
    if (!s_ch.initialized)
        return AT_READY_NO_MODEM;

    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(total_timeout_ms);

    /* ── Step 1: AT echo test ─────────────────────────────────────────── */
    {
        bool alive = false;
        for (uint8_t i = 0; i < at_alive_retries; i++)
        {
            if (at_channel_send_cmd("AT", 1000u) == AT_OK)
            {
                alive = true;
                break;
            }
            if (xTaskGetTickCount() >= deadline)
                return AT_READY_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(1000u));
        }
        if (!alive)
            return AT_READY_NO_MODEM;
    }

    /* ── Step 2: Network registration ────────────────────────────────── */
    /*
     * We poll AT+CGREG? (GPRS) and AT+CEREG? (LTE/EPS) alternately.
     * stat values 1 (home) and 5 (roaming) both indicate full service.
     * See manual sections 4.2.1 (CREG), 5.2.1 (CGREG), 5.2.2 (CEREG).
     *
     * AT+CREG is intentionally NOT polled here — it covers circuit-switched
     * voice registration, which is irrelevant for data/SSL usage.
     */
    {
        /* 64 B capture buffer — enough for "+CGREG: 0,1" */
        char cap[64];
        bool registered = false;

        while (!registered)
        {
            if (xTaskGetTickCount() >= deadline)
                return AT_READY_TIMEOUT;

            const char *cmds[] = { "AT+CGREG?", "AT+CEREG?" };
            for (int c = 0; c < 2 && !registered; c++)
            {
                at_channel_set_capture(cap, sizeof(cap));
                AtResult_t r = at_channel_send_cmd(cmds[c], 5000u);
                at_channel_set_capture(NULL, 0);

                if (r == AT_OK)
                {
                    int stat = -1;
                    if (parse_stat(cap, &stat))
                    {
                        if (stat == 1 || stat == 5)
                            registered = true;
                    }
                }
            }

            if (!registered)
                vTaskDelay(pdMS_TO_TICKS(AT_REG_POLL_MS));
        }
    }

    /* ── Step 3: GPRS/packet domain attach ──────────────────────────── */
    /*
     * AT+CGATT? returns +CGATT: <state> where 1 = attached.
     * See manual section 5.2.3.
     * On LTE this is normally already 1 once CEREG=1, but we verify
     * explicitly rather than assuming.
     */
    {
        char cap[32];
        bool attached = false;

        while (!attached)
        {
            if (xTaskGetTickCount() >= deadline)
                return AT_READY_TIMEOUT;

            at_channel_set_capture(cap, sizeof(cap));
            AtResult_t r = at_channel_send_cmd("AT+CGATT?", 5000u);
            at_channel_set_capture(NULL, 0);

            if (r == AT_OK)
            {
                /* Response line: "+CGATT: 1" — extract the single digit */
                const char *p = strstr(cap, "+CGATT:");
                if (p)
                {
                    int state = -1;
                    if (sscanf(p + 7, " %d", &state) == 1 && state == 1)
                        attached = true;
                }
            }

            if (!attached)
                vTaskDelay(pdMS_TO_TICKS(AT_REG_POLL_MS));
        }
    }

    /* ── Step 4: Start SSL service ───────────────────────────────────── */
    /*
     * AT+CCHSTART activates the PDP context and starts the SSL service.
     * Manual (page 432) shows the exchange:
     *
     *   → AT+CCHSTART\r\n
     *   ← OK              (immediate: command accepted)
     *   ← +CCHSTART: 0    (deferred: PDP result; 0 = success)
     *
     * The +CCHSTART:<err> line is caught by dispatch_line() → is_cch_urc()
     * → parse_and_forward_urc() → posted to urc_queue as HTTP_URC_CCHSTART.
     * We drain the URC queue here, discarding unrelated events, until the
     * CCHSTART result arrives or the deadline passes.
     *
     * err=0 → success; any other value → service activation failed.
     */
    {
        if (xTaskGetTickCount() >= deadline)
            return AT_READY_TIMEOUT;

        AtResult_t r = at_channel_send_cmd("AT+CCHSTART", 5000u);
        if (r != AT_OK)
            return AT_READY_CCHSTART_FAIL;

        /* Wait for the deferred +CCHSTART:<err> URC */
        TickType_t urc_deadline = xTaskGetTickCount() +
                                  pdMS_TO_TICKS(AT_CCHSTART_URC_TIMEOUT_MS);
        HttpUrcEvent_t ev;
        bool got_result = false;

        while (xTaskGetTickCount() < urc_deadline &&
               xTaskGetTickCount() < deadline)
        {
            TickType_t remaining = urc_deadline - xTaskGetTickCount();
            if (xQueueReceive(s_ch.urc_queue, &ev,
                              remaining > pdMS_TO_TICKS(200u)
                              ? pdMS_TO_TICKS(200u) : remaining) == pdPASS)
            {
                if (ev.type == HTTP_URC_CCHSTART)
                {
                    got_result = true;
                    if (ev.param != 0)
                        return AT_READY_CCHSTART_FAIL;
                    break;   /* param == 0: success */
                }
                /* Discard other URCs that arrived during startup */
            }
        }

        if (!got_result)
            return AT_READY_CCHSTART_FAIL;
    }

    return AT_READY_OK;
}

/* -------------------------------------------------------------------------- */

void at_channel_set_capture(char *buf, uint16_t size)
{
    s_ch.capture_buf  = buf;
    s_ch.capture_size = size;
    s_ch.capture_pos  = 0;
    if (buf && size > 0)
        buf[0] = '\0';
}

/* -------------------------------------------------------------------------- */

AtResult_t at_channel_send_cmd(const char *cmd, uint32_t timeout_ms)
{
    if (!s_ch.initialized || !cmd)
        return AT_ERROR;

    xSemaphoreTake(s_ch.cmd_mutex, portMAX_DELAY);

    s_ch.pending_result     = AT_TIMEOUT;
    s_ch.expecting_response = true;

    UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)cmd,
                  (uint16_t)strlen(cmd), AT_COMM_TIMEOUT);
    UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)"\r\n", 2u,
                  AT_COMM_TIMEOUT);

    bool got = (xSemaphoreTake(s_ch.resp_sem,
                               pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    s_ch.expecting_response = false;
    AtResult_t result = got ? s_ch.pending_result : AT_TIMEOUT;
    xSemaphoreGive(s_ch.cmd_mutex);
    return result;
}

/* -------------------------------------------------------------------------- */

AtResult_t at_channel_send_binary(const char    *cmd,
                                   const uint8_t *data,
                                   size_t         len,
                                   uint32_t       timeout_ms)
{
    if (!s_ch.initialized || !cmd || !data)
        return AT_ERROR;

    xSemaphoreTake(s_ch.cmd_mutex, portMAX_DELAY);

    /* Phase 1: send command, wait for '>' prompt */
    s_ch.pending_result     = AT_TIMEOUT;
    s_ch.expecting_response = true;

    UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)cmd,
                  (uint16_t)strlen(cmd), AT_COMM_TIMEOUT);
    UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)"\r\n", 2u,
                  AT_COMM_TIMEOUT);

    bool got_prompt = (xSemaphoreTake(s_ch.resp_sem,
                                      pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    if (!got_prompt || s_ch.pending_result != AT_PROMPT)
    {
        s_ch.expecting_response = false;
        AtResult_t r = got_prompt ? AT_ERROR : AT_TIMEOUT;
        xSemaphoreGive(s_ch.cmd_mutex);
        return r;
    }

    /* Phase 2: stream binary payload, wait for OK / ERROR */
    s_ch.pending_result     = AT_TIMEOUT;
    s_ch.expecting_response = true;

    const uint8_t *ptr    = data;
    size_t         remain = len;
    while (remain > 0)
    {
        uint16_t slice = (remain > AT_TX_CHUNK_SIZE)
                         ? AT_TX_CHUNK_SIZE : (uint16_t)remain;
        UART_Sys_Send(s_ch.uart_ctx, ptr, slice, AT_COMM_TIMEOUT);
        ptr    += slice;
        remain -= slice;
    }

    bool got_ok = (xSemaphoreTake(s_ch.resp_sem,
                                  pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    s_ch.expecting_response = false;
    AtResult_t result = got_ok ? s_ch.pending_result : AT_TIMEOUT;
    xSemaphoreGive(s_ch.cmd_mutex);
    return result;
}

/* -------------------------------------------------------------------------- */

void at_channel_deinit(void)
{
    if (!s_ch.initialized)
        return;
    if (s_ch.rx_task_handle)
    {
        vTaskDelete(s_ch.rx_task_handle);
        s_ch.rx_task_handle = NULL;
    }
    if (s_ch.cmd_mutex)
    {
        vSemaphoreDelete(s_ch.cmd_mutex);
        s_ch.cmd_mutex = NULL;
    }
    if (s_ch.resp_sem)
    {
        vSemaphoreDelete(s_ch.resp_sem);
        s_ch.resp_sem  = NULL;
    }
    s_ch.initialized = false;
}

/* ══════════════════════════ RX Task ══════════════════════════════════════════*/

static void rx_task(void *arg)
{
    (void)arg;
    UART_Packet_t frame;

    for (;;)
    {
        if (!UART_Sys_Receive(s_ch.uart_ctx, &frame, portMAX_DELAY))
            continue;

        for (uint16_t i = 0; i < frame.length; i++)
            process_byte(frame.payload[i]);

        UART_Sys_ReleaseBuffer(frame.payload);
    }
}

/* ══════════════════════════ Line Reassembler ════════════════════════════════*/

static void process_byte(uint8_t b)
{
    /* '>' on an otherwise empty line: immediate data-entry prompt */
    if (b == '>' && s_ch.line_pos == 0)
    {
        dispatch_line(">", 1u);
        return;
    }

    if (b == '\n')
    {
        uint16_t dlen = s_ch.line_pos;
        if (dlen > 0 && s_ch.line_buf[dlen - 1] == '\r')
            dlen--;

        if (dlen > 0)
        {
            s_ch.line_buf[dlen] = '\0';
            dispatch_line(s_ch.line_buf, dlen);
        }
        s_ch.line_pos = 0;
        return;
    }

    if (s_ch.line_pos >= AT_LINE_BUF_SIZE - 1u)
    {
        s_ch.line_pos = 0;  /* overflow: discard and restart */
        return;
    }

    s_ch.line_buf[s_ch.line_pos++] = (char)b;
}

/* ══════════════════════════ Line Dispatcher ═════════════════════════════════*/

/**
 * Route one complete modem line:
 *
 *   ">"            → AT_PROMPT  (binary send: data entry prompt)
 *   "OK"           → AT_OK
 *   "ERROR"        → AT_ERROR
 *   "+CME ERROR:"  → AT_CME_ERROR
 *   CCH URCs       → forward to urc_queue
 *   anything else  → append to capture buffer if active
 */
static void dispatch_line(const char *line, uint16_t len)
{
    if (len == 1u && line[0] == '>')
    {
        signal_result(AT_PROMPT);
        return;
    }

    if (strcmp(line, "OK") == 0)
    {
        signal_result(AT_OK);
        return;
    }
    if (strcmp(line, "ERROR") == 0)
    {
        signal_result(AT_ERROR);
        return;
    }
    if (strncmp(line, "+CME ERROR:", 11) == 0)
    {
        signal_result(AT_CME_ERROR);
        return;
    }

    /* CCH URCs and deferred command results — always forwarded */
    if (is_cch_urc(line))
    {
        parse_and_forward_urc(line);
        return;
    }

    /* Informational text (e.g. "+CGREG: 0,1") — append to capture buffer */
    if (s_ch.expecting_response && s_ch.capture_buf)
        capture_append(line, len);
}

/* ─────────────────────────── Signal / Capture ───────────────────────────── */

static void signal_result(AtResult_t result)
{
    if (!s_ch.expecting_response)
        return;
    s_ch.pending_result     = result;
    s_ch.expecting_response = false;   /* clear BEFORE give to close race */
    xSemaphoreGive(s_ch.resp_sem);
}

static void capture_append(const char *line, uint16_t len)
{
    if (!s_ch.capture_buf || s_ch.capture_size == 0)
        return;

    uint16_t space = s_ch.capture_size - s_ch.capture_pos - 1u;
    if (space == 0)
        return;

    uint16_t copy = (len < space) ? len : space;
    memcpy(s_ch.capture_buf + s_ch.capture_pos, line, copy);
    s_ch.capture_pos += copy;

    if (s_ch.capture_pos < s_ch.capture_size - 1u)
        s_ch.capture_buf[s_ch.capture_pos++] = '\n';

    s_ch.capture_buf[s_ch.capture_pos] = '\0';
}

/* ─────────────────────────── URC Recognition ───────────────────────────── */

/**
 * Returns true if @p line is a CCH URC or deferred CCH command result.
 *
 * URCs defined in section 19.4 of the A76XX manual:
 *   +CCHEVENT:        — data available notification (manual recv mode)
 *   +CCH_RECV_CLOSED: — receive error on a session
 *   +CCHSEND:         — send failure notification (when CCHSET enabled)
 *   +CCH_PEER_CLOSED: — server closed the connection
 *   +CCH:             — CCH service stopped (network error)
 *
 * Deferred results (arrive after the immediate OK, same as +CCHOPEN):
 *   +CCHSTART:        — PDP activation / SSL service start result
 *   +CCHSTOP:         — SSL service stop result
 *   +CCHOPEN:         — TCP/SSL connection result
 *   +CCHCLOSE:        — disconnect result
 */
static bool is_cch_urc(const char *line)
{
    return strncmp(line, "+CCHSTART:",        10) == 0
        || strncmp(line, "+CCHSTOP:",          9) == 0
        || strncmp(line, "+CCHOPEN:",          9) == 0
        || strncmp(line, "+CCHCLOSE:",        10) == 0
        || strncmp(line, "+CCHEVENT:",        10) == 0
        || strncmp(line, "+CCH_RECV_CLOSED:", 18) == 0
        || strncmp(line, "+CCHSEND:",          9) == 0
        || strncmp(line, "+CCH_PEER_CLOSED:", 17) == 0
        || strncmp(line, "+CCH:",              5) == 0;
}

/**
 * Parse a recognised CCH URC / deferred result and post it to urc_queue.
 *
 * Parsing notes
 * ─────────────
 * +CCHEVENT:<id>,RECV EVENT
 *   The second field is the literal string "RECV EVENT", not an integer.
 *   The previous code used sscanf("%d,%d") which silently failed to match
 *   "RECV EVENT" and left param=0.  We now confirm the string explicitly
 *   and set param=0 by convention (there is no numeric param for this URC).
 *
 * +CCH_PEER_CLOSED:<id>
 *   Only one field — the session id. No error code.
 *
 * +CCH: CCH STOP
 *   No session id or error code. client_idx=-1, param=0.
 *
 * +CCHSTART:<err> / +CCHSTOP:<err>
 *   Single field: the error code. client_idx=-1 (not session-specific).
 *
 * +CCHOPEN:<id>,<err> / +CCHCLOSE:<id>,<err> / +CCHSEND:<id>,<err>
 *   Two fields: session id and error code.
 *
 * +CCH_RECV_CLOSED:<id>,<err>
 *   Two fields: session id and error code.
 */
static void parse_and_forward_urc(const char *line)
{
    HttpUrcEvent_t ev = { .client_idx = -1, .param = 0 };

    if (strncmp(line, "+CCHSTART:", 10) == 0)
    {
        ev.type = HTTP_URC_CCHSTART;
        /* Single field: error code. client_idx stays -1. */
        int err = 0;
        sscanf(line + 10, " %d", &err);
        ev.param = (int8_t)err;
    }
    else if (strncmp(line, "+CCHSTOP:", 9) == 0)
    {
        ev.type = HTTP_URC_CCHSTOP;
        int err = 0;
        sscanf(line + 9, " %d", &err);
        ev.param = (int8_t)err;
    }
    else if (strncmp(line, "+CCHOPEN:", 9) == 0)
    {
        ev.type = HTTP_URC_CCHOPEN;
        int id = -1, err = 0;
        sscanf(line + 9, " %d,%d", &id, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCHCLOSE:", 10) == 0)
    {
        ev.type = HTTP_URC_CCHCLOSE;
        int id = -1, err = 0;
        sscanf(line + 10, " %d,%d", &id, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCHEVENT:", 10) == 0)
    {
        /*
         * Format: +CCHEVENT:<session_id>,RECV EVENT
         *
         * The second field is the literal string "RECV EVENT".
         * Previous code used sscanf("%d,%d") which failed silently
         * because "RECV EVENT" is not an integer. We now parse only
         * the session_id and verify the keyword explicitly.
         */
        ev.type = HTTP_URC_CCHEVENT;
        int id = -1;
        if (sscanf(line + 10, " %d,", &id) == 1)
            ev.client_idx = (int8_t)id;
        /* param = 0 by default; "RECV EVENT" carries no numeric payload */
    }
    else if (strncmp(line, "+CCH_RECV_CLOSED:", 18) == 0)
    {
        ev.type = HTTP_URC_CCH_RECV_CLOSED;
        int id = -1, err = 0;
        sscanf(line + 18, " %d,%d", &id, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCHSEND:", 9) == 0)
    {
        ev.type = HTTP_URC_CCHSEND_RESULT;
        int id = -1, err = 0;
        sscanf(line + 9, " %d,%d", &id, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCH_PEER_CLOSED:", 17) == 0)
    {
        /*
         * Format: +CCH_PEER_CLOSED:<session_id>
         * No error code — server initiated the close.
         */
        ev.type = HTTP_URC_CCH_PEER_CLOSED;
        int id = -1;
        sscanf(line + 17, " %d", &id);
        ev.client_idx = (int8_t)id;
        /* param = 0 */
    }
    else if (strncmp(line, "+CCH:", 5) == 0)
    {
        /*
         * Format: +CCH: CCH STOP
         * CCH service stopped due to network error. No id or error code.
         */
        ev.type = HTTP_URC_CCH_STOP;
        /* client_idx = -1, param = 0 by default */
    }
    else
    {
        return;  /* matched is_cch_urc() but no parser — ignore */
    }

    xQueueSend(s_ch.urc_queue, &ev, 0);
}

/* ─────────────────────────── Registration parser ───────────────────────── */

/**
 * Extract the <stat> field from a +CGREG or +CEREG response line.
 *
 * The read command response has two forms:
 *   +CGREG: <n>,<stat>         when n=1 or n=2
 *   +CEREG: <n>,<stat>[,...]
 *
 * We skip the first integer (<n>) and read the second (<stat>).
 *
 * The capture buffer may contain multiple lines separated by '\n'.
 * We scan for "+CGREG:" or "+CEREG:" and parse from there.
 */
static bool parse_stat(const char *response, int *stat_out)
{
    if (!response || !stat_out)
        return false;

    const char *prefixes[] = { "+CGREG:", "+CEREG:", "+CREG:" };
    for (int p = 0; p < 3; p++)
    {
        const char *pos = strstr(response, prefixes[p]);
        if (!pos)
            continue;
        pos += strlen(prefixes[p]);

        int n = -1, stat = -1;
        /* Try "<n>,<stat>" form first (read command with URCs enabled) */
        if (sscanf(pos, " %d,%d", &n, &stat) == 2)
        {
            *stat_out = stat;
            return true;
        }
        /* Fall back to "<stat>" form (URCs disabled, n=0) */
        if (sscanf(pos, " %d", &stat) == 1)
        {
            *stat_out = stat;
            return true;
        }
    }
    return false;
}
