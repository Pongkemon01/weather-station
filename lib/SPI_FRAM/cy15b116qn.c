#include "cy15b116qn.h"

// FRAM Commands
// Write enable control
#define WREN 0x06 // Set Write Enable Latch
#define WRDI 0x04 // Reset Write Enable Latch
// Register access
#define RDSR 0x05 // Read Status Register
#define WRSR 0x01 // Write Status Register
// Read/Write
#define FSTRD 0x0B // Fast Read Data Bytes (for compatiblity with SPI Flash)
#define READ 0x03  // Read Data Byte
#define WRITE 0x02 // Write Data Byte
// Special sector memory access
#define SSWR 0x42 // Special Sector Write
#define SSRD 0x4B // Special Sector Read
// Misc
#define RDID 0x9F // Read Device ID
#define RUID 0x4C // Read Unique ID
#define WRSN 0xC2 // Write Serial Number
#define RDSN 0xC3 // Read Serial Number
// Low-power mode control
#define DPD 0xBA // Enter deep power-down
#define HBN 0xB9 // Enter hibernate mode

// Useable macros
#define FRAM_CS_ON() HAL_GPIO_WritePin(FRAM_CS_PORT, FRAM_CS_PIN, GPIO_PIN_RESET)
#define FRAM_CS_OFF() HAL_GPIO_WritePin(FRAM_CS_PORT, FRAM_CS_PIN, GPIO_PIN_SET)

// Global variable
static SPI_HandleTypeDef *hfram;

/* =============================================================== */
/* API implementation */
bool fram_init(SPI_HandleTypeDef *hframspi)
{
    hfram = hframspi;

    if(fram_write_enable())
        return true;
    
    hfram = NULL;
    return false;
}

/* --------------------------------------------------- */
bool fram_write_enable(void)
{
    uint8_t cmd = WREN;
    bool ret;

    if(hfram == NULL)
        return false;

    FRAM_CS_ON();
    ret = (HAL_SPI_Transmit(hfram, &cmd, 1, FRAM_TIMEOUT) == HAL_OK);
    FRAM_CS_OFF();

    return ret;
}

/* --------------------------------------------------- */
bool fram_write_disable(void)
{
    uint8_t cmd = WRDI;
    bool ret;

    if(hfram == NULL)
        return false;

    FRAM_CS_ON();
    ret = (HAL_SPI_Transmit(hfram, &cmd, 1, FRAM_TIMEOUT) == HAL_OK);
    FRAM_CS_OFF();

    return ret;
}

/* --------------------------------------------------- */
uint32_t fram_write(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint8_t cmd[4];
    uint32_t written_len;

    if(hfram == NULL)
        return 0;

    cmd[0] = WRITE;
    cmd[1] = (uint8_t)((addr >> 16) & 0x1F); // Limit address to only 21 bits
    cmd[2] = (uint8_t)((addr >> 8) & 0xFF);
    cmd[3] = (uint8_t)(addr & 0xFF);

    FRAM_CS_ON();
    if (HAL_SPI_Transmit(hfram, &cmd, 4, FRAM_TIMEOUT) != HAL_OK)
    {
        FRAM_CS_OFF();
        return 0;
    }

    /* Write the data. Since HAL_SPI_Transmit can process data with 65535 maximum length.
       If the desired length is greater than this maximum, we should send the data in chunks. */
    written_len = 0;
    while (len > 65535)
    {
        if (HAL_SPI_Transmit(hfram, data, 65535, FRAM_TIMEOUT) != HAL_OK)
        {
            FRAM_CS_OFF();
            return written_len;
        }
        len -= 65535u;
        data += 65535u;
        written_len += 65535u;
    }

    if (HAL_SPI_Transmit(hfram, data, len, FRAM_TIMEOUT) != HAL_OK)
    {
        FRAM_CS_OFF();
        return written_len;
    }
    FRAM_CS_OFF();

    return written_len + len;
}

/* --------------------------------------------------- */
uint32_t fram_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint8_t cmd[4];
    uint32_t read_len;

    if(hfram == NULL)
        return 0;

    cmd[0] = READ;
    cmd[1] = (uint8_t)((addr >> 16) & 0x1F); // Limit address to only 21 bits
    cmd[2] = (uint8_t)((addr >> 8) & 0xFF);
    cmd[3] = (uint8_t)(addr & 0xFF);

    FRAM_CS_ON();
    if (HAL_SPI_Transmit(hfram, &cmd, 4, FRAM_TIMEOUT) != HAL_OK)
    {
        FRAM_CS_OFF();
        return 0;
    }

    /* Read the data. Since HAL_SPI_Receive can process data with 65535 maximum length.
       If the desired length is greater than this maximum, we should receive the data in chunks. */
    read_len = 0;
    while (len > 65535)
    {
        if (HAL_SPI_Receive(hfram, data, 65535, FRAM_TIMEOUT) != HAL_OK)
        {
            FRAM_CS_OFF();
            return read_len;
        }
        len -= 65535u;
        data += 65535u;
        read_len += 65535u;
    }

    if (HAL_SPI_Receive(hfram, data, len, FRAM_TIMEOUT) != HAL_OK)
    {
        FRAM_CS_OFF();
        return read_len;
    }
    FRAM_CS_OFF();

    return read_len + len;
}

/* --------------------------------------------------- */
uint16_t fram_write_special_sector(uint8_t addr, uint8_t *data, uint16_t len)
{
    uint8_t cmd[4];
    uint16_t actual_len;

    if(hfram == NULL)
        return 0;

    // Limit the actual length of data to write
    if ((uint16_t)addr + len > 256)
        actual_len = 256 - (uint16_t)addr;
    else
        actual_len = len;

    cmd[0] = SSWR;
    cmd[1] = 0;
    cmd[2] = 0;
    cmd[3] = addr;

    /* Writing to special sector starts with WREN command */
    if (!(fram_write_enable()))
        return 0;

    /* Start writing process */
    FRAM_CS_ON();
    if (HAL_SPI_Transmit(hfram, cmd, 4, FRAM_TIMEOUT) != HAL_OK)
    {
        FRAM_CS_OFF();
        return 0;
    }

    if (HAL_SPI_Transmit(hfram, data, actual_len, FRAM_TIMEOUT) != HAL_OK)
    {
        actual_len = 0;
    }
    FRAM_CS_OFF();

    return actual_len;
}

/* --------------------------------------------------- */
uint16_t fram_read_special_sector(uint8_t addr, uint8_t *data, uint16_t len)
{
    uint8_t cmd[4];
    uint16_t actual_len;

    if(hfram == NULL)
        return 0;

    cmd[0] = SSRD;
    cmd[1] = 0;
    cmd[2] = 0;
    cmd[3] = addr;

    // Limit the actual length of data to read (address autoincrement to 0xFF only)
    if ((uint16_t)addr + len > 256)
        actual_len = 256 - (uint16_t)addr;
    else
        actual_len = len;

    FRAM_CS_ON();
    if (HAL_SPI_Transmit(hfram, cmd, 4, FRAM_TIMEOUT) != HAL_OK)
    {
        FRAM_CS_OFF();
        return 0;
    }

    if (HAL_SPI_Receive(hfram, data, actual_len, FRAM_TIMEOUT) != HAL_OK)
    {
        actual_len = 0;
    }
    FRAM_CS_OFF();

    return actual_len;
}

/* --------------------------------------------------- */
uint8_t fram_read_status_register(void)
{
    uint8_t cmd = RDSR;
    uint8_t ret;

    if(hfram == NULL)
        return 0;

    FRAM_CS_ON();
    HAL_SPI_Transmit(hfram, &cmd, 1, FRAM_TIMEOUT);
    HAL_SPI_Receive(hfram, &ret, 1, FRAM_TIMEOUT);
    FRAM_CS_OFF();

    return ret;
}

/* --------------------------------------------------- */
bool fram_write_status_register(uint8_t data)
{
    uint8_t cmd[2];
    HAL_StatusTypeDef ret;

    if(hfram == NULL)
        return false;

    cmd[0] = WRSR;
    cmd[1] = data;

    FRAM_CS_ON();
    ret = (HAL_SPI_Transmit(hfram, &cmd, 2, FRAM_TIMEOUT) == HAL_OK);
    FRAM_CS_OFF();

    return ret;
}
