/**
 * @file    cy15b116qn.c
 * @brief   Driver for Cypress CY15B116QN 2Mb SPI F-RAM
 *
 * Thread-safety notes:
 *   All public functions are reentrant with respect to the SPI bus.
 *   The caller (nv_database) is responsible for higher-level mutual
 *   exclusion when combining multiple FRAM operations into a logical
 *   transaction.  The WREN issued inside fram_write() is atomic with
 *   respect to the subsequent data transfer because CS is kept asserted
 *   across the whole sequence — the datasheet (section 6.2) requires
 *   WREN to be the immediately preceding transaction, not the same one.
 *   Therefore WREN is sent, CS de-asserted, CS re-asserted, then WRITE
 *   is sent, exactly as the datasheet shows.
 */

#include "cy15b116qn.h"

/* ---- FRAM opcode table ----------------------------------------- */
#define CMD_WREN    0x06u   /* Set Write Enable Latch              */
#define CMD_WRDI    0x04u   /* Reset Write Enable Latch            */
#define CMD_RDSR    0x05u   /* Read Status Register                */
#define CMD_WRSR    0x01u   /* Write Status Register               */
#define CMD_FSTRD   0x0Bu   /* Fast Read (SPI-Flash compatibility) */
#define CMD_READ    0x03u   /* Read Data Bytes                     */
#define CMD_WRITE   0x02u   /* Write Data Bytes                    */
#define CMD_SSWR    0x42u   /* Special Sector Write                */
#define CMD_SSRD    0x4Bu   /* Special Sector Read                 */
#define CMD_RDID    0x9Fu   /* Read Device ID                      */
#define CMD_RUID    0x4Cu   /* Read Unique ID                      */
#define CMD_WRSN    0xC2u   /* Write Serial Number                 */
#define CMD_RDSN    0xC3u   /* Read Serial Number                  */
#define CMD_DPD     0xBAu   /* Enter Deep Power-Down               */
#define CMD_HBN     0xB9u   /* Enter Hibernate Mode                */

/* Maximum single HAL SPI transfer length (HAL uses uint16_t Size)  */
#define HAL_SPI_MAX_LEN  65535u

/* ---- Chip-select helpers --------------------------------------- */
static inline void cs_assert(void)
{
    HAL_GPIO_WritePin(FRAM_CS_PORT, FRAM_CS_PIN, GPIO_PIN_RESET);
}

static inline void cs_deassert(void)
{
    HAL_GPIO_WritePin(FRAM_CS_PORT, FRAM_CS_PIN, GPIO_PIN_SET);
}

/* ---- Module state --------------------------------------------- */
static SPI_HandleTypeDef *hfram = NULL;

/* ================================================================ */
/* Public API                                                        */
/* ================================================================ */

/**
 * @brief  Initialise the FRAM driver.
 * @param  hframspi  Pointer to a fully-configured HAL SPI handle.
 * @retval true on success, false if the first SPI transaction fails.
 *
 * Safe to call once at startup before the RTOS scheduler starts.
 * Do not call concurrently from multiple tasks.
 */
