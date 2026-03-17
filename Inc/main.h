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

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

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

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
