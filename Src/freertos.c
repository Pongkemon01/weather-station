/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
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
/*
 * Peripheral allocation:
 * - USART1 : RS485 Modbus RTU (Light addr = 0x01, Rain addr = 0x02)
 * - USART2 : Console port
 * - USART3 : A7670E LTE Modem
 * - I2C1   : MCP23017 (UI Interface) (addr = 0x20)
 * - I2C2   : Sensor bus (BMP390(addr=0x76) and SHT45(addr=0x44))
 * - SPI1   : FRAM (2M x 8bit)
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "crc.h"
#include "dma.h"
#include "fatfs.h"
#include "i2c.h"
#include "rtc.h"
#include "sdmmc.h"
#include "spi.h"
#include "usart.h"
#include "usb_otg.h"
#include "gpio.h"

#include "tusb.h"
#include "uart_subsystem.h"
#include "modbus.h"
#include "bmp390.h"
#include "sht45.h"
#include "a7670.h"
#include "ui.h"
#include "nv_database.h"
#include "weather_data.h"
#include "datetime.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CDC_STACK_SIZE (configMINIMAL_STACK_SIZE * (CFG_TUSB_DEBUG ? 2 : 1))

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

System_Ready_Status_t system_ready_status;

/* Definitions for maintask */
osThreadId_t MainTaskHandle;
const osThreadAttr_t MainTask_attributes = {
    .name = "main",
    .stack_size = 512 * 2,
    .priority = (osPriority_t)osPriorityNormal,
};

/* Definitions for user-control-center task */
osThreadId_t UserControlTaskHandle;
const osThreadAttr_t UserControlTask_attributes = {
    .name = "ucc",
    .stack_size = 512 * 2,
    .priority = (osPriority_t)osPriorityNormal,
};

/* Definitions for ssl upload task */
osThreadId_t SslUploadTaskHandle;
const osThreadAttr_t SslUploadTask_attributes = {
    .name = "sslupload",
    .stack_size = 512 * 2,
    .priority = (osPriority_t)osPriorityNormal,
};

/* Definitions for uitask */
osThreadId_t UITaskHandle;
const osThreadAttr_t UITask_attributes = {
    .name = "ui",
    .stack_size = 512,
    .priority = (osPriority_t)osPriorityNormal,
};

/* Definitions for UsbLoop */
osThreadId_t UsbCDCHandle;
const osThreadAttr_t UsbCDC_attributes = {
    .name = "cdc",
    .stack_size = CDC_STACK_SIZE,
    .priority = (osPriority_t)osPriorityHigh1,
};

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
    .name = "defaultTask",
    .stack_size = 128 * 4,
    .priority = (osPriority_t)osPriorityNormal,
};
/* Definitions for UsbLoop */
osThreadId_t UsbLoopHandle;
const osThreadAttr_t UsbLoop_attributes = {
    .name = "UsbLoop",
    .stack_size = 512 * 4,
    .priority = (osPriority_t)osPriorityHigh,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

extern void cdc_task(void *params);
extern void maintask(void *params);
extern void uitask(void *params);
extern void ucctask(void *params);
extern void ssluploadtask(void *params);

int __io_putchar(int ch)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 0xffff);
    return ch;
}

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void UsbLoopTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
 * @brief  FreeRTOS initialization
 * @param  None
 * @retval None
 */
