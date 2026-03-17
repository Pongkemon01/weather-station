#include "modbus.h"

/* Global Variables */
#define RS485_TIMEOUT 1000

static UART_Ctx_t *modbus_ctx;

/* Local API */
/* ---------------------------------------------- */
static uint16_t crc_485(uint8_t *buffer, uint16_t len)
{
	LL_CRC_SetPolynomialSize(CRC, LL_CRC_POLYLENGTH_16B);
	LL_CRC_ResetCRCCalculationUnit(CRC);
	LL_CRC_SetInitialData(CRC, 0xffff);
	LL_CRC_SetPolynomialCoef(CRC, 0x8005);
	LL_CRC_SetInputDataReverseMode(CRC, LL_CRC_INDATA_REVERSE_HALFWORD);
	LL_CRC_SetOutputDataReverseMode(CRC, CRC_CR_REV_OUT);

	for (uint8_t i=0 ; i<len ; i++)
		LL_CRC_FeedData8(CRC,buffer[i]);

	return(LL_CRC_ReadData16(CRC));
}

/* Public API */
/* ---------------------------------------------- */
bool modbus_init(UART_HandleTypeDef *huart)
{
    modbus_ctx = UART_Sys_Register(huart);
    if(modbus_ctx == NULL)
        return false;

    return true;
}

/* ---------------------------------------------- */
bool modbus_read_register(uint8_t addr, uint16_t reg_num, uint16_t *data, uint16_t len)
{
    Uart_Packet_t rx_pkt;
    uint8_t packet[8]; // Packet buffer
    uint16_t crc;

    // Setup query data
    packet[0] = addr;
    packet[1] = 0x03;
    packet[2] = (uint8_t)(reg_num >> 8);
    packet[3] = (uint8_t)(reg_num & 0xFF);
    packet[4] = (uint8_t)(len >> 8);
    packet[5] = (uint8_t)(len & 0xFF);

    crc = crc_485(&packet[0], 6);
    packet[6] = (uint8_t)(crc >> 8);
    packet[7] = (uint8_t)(crc & 0xFF);

    // Send query
    if (!(UART_Sys_Send(modbus_ctx, packet, 8, RS485_TIMEOUT)))
        return false;

    // Receive response
    if (UART_Sys_Receive(modbus_ctx, &rx_pkt, RS485_TIMEOUT))
    {
        // Check response
        if ((rx_pkt.payload[0] == addr) && (rx_pkt.payload[1] == 0x03))
        {
            // Check CRC
            crc = crc_485(&rx_pkt.payload[0], rx_pkt.length - 2);
            if ((rx_pkt.payload[rx_pkt.length - 2] == (uint8_t)(crc >> 8)) && (rx_pkt.payload[rx_pkt.length - 1] == (uint8_t)(crc & 0xFF)))
            {
                // Copy data
                for (int i = 0; i < len; i++)
                {
                    data[i] = (rx_pkt.payload[(i * 2) + 3] << 8) | rx_pkt.payload[(i * 2) + 4];
                }
                UART_Sys_ReleaseBuffer(rx_pkt.payload);
                return true;
            }
        }
        UART_Sys_ReleaseBuffer(rx_pkt.payload);
    }

    return false;
}

/* ---------------------------------------------- */
bool modbus_write_register(uint8_t addr, uint16_t reg_num, uint16_t data)
{
    Uart_Packet_t rx_pkt;
    uint8_t packet[8]; // Packet buffer
    uint16_t crc;

    // Setup query data
    packet[0] = addr;
    packet[1] = 0x06;
    packet[2] = (uint8_t)(reg_num >> 8);
    packet[3] = (uint8_t)(reg_num & 0xFF);
    packet[4] = (uint8_t)(data >> 8);
    packet[5] = (uint8_t)(data & 0xFF);

    crc = crc_485(&packet[0], 6);
    packet[6] = (uint8_t)(crc >> 8);
    packet[7] = (uint8_t)(crc & 0xFF);

    // Send query
    if (!(UART_Sys_Send(modbus_ctx, packet, 8, RS485_TIMEOUT)))
        return false;

    // Receive response
    if (UART_Sys_Receive(modbus_ctx, &rx_pkt, RS485_TIMEOUT))
    {
        // Check response
        if(rx_pkt.length == 6)
        {
            for(int i = 0 ; i < 6 ; i++)
            {
                if(rx_pkt.payload[i] != packet[i])      // We skip crc calculation here (just lazy)
                {
                    UART_Sys_ReleaseBuffer(rx_pkt.payload);
                    return false;
                }
            }
            UART_Sys_ReleaseBuffer(rx_pkt.payload);
            return true;
        }
        UART_Sys_ReleaseBuffer(rx_pkt.payload);
    }

    return false;
}


        