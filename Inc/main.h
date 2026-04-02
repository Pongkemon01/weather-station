/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.h
 * @brief          : Header for main.c file.
 *                   This file contains the common defines of the application.
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdbool.h>
#include <stdint.h>

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
    /* Ready status of each peripheral */
    typedef struct
    {
        bool ui_ready;
        bool usart_ready;
        bool modbus_ready;
        bool a7670_ready;
        bool bmp390_ready;
        bool sht45_ready;
        bool fram_ready;
        bool datetime_ready;
        bool rainfall_ok;
        bool light_ok;
        bool sd_detected;
        bool sd_write_protected;
    } System_Ready_Status_t;
    extern System_Ready_Status_t system_ready_status;

    /* UI interface data */
    typedef enum
    {
        LED_OFF = 0,
        LED_ON,
        LED_BLINK
    } UI_LED_State_t;

    typedef struct
    {
        // LCD
        char disp[2][16];      // Display buffer
        bool lcd_need_updated; // "disp" contains new data needed to update the lcd

        // LCD Back-light
        bool lcd_bk_on;

        // LCD Cursor
        bool lcd_cursor_on;

        // LED
        UI_LED_State_t led_red;   // Desire status of red LED
        UI_LED_State_t led_green; // Desire status of green LED

        // Switch status
        bool key_up;
        bool key_down;
        bool key_enter;
        bool key_menu;

        /*
         * Data in this structure are used for communication between UI task and other tasks.
         * Therefore, it is a shared data requiring protection.
         */
        SemaphoreHandle_t mutex;
    } UI_Interface_t;
    extern UI_Interface_t ui_interface;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED_RED_Pin GPIO_PIN_0
#define LED_RED_GPIO_Port GPIOC
#define LED_YELLOW_Pin GPIO_PIN_1
#define LED_YELLOW_GPIO_Port GPIOC
#define LED_GREEN_Pin GPIO_PIN_2
#define LED_GREEN_GPIO_Port GPIOC
#define LED_BLUE_Pin GPIO_PIN_3
#define LED_BLUE_GPIO_Port GPIOC
#define CONSOLE_TX_Pin GPIO_PIN_2
#define CONSOLE_TX_GPIO_Port GPIOA
#define CONSOLE_RX_Pin GPIO_PIN_3
#define CONSOLE_RX_GPIO_Port GPIOA
#define FRAM_CS_Pin GPIO_PIN_4
#define FRAM_CS_GPIO_Port GPIOA
#define FRAM_SCK_Pin GPIO_PIN_5
#define FRAM_SCK_GPIO_Port GPIOA
#define FRAM_MSIO_Pin GPIO_PIN_6
#define FRAM_MSIO_GPIO_Port GPIOA
#define FRAM_MOSI_Pin GPIO_PIN_7
#define FRAM_MOSI_GPIO_Port GPIOA
#define USR_SW1_Pin GPIO_PIN_4
#define USR_SW1_GPIO_Port GPIOC
#define USR_SW2_Pin GPIO_PIN_5
#define USR_SW2_GPIO_Port GPIOC
#define LTE_TX_Pin GPIO_PIN_10
#define LTE_TX_GPIO_Port GPIOB
#define LTE_RX_Pin GPIO_PIN_11
#define LTE_RX_GPIO_Port GPIOB
#define EXT_SCL_Pin GPIO_PIN_13
#define EXT_SCL_GPIO_Port GPIOB
#define EXT_SDA_Pin GPIO_PIN_14
#define EXT_SDA_GPIO_Port GPIOB
#define SD_CD_Pin GPIO_PIN_6
#define SD_CD_GPIO_Port GPIOC
#define SD_WP_Pin GPIO_PIN_7
#define SD_WP_GPIO_Port GPIOC
#define RS485_RX_Pin GPIO_PIN_10
#define RS485_RX_GPIO_Port GPIOA
#define RS485_DE_Pin GPIO_PIN_3
#define RS485_DE_GPIO_Port GPIOB
#define RS485_TX_Pin GPIO_PIN_6
#define RS485_TX_GPIO_Port GPIOB
#define UI_SDA_Pin GPIO_PIN_7
#define UI_SDA_GPIO_Port GPIOB
#define UI_SCL_Pin GPIO_PIN_8
#define UI_SCL_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* Debug LED macros */
#define LED_DEBUG_RED_ON() HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET)
#define LED_DEBUG_RED_OFF() HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_RESET)
#define LED_DEBUG_RED_TOGGLE() HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin)
#define LED_DEBUG_YELLOW_ON() HAL_GPIO_WritePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, GPIO_PIN_SET)
#define LED_DEBUG_YELLOW_OFF() HAL_GPIO_WritePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin, GPIO_PIN_RESET)
#define LED_DEBUG_YELLOW_TOGGLE() HAL_GPIO_TogglePin(LED_YELLOW_GPIO_Port, LED_YELLOW_Pin)
#define LED_DEBUG_GREEN_ON() HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_SET)
#define LED_DEBUG_GREEN_OFF() HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_RESET)
#define LED_DEBUG_GREEN_TOGGLE() HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin)
#define LED_DEBUG_BLUE_ON() HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_SET)
#define LED_DEBUG_BLUE_OFF() HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_RESET)
#define LED_DEBUG_BLUE_TOGGLE() HAL_GPIO_TogglePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin)

/* User switches macros */
#define SW1_DEBUG_STATUS() HAL_GPIO_ReadPin(USR_SW1_GPIO_Port, USR_SW1_Pin)
#define SW2_DEBUG_STATUS() HAL_GPIO_ReadPin(USR_SW2_GPIO_Port, USR_SW2_Pin)

/* SD Card status */
#define SD_INSERTED_STATUS() (HAL_GPIO_ReadPin(SD_CD_GPIO_Port, SD_CD_Pin) == GPIO_PIN_RESET)
#define SD_WRITE_PROTECTED_STATUS() (HAL_GPIO_ReadPin(SD_WP_GPIO_Port, SD_WP_Pin) == GPIO_PIN_RESET)

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