void MX_FREERTOS_Init(void)
{
    /* USER CODE BEGIN Init */
    /* Initialization stage 1 : Before RTOS scheduler.
       Minimal initialization only
    */

    /* Peripheral initialization */
    LED_DEBUG_RED_ON();
    LED_DEBUG_YELLOW_ON();
    LED_DEBUG_GREEN_ON();
    LED_DEBUG_BLUE_OFF();

    // 0. Initialize status
    system_ready_status.ui_ready = false;
    system_ready_status.usart_ready = false;
    system_ready_status.modbus_ready = false;
    system_ready_status.a7670_ready = false;
    system_ready_status.bmp390_ready = false;
    system_ready_status.sht45_ready = false;
    system_ready_status.fram_ready = false;
    system_ready_status.datetime_ready = false;
    system_ready_status.rainfall_ok = false;
    system_ready_status.light_ok = false;
    system_ready_status.sd_detected = false;
    system_ready_status.sd_write_protected = false;

    // 1. UI module
    if ((system_ready_status.ui_ready = ui_init(&hi2c1)))
    {
        LED_DEBUG_RED_OFF();
        if (ui_led_set_value(0xF0))
        {
            LED_DEBUG_YELLOW_OFF();
            ui_lcd_printXY(0, 1, "Weather Station");
            ui_lcd_printXY(1, 0, "Stat : ");
        }
    }

    // 2. Weather database
    if ((system_ready_status.fram_ready = DB_Init(&hspi1)))
        ui_lcd_print("D");
    else
        ui_lcd_print("d");

    // 3. BMP390 and SHT41
    if ((system_ready_status.bmp390_ready = bmp390_init(&hi2c2)))
        ui_lcd_print("P");
    else
        ui_lcd_print("p");

    if ((system_ready_status.sht45_ready = sht45_init(&hi2c2)))
        ui_lcd_print("T");
    else
        ui_lcd_print("t");

    /* USER CODE END Init */

    /* USER CODE BEGIN RTOS_MUTEX */
    /* add mutexes, ... */
    /* USER CODE END RTOS_MUTEX */

    /* USER CODE BEGIN RTOS_SEMAPHORES */
    /* add semaphores, ... */
    /* USER CODE END RTOS_SEMAPHORES */

    /* USER CODE BEGIN RTOS_TIMERS */
    /* start timers, add new ones, ... */
    /* USER CODE END RTOS_TIMERS */

    /* USER CODE BEGIN RTOS_QUEUES */
    /* add queues, ... */
    /* USER CODE END RTOS_QUEUES */

    /* Create the thread(s) */
    /* creation of defaultTask */
    defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

    /* creation of UsbLoop */
    UsbLoopHandle = osThreadNew(UsbLoopTask, NULL, &UsbLoop_attributes);

    /* USER CODE BEGIN RTOS_THREADS */
    /* add threads, ... */
    UsbCDCHandle = osThreadNew(cdc_task, NULL, &UsbCDC_attributes);
    MainTaskHandle = osThreadNew(maintask, NULL, &MainTask_attributes);
    UITaskHandle = osThreadNew(uitask, NULL, &UITask_attributes);
    UserControlTaskHandle = osThreadNew(ucctask, NULL, &UserControlTask_attributes);
    SslUploadTaskHandle = osThreadNew(ssluploadtask, NULL, &SslUploadTask_attributes);

    /* USER CODE END RTOS_THREADS */

    /* USER CODE BEGIN RTOS_EVENTS */
    /* add events, ... */
    /* USER CODE END RTOS_EVENTS */
}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief  Function implementing the defaultTask thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
    /* USER CODE BEGIN StartDefaultTask */
    /* Infinite loop */
    for (;;)
    {
        osDelay(1);
    }
    /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_UsbLoopTask */
/**
 * @brief Function implementing the UsbLoop thread.
 * @param argument: Not used
 * @retval None
 */
/* USER CODE END Header_UsbLoopTask */
void UsbLoopTask(void *argument)
{
    /* USER CODE BEGIN UsbLoopTask */

    // init device stack on configured roothub port
    // This should be called after scheduler/kernel is started.
    // Otherwise it could cause kernel issue since USB IRQ handler does use RTOS queue API.
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    // board_init_after_tusb();

    /* Infinite loop */
    for (;;)
    {
        // put this thread to waiting state until there is new events
        tud_task();

        // following code only run if tud_task() process at least 1 event
        tud_cdc_write_flush();
    }
    /* USER CODE END UsbLoopTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */
