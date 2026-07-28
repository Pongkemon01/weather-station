/**
 * @file    main.h
 * @brief   Bootloader pin definitions and peripheral declarations.
 *
 * Mirror of the application main.h for FRAM SPI1 GPIO and LED pin mapping.
 * No FreeRTOS, no UI, no sensors — bootloader uses SPI1, Flash.
 *
 * WDT toggle: #define BOOTLOADER_WDT_ENABLE to enable IWDG;
 * comment it out to disable the watchdog entirely.
 */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32l4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/* ── FRAM SPI1 GPIO (matches application pin assignment) ───────────── */
#define LED_RED_Pin GPIO_PIN_0
#define LED_RED_GPIO_Port GPIOC
#define LED_YELLOW_Pin GPIO_PIN_1
#define LED_YELLOW_GPIO_Port GPIOC
#define LED_GREEN_Pin GPIO_PIN_2
#define LED_GREEN_GPIO_Port GPIOC
#define LED_BLUE_Pin GPIO_PIN_3
#define LED_BLUE_GPIO_Port GPIOC
#define FRAM_CS_Pin           GPIO_PIN_4
#define FRAM_CS_GPIO_Port     GPIOA
#define FRAM_SCK_Pin          GPIO_PIN_5
#define FRAM_SCK_GPIO_Port    GPIOA
#define FRAM_MSIO_Pin         GPIO_PIN_6   /* MISO */
#define FRAM_MSIO_GPIO_Port   GPIOA
#define FRAM_MOSI_Pin         GPIO_PIN_7
#define FRAM_MOSI_GPIO_Port   GPIOA

/* ── USART2 debug console (PA2=TX, PA3=RX, AF7) ──────────────────────── */
#define CONSOLE_TX_Pin        GPIO_PIN_2
#define CONSOLE_TX_GPIO_Port  GPIOA
#define CONSOLE_RX_Pin        GPIO_PIN_3
#define CONSOLE_RX_GPIO_Port  GPIOA

/* ── Nucleo on-board LED (PA5 clashes with SPI SCK — use PC2 green) ── */
#define BOOT_LED_Pin          GPIO_PIN_2
#define BOOT_LED_GPIO_Port    GPIOC

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

/* ── Peripheral handles (defined in bootloader/src/main.c) ─────────── */
extern SPI_HandleTypeDef  hspi1;
extern UART_HandleTypeDef huart2;
#ifdef BOOTLOADER_WDT_ENABLE
extern IWDG_HandleTypeDef hiwdg;
#endif

/* ── Error handler ──────────────────────────────────────────────────── */
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
