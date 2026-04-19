/**
 * @file    main.h
 * @brief   Bootloader pin definitions and peripheral declarations.
 *
 * Mirror of the application main.h for FRAM SPI1 GPIO mapping only.
 * No FreeRTOS, no UI, no sensors — bootloader uses SPI1, IWDG, Flash.
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
#define FRAM_CS_Pin           GPIO_PIN_4
#define FRAM_CS_GPIO_Port     GPIOA
#define FRAM_SCK_Pin          GPIO_PIN_5
#define FRAM_SCK_GPIO_Port    GPIOA
#define FRAM_MSIO_Pin         GPIO_PIN_6   /* MISO */
#define FRAM_MSIO_GPIO_Port   GPIOA
#define FRAM_MOSI_Pin         GPIO_PIN_7
#define FRAM_MOSI_GPIO_Port   GPIOA

/* ── Nucleo on-board LED (PA5 clashes with SPI SCK — use PC2 green) ── */
#define BOOT_LED_Pin          GPIO_PIN_2
#define BOOT_LED_GPIO_Port    GPIOC

/* ── Peripheral handles (defined in bootloader/src/main.c) ─────────── */
extern SPI_HandleTypeDef  hspi1;
extern IWDG_HandleTypeDef hiwdg;

/* ── Error handler ──────────────────────────────────────────────────── */
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
