/**
 * @file    a7670_at_channel.c
 * @brief   AT channel — line reassembly, URC dispatch, command sequencing,
 *          modem/network readiness, and SSL service startup.
 *
 * =============================================================================
 * Bugs fixed / improvements in this revision
 * ─────────────────────────────────────────────────────────────────────────────
 * BUG-AT-1  (send_cmd UART failure not checked)
 *   UART_Sys_Send() can return false (timeout, DMA busy).  The original code
 *   ignored both return values in send_cmd and send_binary, so a failed TX
 *   would still arm expecting_response and wait the full timeout_ms before
 *   declaring AT_TIMEOUT.  Now: if the command TX fails, release the mutex
 *   and return AT_TIMEOUT immediately without blocking.
 *
 * BUG-AT-2  (send_binary mutex leak on prompt failure)
 *   In at_channel_send_binary(), if the prompt wait timed out the code set
 *   expecting_response=false but forgot to call xSemaphoreGive(cmd_mutex)
 *   before returning — the mutex was never released, permanently blocking
 *   any subsequent AT command.  Now: the mutex is always released before
 *   every return path.
 *   (Note: the filed code already had this correct in the upload, but the
 *    logic was fragile and has been made explicit with a single exit point.)
 *
 * BUG-AT-3  (at_channel_set_capture race with rx_task)
 *   capture_buf, capture_size, and capture_pos are written non-atomically
 *   from the calling task while rx_task reads them concurrently.  On
 *   Cortex-M4 a misaligned or wide struct write is not atomic, so rx_task
 *   could see a partially-updated state and write into a stale or NULL
 *   buffer.  Fix: wrap capture updates in taskENTER_CRITICAL / taskEXIT_CRITICAL.
 *   (cmd_mutex alone doesn't protect this because rx_task never takes it.)
 *
 * BUG-AT-4  (deadline tick overflow in wait_ready registration poll)
 *   `xTaskGetTickCount() >= deadline` wraps incorrectly when the tick count
 *   rolls over past 0xFFFFFFFF.  FreeRTOS documents using time_after-style
 *   arithmetic: `(TickType_t)(xTaskGetTickCount() - deadline) < 0x80000000`.
 *   Changed all deadline comparisons to the portable helper macro pattern.
 *
 * BUG-AT-5  (CCHSTART already-running not handled)
 *   If the modem is already running (e.g. warm restart without power cycle),
 *   AT+CCHSTART returns +CME ERROR: 3 ("operation not allowed") because the
 *   service is already active.  The original code returned
 *   AT_READY_CCHSTART_FAIL in that case, forcing a full modem reinit.
 *   Fix: treat AT_CME_ERROR from AT+CCHSTART as a success path — the service
 *   is already up, so skip the deferred URC wait and proceed.
 *
 * OPTIM-1  (sscanf removed from hot ISR path)
 *   parse_and_forward_urc() runs on every modem line from the rx_task.
 *   Replaced sscanf() with a lightweight manual integer parser (parse_int)
 *   that reads one or two decimal fields.  Removes ~1-2 KB of scanf code.
 *
 * OPTIM-2  (capture_append separator)
 *   The original appended '\n' unconditionally and then checked bounds,
 *   which could overwrite the null terminator when the buffer was exactly
 *   full.  Rewritten with a single bounds check up front.
 * =============================================================================
 *
 * RAM layout (static allocations owned by this file)
 * ───────────────────────────────────────────────────
 *   s_ch struct          ~44 B   queues, semaphores, flags
 *   s_ch.line_buf[128]  128 B   line reassembly accumulator
 *                        ─────
 *   Total static         172 B
 */

#include "a7670_at_channel.h"

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ─────────────────────────── Private constants ──────────────────────────── */

#define AT_RX_TASK_STACK_WORDS     192u
#define AT_RX_TASK_PRIORITY        4u

/** Timeout for individual AT command TX sends (UART level). */
#define AT_COMM_TIMEOUT            500u

/**
 * TX chunk size for binary payload streaming (AT+CCHSEND / AT+CCERTDOWN).
 * Independent of UART_BLOCK_SIZE (receive-path constant).
 * A7670E accepts up to 2048 B per AT+CCHSEND; 512 B gives a comfortable margin.
 */
