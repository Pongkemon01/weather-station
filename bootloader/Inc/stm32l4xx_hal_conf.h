/**
 * @file    stm32l4xx_hal_conf.h
 * @brief   Minimal HAL configuration for the STM32L476RG bootloader.
 *
 * Only the peripherals used by the bootloader are enabled:
 *   SPI1  — polling FRAM access (boot_fram.c)
 *   IWDG  — independent watchdog refresh during Flash programming
 *   Flash — page erase and double-word programming (boot_flash.c)
 *   GPIO  — FRAM chip-select
 *   RCC   — clock control
 *   PWR   — required by HAL_Init
 *
 * All other modules are deliberately disabled to keep Flash usage low.
 */

#ifndef STM32L4XX_HAL_CONF_H
#define STM32L4XX_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ########################## Module Selection ############################## */
#define HAL_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED      /* DMA driver referenced by HAL SPI headers */

/* All other modules disabled */
/*#define HAL_ADC_MODULE_ENABLED   */
/*#define HAL_CAN_MODULE_ENABLED   */
/*#define HAL_COMP_MODULE_ENABLED  */
/*#define HAL_CRC_MODULE_ENABLED   */
/*#define HAL_DAC_MODULE_ENABLED   */
/*#define HAL_I2C_MODULE_ENABLED   */
/*#define HAL_I2S_MODULE_ENABLED   */
/*#define HAL_IRDA_MODULE_ENABLED  */
/*#define HAL_LPTIM_MODULE_ENABLED */
/*#define HAL_MMC_MODULE_ENABLED   */
/*#define HAL_NAND_MODULE_ENABLED  */
/*#define HAL_NOR_MODULE_ENABLED   */
/*#define HAL_OPAMP_MODULE_ENABLED */
/*#define HAL_PCD_MODULE_ENABLED   */
/*#define HAL_RNG_MODULE_ENABLED   */
/*#define HAL_RTC_MODULE_ENABLED   */
/*#define HAL_SAI_MODULE_ENABLED   */
/*#define HAL_SD_MODULE_ENABLED    */
/*#define HAL_SMBUS_MODULE_ENABLED */
/*#define HAL_TIM_MODULE_ENABLED   */
/*#define HAL_UART_MODULE_ENABLED  */
/*#define HAL_USART_MODULE_ENABLED */
/*#define HAL_WWDG_MODULE_ENABLED  */
/*#define HAL_EXTI_MODULE_ENABLED  */

/* ########################## Oscillator Values ############################# */
#if !defined(HSE_VALUE)
#define HSE_VALUE    8000000U
#endif

#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT    100U
#endif

#if !defined(MSI_VALUE)
#define MSI_VALUE    4000000U
#endif

#if !defined(HSI_VALUE)
#define HSI_VALUE    16000000U
#endif

#if !defined(HSI48_VALUE)
#define HSI48_VALUE  48000000U
#endif

#if !defined(LSI_VALUE)
#define LSI_VALUE    32000U
#endif

#if !defined(LSE_VALUE)
#define LSE_VALUE    32768U
#endif

#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT    5000U
#endif

#if !defined(EXTERNAL_SAI1_CLOCK_VALUE)
#define EXTERNAL_SAI1_CLOCK_VALUE    2097000U
#endif

#if !defined(EXTERNAL_SAI2_CLOCK_VALUE)
#define EXTERNAL_SAI2_CLOCK_VALUE    2097000U
#endif

/* ########################### System Configuration ######################### */
#define VDD_VALUE               3300U
#define TICK_INT_PRIORITY       15U
#define USE_RTOS                0U
#define PREFETCH_ENABLE         0U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE        1U

/* ################## Register callback feature ############################# */
/**
  * @brief Enable register callbacks — set to 0 to reduce code size.
  */
#define USE_HAL_SPI_REGISTER_CALLBACKS   0U

/* ########################## Assert Selection ############################## */
/* #define USE_FULL_ASSERT  1U */

/**
 * @brief  assert_param macro — no-op for the bootloader (saves code space).
 *         The framework's stm32l4xx_hal_def.h does not define assert_param;
 *         it must be provided here in stm32l4xx_hal_conf.h.
 */
#ifdef  USE_FULL_ASSERT
#  define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
   void assert_failed(uint8_t *file, uint32_t line);
#else
#  define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

/* ########################################################################  */
/* Module header includes — each header pulls in stm32l4xx_hal_def.h which  */
/* includes stm32l4xx.h (CMSIS device) and defines __IO, uint32_t, etc.    */
/* ########################################################################  */
#ifdef HAL_RCC_MODULE_ENABLED
#  include "stm32l4xx_hal_rcc.h"
#  include "stm32l4xx_hal_rcc_ex.h"
#endif

#ifdef HAL_GPIO_MODULE_ENABLED
#  include "stm32l4xx_hal_gpio.h"
#endif

#ifdef HAL_DMA_MODULE_ENABLED
#  include "stm32l4xx_hal_dma.h"
#endif

#ifdef HAL_CORTEX_MODULE_ENABLED
#  include "stm32l4xx_hal_cortex.h"
#endif

#ifdef HAL_FLASH_MODULE_ENABLED
#  include "stm32l4xx_hal_flash.h"
#  include "stm32l4xx_hal_flash_ex.h"
#  include "stm32l4xx_hal_flash_ramfunc.h"
#endif

#ifdef HAL_PWR_MODULE_ENABLED
#  include "stm32l4xx_hal_pwr.h"
#  include "stm32l4xx_hal_pwr_ex.h"
#endif

#ifdef HAL_IWDG_MODULE_ENABLED
#  include "stm32l4xx_hal_iwdg.h"
#endif

#ifdef HAL_SPI_MODULE_ENABLED
#  include "stm32l4xx_hal_spi.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32L4XX_HAL_CONF_H */
