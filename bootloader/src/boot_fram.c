/**
 * @file    boot_fram.c
 * @brief   Minimal polling SPI1 FRAM driver for the STM32L476RG bootloader.
 *
 * Protocol: CY15B116QN SPI, Mode 0 (CPOL=0, CPHA=0), MSB first, 8-bit.
 * CS is driven manually so the command header and data payload can be
 * transmitted in separate HAL calls while keeping CS asserted throughout.
 */

#include "boot_fram.h"

/* ── FRAM opcode table (subset used by the bootloader) ──────────────── */
#define CMD_WREN    0x06u   /* Set Write Enable Latch  */
#define CMD_READ    0x03u   /* Read Data Bytes         */
#define CMD_WRITE   0x02u   /* Write Data Bytes        */

/** Maximum byte count per HAL SPI transfer (HAL uses uint16_t Size).   */
#define HAL_SPI_MAX_LEN  65535u

/** SPI transaction timeout in ms.  Worst case: 65535 bytes at 1 MHz. */
#define FRAM_TIMEOUT_MS  200u

/* ── Module state ───────────────────────────────────────────────────── */
static SPI_HandleTypeDef *hfram = NULL;

/* ── Chip-select helpers ────────────────────────────────────────────── */
static inline void cs_assert(void)
{
    HAL_GPIO_WritePin(FRAM_CS_GPIO_Port, FRAM_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_deassert(void)
{
    HAL_GPIO_WritePin(FRAM_CS_GPIO_Port, FRAM_CS_Pin, GPIO_PIN_SET);
}

/* ── Private: send WREN opcode ─────────────────────────────────────── */
static bool send_wren(void)
{
    uint8_t cmd = CMD_WREN;
    bool ok;

    cs_assert();
    ok = (HAL_SPI_Transmit(hfram, &cmd, 1u, FRAM_TIMEOUT_MS) == HAL_OK);
    cs_deassert();
    return ok;
}

/* ================================================================ */
/* Public API                                                        */
/* ================================================================ */

/**
 * @brief  Initialise the FRAM driver.
 */
bool boot_fram_init(SPI_HandleTypeDef *hspi)
{
    if (hspi == NULL)
        return false;

    hfram = hspi;
    cs_deassert();   /* ensure CS starts deasserted */

    /* Verify bus is alive with a WREN cycle. */
    if (!send_wren())
    {
        hfram = NULL;
        return false;
    }
    return true;
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Read bytes from the FRAM main array.
 */
uint32_t boot_fram_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint8_t header[4];
    uint32_t remaining;
    uint32_t offset;
    uint16_t chunk;

    if (hfram == NULL || data == NULL || len == 0u)
        return 0u;

    /* Build command + 24-bit address (MSB first). */
    header[0] = CMD_READ;
    header[1] = (uint8_t)((addr >> 16u) & 0xFFu);
    header[2] = (uint8_t)((addr >>  8u) & 0xFFu);
    header[3] = (uint8_t)( addr         & 0xFFu);

    cs_assert();

    /* Send command header. */
    if (HAL_SPI_Transmit(hfram, header, 4u, FRAM_TIMEOUT_MS) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    /* Receive data in HAL_SPI_MAX_LEN chunks (handles transfers > 65535 B). */
    remaining = len;
    offset    = 0u;

    while (remaining > 0u)
    {
        chunk = (uint16_t)((remaining > HAL_SPI_MAX_LEN) ? HAL_SPI_MAX_LEN : remaining);

        if (HAL_SPI_Receive(hfram, data + offset, chunk, FRAM_TIMEOUT_MS) != HAL_OK)
        {
            cs_deassert();
            return 0u;
        }

        offset    += chunk;
        remaining -= chunk;
    }

    cs_deassert();
    return len;
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Write bytes to the FRAM main array.
 */
uint32_t boot_fram_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t header[4];

    if (hfram == NULL || data == NULL || len == 0u)
        return 0u;

    /* Issue WREN (CS must be toggled between WREN and WRITE). */
    if (!send_wren())
        return 0u;

    /* Build command + 24-bit address (MSB first). */
    header[0] = CMD_WRITE;
    header[1] = (uint8_t)((addr >> 16u) & 0xFFu);
    header[2] = (uint8_t)((addr >>  8u) & 0xFFu);
    header[3] = (uint8_t)( addr         & 0xFFu);

    cs_assert();

    /* Send command header. */
    if (HAL_SPI_Transmit(hfram, header, 4u, FRAM_TIMEOUT_MS) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    /* Send data payload (len ≤ 65535 for all OCB and page writes). */
    if (len > HAL_SPI_MAX_LEN)
    {
        cs_deassert();
        return 0u;   /* caller must split oversized transfers */
    }

    if (HAL_SPI_Transmit(hfram, (uint8_t *)data, (uint16_t)len, FRAM_TIMEOUT_MS) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    cs_deassert();
    return len;
}