#define AT_TX_CHUNK_SIZE           512u

/**
 * Timeout for the deferred +CCHSTART URC after the immediate OK.
 * 30 s is a conservative bound; PDP activation typically completes in < 10 s.
 */
#define AT_CCHSTART_URC_TIMEOUT_MS 30000u

/* ─────────────────────────── Portable deadline helper ───────────────────── */
/*
 * FreeRTOS tick counter wraps at 0xFFFFFFFF.  A simple ">=" comparison fails
 * when the tick rolls over.  The portable test is:
 *   elapsed = (TickType_t)(now - start)
 * which is always correct under two's-complement wrap, as long as the elapsed
 * time is < UINT32_MAX/2.
 */
#define TICKS_ELAPSED(start)  ((TickType_t)(xTaskGetTickCount() - (start)))
#define DEADLINE_PASSED(start, ticks)  (TICKS_ELAPSED(start) >= (ticks))

/* ─────────────────────────── Module state ───────────────────────────────── */

static struct {
    UART_Ctx_t       *uart_ctx;
    QueueHandle_t     urc_queue;
    TaskHandle_t      rx_task_handle;

    SemaphoreHandle_t cmd_mutex;
    SemaphoreHandle_t resp_sem;

    volatile AtResult_t pending_result;
    volatile bool       expecting_response;

    /* Optional caller-supplied text capture buffer (BUG-AT-3: protected by critical section). */
    char     *capture_buf;
    uint16_t  capture_size;
    uint16_t  capture_pos;

    /* Line reassembly — owned exclusively by rx_task. */
    char     line_buf[AT_LINE_BUF_SIZE];
    uint16_t line_pos;

    bool initialized;
} s_ch;

/* ─────────────────────────── Forward declarations ───────────────────────── */

static void     rx_task(void *arg);
static void     process_byte(uint8_t b);
static void     dispatch_line(const char *line, uint16_t len);
static void     signal_result(AtResult_t result);
static void     capture_append(const char *line, uint16_t len);
static bool     is_cch_urc(const char *line);
static void     parse_and_forward_urc(const char *line);
static bool     parse_stat(const char *response, int *stat_out);
/* Lightweight integer parser replacing sscanf in the hot path (OPTIM-1). */
static int      parse_int(const char *s, int *out);

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
    if (s_ch.cmd_mutex) { vSemaphoreDelete(s_ch.cmd_mutex); s_ch.cmd_mutex = NULL; }
    if (s_ch.resp_sem)  { vSemaphoreDelete(s_ch.resp_sem);  s_ch.resp_sem  = NULL; }
    return false;
}

/* -------------------------------------------------------------------------- */

AtResult_t at_channel_ping_modem(uint32_t total_timeout_ms,
                                  uint8_t  at_alive_retries)
{
    if (!s_ch.initialized)
        return AT_ERROR;

    TickType_t start = xTaskGetTickCount();

    for (uint8_t i = 0u; i < at_alive_retries; i++)
    {
        if (at_channel_send_cmd("AT", 1000u) == AT_OK)
            return AT_OK;

        if (DEADLINE_PASSED(start, pdMS_TO_TICKS(total_timeout_ms)))
            return AT_TIMEOUT;

        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
    return AT_ERROR;
}

/* -------------------------------------------------------------------------- */

/**
 * at_channel_wait_ready — block until modem alive, network registered,
 * and AT+CCHSTART completed successfully.
 *
 * Step 1  AT echo test
 * Step 2  Network registration (AT+CGREG? / AT+CEREG?)
 * Step 3  GPRS/packet domain attach (AT+CGATT?)
 * Step 4  Start SSL service (AT+CCHSTART) + wait for deferred +CCHSTART:0 URC
 *
 * BUG-AT-4: All deadline checks use DEADLINE_PASSED() (wrap-safe arithmetic).
 * BUG-AT-5: AT_CME_ERROR from AT+CCHSTART treated as service-already-running.
 */
AtReadyResult_t at_channel_wait_ready(uint32_t total_timeout_ms,
                                       uint8_t  at_alive_retries)
{
    if (!s_ch.initialized)
        return AT_READY_NO_MODEM;

    const TickType_t total_ticks = pdMS_TO_TICKS(total_timeout_ms);
    TickType_t       start       = xTaskGetTickCount();

    /* ── Step 1: AT echo test ─────────────────────────────────────────── */
    {
        bool alive = false;
        for (uint8_t i = 0u; i < at_alive_retries; i++)
        {
            if (at_channel_send_cmd("AT", 1000u) == AT_OK)
            {
                alive = true;
                break;
            }
            if (DEADLINE_PASSED(start, total_ticks))
                return AT_READY_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(1000u));
        }
        if (!alive)
            return AT_READY_NO_MODEM;
    }

    /* ── Step 2: Network registration ────────────────────────────────── */
    /*
     * Poll AT+CGREG? (GPRS) and AT+CEREG? (LTE/EPS) alternately.
     * stat=1 (home) or stat=5 (roaming) = registered.
     * AT+CREG (voice) is intentionally not polled — irrelevant for data.
     */
    {
        char cap[64];
        bool registered = false;

        while (!registered)
        {
            if (DEADLINE_PASSED(start, total_ticks))
                return AT_READY_TIMEOUT;

            const char *cmds[] = { "AT+CGREG?", "AT+CEREG?" };
            for (int c = 0; c < 2 && !registered; c++)
            {
                /* BUG-AT-3: protect capture pointer write */
                taskENTER_CRITICAL();
                s_ch.capture_buf  = cap;
                s_ch.capture_size = sizeof(cap);
                s_ch.capture_pos  = 0u;
                cap[0]            = '\0';
                taskEXIT_CRITICAL();

                AtResult_t r = at_channel_send_cmd(cmds[c], 5000u);

                taskENTER_CRITICAL();
                s_ch.capture_buf = NULL;
                taskEXIT_CRITICAL();

                if (r == AT_OK)
                {
                    int stat = -1;
                    if (parse_stat(cap, &stat) && (stat == 1 || stat == 5))
                        registered = true;
                }
            }

            if (!registered)
                vTaskDelay(pdMS_TO_TICKS(AT_REG_POLL_MS));
        }
    }

    /* ── Step 3: GPRS/packet domain attach ──────────────────────────── */
    {
        char cap[32];
        bool attached = false;

        while (!attached)
        {
            if (DEADLINE_PASSED(start, total_ticks))
                return AT_READY_TIMEOUT;

            taskENTER_CRITICAL();
            s_ch.capture_buf  = cap;
            s_ch.capture_size = sizeof(cap);
            s_ch.capture_pos  = 0u;
            cap[0]            = '\0';
            taskEXIT_CRITICAL();

            AtResult_t r = at_channel_send_cmd("AT+CGATT?", 5000u);

            taskENTER_CRITICAL();
            s_ch.capture_buf = NULL;
            taskEXIT_CRITICAL();

            if (r == AT_OK)
            {
                const char *p = strstr(cap, "+CGATT:");
                if (p)
                {
                    int state = -1;
                    parse_int(p + 7, &state);
                    if (state == 1) attached = true;
                }
            }

            if (!attached)
                vTaskDelay(pdMS_TO_TICKS(AT_REG_POLL_MS));
        }
    }

    /* ── Step 4: Start SSL service ───────────────────────────────────── */
    /*
     * BUG-AT-5: On a warm restart the modem may already have the CCH service
     * running, in which case AT+CCHSTART returns +CME ERROR: 3 ("operation
     * not allowed").  Treat this as success — the service is already up —
     * rather than failing with AT_READY_CCHSTART_FAIL.
     */
    {
        if (DEADLINE_PASSED(start, total_ticks))
            return AT_READY_TIMEOUT;

        AtResult_t r = at_channel_send_cmd("AT+CCHSTART", 5000u);

        /* Service already running — skip the deferred URC wait */
        if (r == AT_CME_ERROR)
            return AT_READY_OK;

        if (r != AT_OK)
            return AT_READY_CCHSTART_FAIL;

        /* Wait for deferred +CCHSTART:<err> URC */
        TickType_t urc_ticks = pdMS_TO_TICKS(AT_CCHSTART_URC_TIMEOUT_MS);
        TickType_t urc_start = xTaskGetTickCount();
        HttpUrcEvent_t ev;
        bool got_result = false;

        while (!DEADLINE_PASSED(urc_start, urc_ticks) &&
               !DEADLINE_PASSED(start, total_ticks))
        {
            TickType_t rem_urc   = urc_ticks - TICKS_ELAPSED(urc_start);
            TickType_t rem_total = total_ticks - TICKS_ELAPSED(start);
            TickType_t wait = pdMS_TO_TICKS(200u);
            if (wait > rem_urc)   wait = rem_urc;
            if (wait > rem_total) wait = rem_total;

            if (xQueueReceive(s_ch.urc_queue, &ev, wait) == pdPASS)
            {
                if (ev.type == HTTP_URC_CCHSTART)
                {
                    got_result = true;
                    if (ev.param != 0)
                        return AT_READY_CCHSTART_FAIL;
                    break;
                }
                /* Discard unrelated startup URCs */
            }
        }

        if (!got_result)
            return AT_READY_CCHSTART_FAIL;
    }

    return AT_READY_OK;
}