bool fram_init(SPI_HandleTypeDef *hframspi)
{
    if (hframspi == NULL)
        return false;

    hfram = hframspi;

    /* Verify the SPI bus is alive by attempting a WREN cycle. */
    if (!fram_write_enable())
    {
        hfram = NULL;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Send the WREN opcode so the next write / erase is accepted.
 *
 * The CY15B116QN clears the WEL bit after every write operation, so
 * this must be called before every WRITE / WRSR / SSWR transaction.
 * fram_write() and fram_write_special_sector() call this internally;
 * external callers should not need it in normal use.
 */
bool fram_write_enable(void)
{
    static const uint8_t cmd = CMD_WREN;

    if (hfram == NULL)
        return false;

    cs_assert();
    bool ok = (HAL_SPI_Transmit(hfram, (uint8_t *)&cmd, 1, FRAM_TIMEOUT) == HAL_OK);
    cs_deassert();
    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Send the WRDI opcode to clear the Write Enable Latch.
 */
bool fram_write_disable(void)
{
    static const uint8_t cmd = CMD_WRDI;

    if (hfram == NULL)
        return false;

    cs_assert();
    bool ok = (HAL_SPI_Transmit(hfram, (uint8_t *)&cmd, 1, FRAM_TIMEOUT) == HAL_OK);
    cs_deassert();
    return ok;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Write @p len bytes from @p data to FRAM starting at @p addr.
 *
 * Handles the HAL 65535-byte transfer-size limit by chunking.
 * Issues its own WREN immediately before asserting CS for the write,
 * exactly as required by the CY15B116QN datasheet (§6.2).
 *
 * @param  addr  21-bit FRAM byte address (0 … FRAM_MAX_ADDR).
 * @param  data  Source buffer (must remain valid for the call duration).
 * @param  len   Number of bytes to write.
 * @retval Actual number of bytes written; 0 on error.
 */
uint32_t fram_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t  cmd[4];
    uint32_t written = 0u;

    if (hfram == NULL || data == NULL || len == 0u)
        return 0u;

    /* Clamp to the physical address space */
    if (addr > FRAM_MAX_ADDR)
        return 0u;
    if (len > (FRAM_MAX_ADDR - addr + 1u))
        len = FRAM_MAX_ADDR - addr + 1u;

    cmd[0] = CMD_WRITE;
    cmd[1] = (uint8_t)((addr >> 16u) & 0x1Fu); /* 21-bit address, MSB */
    cmd[2] = (uint8_t)((addr >>  8u) & 0xFFu);
    cmd[3] = (uint8_t)( addr         & 0xFFu);

    /*
     * WREN must be the transaction immediately before the WRITE
     * (§6.2).  CS must be de-asserted between them.
     */
    if (!fram_write_enable())
        return 0u;

    cs_assert();

    if (HAL_SPI_Transmit(hfram, cmd, 4u, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    /* Stream data in ≤65535-byte chunks to work around HAL limit. */
    while (len > HAL_SPI_MAX_LEN)
    {
        if (HAL_SPI_Transmit(hfram, (uint8_t *)data, HAL_SPI_MAX_LEN, FRAM_TIMEOUT) != HAL_OK)
        {
            cs_deassert();
            return written;
        }
        data    += HAL_SPI_MAX_LEN;
        len     -= HAL_SPI_MAX_LEN;
        written += HAL_SPI_MAX_LEN;
    }

    if (HAL_SPI_Transmit(hfram, (uint8_t *)data, (uint16_t)len, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return written;
    }

    cs_deassert();
    return written + len;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Read @p len bytes from FRAM starting at @p addr into @p data.
 *
 * @param  addr  21-bit FRAM byte address.
 * @param  data  Destination buffer.
 * @param  len   Number of bytes to read.
 * @retval Actual number of bytes read; 0 on error.
 */
uint32_t fram_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint8_t  cmd[4];
    uint32_t nread = 0u;

    if (hfram == NULL || data == NULL || len == 0u)
        return 0u;

    if (addr > FRAM_MAX_ADDR)
        return 0u;
    if (len > (FRAM_MAX_ADDR - addr + 1u))
        len = FRAM_MAX_ADDR - addr + 1u;

    cmd[0] = CMD_READ;
    cmd[1] = (uint8_t)((addr >> 16u) & 0x1Fu);
    cmd[2] = (uint8_t)((addr >>  8u) & 0xFFu);
    cmd[3] = (uint8_t)( addr         & 0xFFu);

    cs_assert();

    if (HAL_SPI_Transmit(hfram, cmd, 4u, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    while (len > HAL_SPI_MAX_LEN)
    {
        if (HAL_SPI_Receive(hfram, data, HAL_SPI_MAX_LEN, FRAM_TIMEOUT) != HAL_OK)
        {
            cs_deassert();
            return nread;
        }
        data  += HAL_SPI_MAX_LEN;
        len   -= HAL_SPI_MAX_LEN;
        nread += HAL_SPI_MAX_LEN;
    }

    if (HAL_SPI_Receive(hfram, data, (uint16_t)len, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return nread;
    }

    cs_deassert();
    return nread + len;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Write up to @p len bytes into the 256-byte Special Sector.
 *
 * The Special Sector address range is 0x00–0xFF.  Writes that would
 * overflow the sector are silently clamped.
 *
 * @param  addr  Byte offset within the Special Sector (0–255).
 * @param  data  Source buffer.
 * @param  len   Number of bytes to write.
 * @retval Actual bytes written; 0 on error.
 */
uint16_t fram_write_special_sector(uint8_t addr, const uint8_t *data, uint16_t len)
{
    uint8_t  cmd[4];
    uint16_t actual_len;

    if (hfram == NULL || data == NULL || len == 0u)
        return 0u;

    /* Clamp to the 256-byte sector boundary. */
    actual_len = ((uint16_t)addr + len > 256u) ? (256u - (uint16_t)addr) : len;

    cmd[0] = CMD_SSWR;
    cmd[1] = 0u;
    cmd[2] = 0u;
    cmd[3] = addr;

    if (!fram_write_enable())
        return 0u;

    cs_assert();

    if (HAL_SPI_Transmit(hfram, cmd, 4u, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    if (HAL_SPI_Transmit(hfram, (uint8_t *)data, actual_len, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    cs_deassert();
    return actual_len;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Read up to @p len bytes from the 256-byte Special Sector.
 *
 * @param  addr  Byte offset within the Special Sector (0–255).
 * @param  data  Destination buffer.
 * @param  len   Number of bytes to read.
 * @retval Actual bytes read; 0 on error.
 */
uint16_t fram_read_special_sector(uint8_t addr, uint8_t *data, uint16_t len)
{
    uint8_t  cmd[4];
    uint16_t actual_len;

    if (hfram == NULL || data == NULL || len == 0u)
        return 0u;

    actual_len = ((uint16_t)addr + len > 256u) ? (256u - (uint16_t)addr) : len;

    cmd[0] = CMD_SSRD;
    cmd[1] = 0u;
    cmd[2] = 0u;
    cmd[3] = addr;

    cs_assert();

    if (HAL_SPI_Transmit(hfram, cmd, 4u, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    if (HAL_SPI_Receive(hfram, data, actual_len, FRAM_TIMEOUT) != HAL_OK)
    {
        cs_deassert();
        return 0u;
    }

    cs_deassert();
    return actual_len;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Read and return the 8-bit Status Register.
 * @retval Status register value; 0 on error (ambiguous — check hfram != NULL first).
 */
uint8_t fram_read_status_register(void)
{
    static const uint8_t cmd = CMD_RDSR;
    uint8_t reg = 0u;

    if (hfram == NULL)
        return 0u;

    cs_assert();
    if (HAL_SPI_Transmit(hfram, (uint8_t *)&cmd, 1u, FRAM_TIMEOUT) == HAL_OK)
        HAL_SPI_Receive(hfram, &reg, 1u, FRAM_TIMEOUT);
    cs_deassert();

    return reg;
}

/* ---------------------------------------------------------------- */
/**
 * @brief  Write @p data to the Status Register.
 *
 * Note: WRSR does NOT require a preceding WREN on the CY15B116QN
 * (the datasheet shows WRSR as an independent command).
 */
bool fram_write_status_register(uint8_t data)
{
    uint8_t cmd[2] = { CMD_WRSR, data };

    if (hfram == NULL)
        return false;

    cs_assert();
    bool ok = (HAL_SPI_Transmit(hfram, cmd, 2u, FRAM_TIMEOUT) == HAL_OK);
    cs_deassert();
    return ok;
}