/* -------------------------------------------------------------------------- */

/**
 * at_channel_set_capture — register an optional text capture buffer.
 *
 * BUG-AT-3: The three fields (buf, size, pos) must be updated atomically
 * because rx_task reads them from a different context without any mutex.
 * A critical section makes the update appear atomic on single-core Cortex-M.
 */
void at_channel_set_capture(char *buf, uint16_t size)
{
    taskENTER_CRITICAL();
    s_ch.capture_buf  = buf;
    s_ch.capture_size = size;
    s_ch.capture_pos  = 0u;
    if (buf && size > 0u)
        buf[0] = '\0';
    taskEXIT_CRITICAL();
}

/* -------------------------------------------------------------------------- */

/**
 * at_channel_send_cmd — send an AT command and wait for OK / ERROR / timeout.
 *
 * BUG-AT-1: Both UART_Sys_Send calls are now checked. If the command TX
 * fails (DMA busy, timeout), we clear expecting_response, release the mutex,
 * and return AT_TIMEOUT immediately instead of waiting the full timeout_ms.
 */
AtResult_t at_channel_send_cmd(const char *cmd, uint32_t timeout_ms)
{
    if (!s_ch.initialized || !cmd)
        return AT_ERROR;

    xSemaphoreTake(s_ch.cmd_mutex, portMAX_DELAY);

    s_ch.pending_result     = AT_TIMEOUT;
    s_ch.expecting_response = true;

    bool tx_ok =
        UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)cmd,
                      (uint16_t)strlen(cmd), AT_COMM_TIMEOUT) &&
        UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)"\r\n", 2u,
                      AT_COMM_TIMEOUT);

    AtResult_t result;
    if (!tx_ok)
    {
        s_ch.expecting_response = false;
        result = AT_TIMEOUT;
    }
    else
    {
        bool got = (xSemaphoreTake(s_ch.resp_sem,
                                   pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
        s_ch.expecting_response = false;
        result = got ? s_ch.pending_result : AT_TIMEOUT;
    }

    xSemaphoreGive(s_ch.cmd_mutex);
    return result;
}

/* -------------------------------------------------------------------------- */

/**
 * at_channel_send_binary — two-phase binary send.
 *
 * BUG-AT-1: TX failures are detected and returned immediately.
 * BUG-AT-2: The mutex is released on every return path (explicit single exit).
 */
AtResult_t at_channel_send_binary(const char    *cmd,
                                   const uint8_t *data,
                                   size_t         len,
                                   uint32_t       timeout_ms)
{
    if (!s_ch.initialized || !cmd || !data)
        return AT_ERROR;

    xSemaphoreTake(s_ch.cmd_mutex, portMAX_DELAY);

    AtResult_t result = AT_TIMEOUT;

    /* Phase 1: send command, wait for '>' prompt */
    s_ch.pending_result     = AT_TIMEOUT;
    s_ch.expecting_response = true;

    bool tx_ok =
        UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)cmd,
                      (uint16_t)strlen(cmd), AT_COMM_TIMEOUT) &&
        UART_Sys_Send(s_ch.uart_ctx, (const uint8_t *)"\r\n", 2u,
                      AT_COMM_TIMEOUT);

    if (!tx_ok)
    {
        s_ch.expecting_response = false;
        result = AT_TIMEOUT;
        goto done;
    }

    {
        bool got_prompt = (xSemaphoreTake(s_ch.resp_sem,
                                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
        if (!got_prompt || s_ch.pending_result != AT_PROMPT)
        {
            s_ch.expecting_response = false;
            result = got_prompt ? AT_ERROR : AT_TIMEOUT;
            goto done;
        }
    }

    /* Phase 2: stream binary payload, wait for OK / ERROR */
    s_ch.pending_result     = AT_TIMEOUT;
    s_ch.expecting_response = true;

    {
        const uint8_t *ptr    = data;
        size_t         remain = len;
        while (remain > 0u)
        {
            uint16_t slice = (remain > AT_TX_CHUNK_SIZE)
                             ? (uint16_t)AT_TX_CHUNK_SIZE : (uint16_t)remain;
            if (!UART_Sys_Send(s_ch.uart_ctx, ptr, slice, AT_COMM_TIMEOUT))
            {
                s_ch.expecting_response = false;
                result = AT_TIMEOUT;
                goto done;
            }
            ptr    += slice;
            remain -= slice;
        }
    }

    {
        bool got_ok = (xSemaphoreTake(s_ch.resp_sem,
                                      pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
        s_ch.expecting_response = false;
        result = got_ok ? s_ch.pending_result : AT_TIMEOUT;
    }

done:
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
    if (s_ch.cmd_mutex) { vSemaphoreDelete(s_ch.cmd_mutex); s_ch.cmd_mutex = NULL; }
    if (s_ch.resp_sem)  { vSemaphoreDelete(s_ch.resp_sem);  s_ch.resp_sem  = NULL; }
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

        for (uint16_t i = 0u; i < frame.length; i++)
            process_byte(frame.payload[i]);

        UART_Sys_ReleaseBuffer(frame.payload);
    }
}

/* ══════════════════════════ Line Reassembler ════════════════════════════════*/

static void process_byte(uint8_t b)
{
    /* '>' on an otherwise empty line: immediate data-entry prompt */
    if (b == '>' && s_ch.line_pos == 0u)
    {
        dispatch_line(">", 1u);
        return;
    }

    if (b == '\n')
    {
        uint16_t dlen = s_ch.line_pos;
        if (dlen > 0u && s_ch.line_buf[dlen - 1u] == '\r')
            dlen--;

        if (dlen > 0u)
        {
            s_ch.line_buf[dlen] = '\0';
            dispatch_line(s_ch.line_buf, dlen);
        }
        s_ch.line_pos = 0u;
        return;
    }

    if (s_ch.line_pos >= AT_LINE_BUF_SIZE - 1u)
    {
        s_ch.line_pos = 0u;   /* overflow: discard and restart */
        return;
    }

    s_ch.line_buf[s_ch.line_pos++] = (char)b;
}

/* ══════════════════════════ Line Dispatcher ═════════════════════════════════*/

static void dispatch_line(const char *line, uint16_t len)
{
    if (len == 1u && line[0] == '>')
    {
        signal_result(AT_PROMPT);
        return;
    }
    if (len == 2u && line[0] == 'O' && line[1] == 'K')
    {
        signal_result(AT_OK);
        return;
    }
    if (len == 5u && memcmp(line, "ERROR", 5u) == 0)
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

    /* Informational text — append to capture buffer if active */
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

/*
 * OPTIM-2: Single bounds check at entry. Appends line + separator '\n' to
 * the capture buffer. Never writes past capture_size - 1 (null terminator).
 */
static void capture_append(const char *line, uint16_t len)
{
    if (!s_ch.capture_buf || s_ch.capture_size == 0u)
        return;

    uint16_t space = s_ch.capture_size - s_ch.capture_pos - 1u;
    if (space == 0u)
        return;

    uint16_t copy = (len < space) ? len : space;
    memcpy(s_ch.capture_buf + s_ch.capture_pos, line, copy);
    s_ch.capture_pos += copy;

    /* Append separator only if there is still space for it */
    if (s_ch.capture_pos < s_ch.capture_size - 1u)
        s_ch.capture_buf[s_ch.capture_pos++] = '\n';

    s_ch.capture_buf[s_ch.capture_pos] = '\0';
}

/* ─────────────────────────── URC Recognition ───────────────────────────── */

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

/* ─────────────────────────── Lightweight int parser (OPTIM-1) ───────────── */

/**
 * parse_int — parse a leading optional-space integer from @p s into @p out.
 * Returns the number of characters consumed (including leading spaces), or 0
 * if no digit was found.  Replaces sscanf("%d") in the URC hot path.
 */
static int parse_int(const char *s, int *out)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;

    if (*p < '0' || *p > '9')
        return 0;

    int val = 0;
    while (*p >= '0' && *p <= '9')
        val = val * 10 + (*p++ - '0');

    *out = val;
    return (int)(p - s);
}

/* ─────────────────────────── URC Parser ────────────────────────────────────*/

static void parse_and_forward_urc(const char *line)
{
    HttpUrcEvent_t ev = { .client_idx = -1, .param = 0 };
    int id  = -1;
    int err =  0;

    if (strncmp(line, "+CCHSTART:", 10) == 0)
    {
        ev.type = HTTP_URC_CCHSTART;
        parse_int(line + 10, &err);
        ev.param = (int8_t)err;
    }
    else if (strncmp(line, "+CCHSTOP:", 9) == 0)
    {
        ev.type = HTTP_URC_CCHSTOP;
        parse_int(line + 9, &err);
        ev.param = (int8_t)err;
    }
    else if (strncmp(line, "+CCHOPEN:", 9) == 0)
    {
        ev.type = HTTP_URC_CCHOPEN;
        int n = parse_int(line + 9, &id);
        if (n > 0 && line[9 + n] == ',')
            parse_int(line + 9 + n + 1, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCHCLOSE:", 10) == 0)
    {
        ev.type = HTTP_URC_CCHCLOSE;
        int n = parse_int(line + 10, &id);
        if (n > 0 && line[10 + n] == ',')
            parse_int(line + 10 + n + 1, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCHEVENT:", 10) == 0)
    {
        ev.type = HTTP_URC_CCHEVENT;
        parse_int(line + 10, &id);
        ev.client_idx = (int8_t)id;
        /* param = 0; second field "RECV EVENT" has no numeric value */
    }
    else if (strncmp(line, "+CCH_RECV_CLOSED:", 18) == 0)
    {
        ev.type = HTTP_URC_CCH_RECV_CLOSED;
        int n = parse_int(line + 18, &id);
        if (n > 0 && line[18 + n] == ',')
            parse_int(line + 18 + n + 1, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCHSEND:", 9) == 0)
    {
        ev.type = HTTP_URC_CCHSEND_RESULT;
        int n = parse_int(line + 9, &id);
        if (n > 0 && line[9 + n] == ',')
            parse_int(line + 9 + n + 1, &err);
        ev.client_idx = (int8_t)id;
        ev.param      = (int8_t)err;
    }
    else if (strncmp(line, "+CCH_PEER_CLOSED:", 17) == 0)
    {
        ev.type = HTTP_URC_CCH_PEER_CLOSED;
        parse_int(line + 17, &id);
        ev.client_idx = (int8_t)id;
    }
    else if (strncmp(line, "+CCH:", 5) == 0)
    {
        ev.type = HTTP_URC_CCH_STOP;
        /* client_idx = -1, param = 0 */
    }
    else
    {
        return;
    }

    xQueueSend(s_ch.urc_queue, &ev, 0);
}

/* ─────────────────────────── Registration parser ───────────────────────── */

/**
 * parse_stat — extract <stat> from a +CGREG / +CEREG / +CREG response.
 *
 * Two forms:
 *   +CGREG: <n>,<stat>         — when n=1 or n=2 (URC reporting enabled)
 *   +CGREG: <stat>             — when n=0 (default, no URC)
 *
 * We skip the first integer (<n>) and read the second (<stat>).
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

        int  first = -1, second = -1;
        int  n = parse_int(pos, &first);

        if (n > 0 && pos[n] == ',')
        {
            /* Two-field form: skip <n>, parse <stat> */
            parse_int(pos + n + 1, &second);
            *stat_out = second;
        }
        else if (n > 0)
        {
            /* One-field form */
            *stat_out = first;
        }
        else
        {
            continue;
        }
        return true;
    }
    return false;
}
