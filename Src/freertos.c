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
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "tusb.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CDC_STACK_SIZE      (configMINIMAL_STACK_SIZE * (CFG_TUSB_DEBUG ? 2 : 1))

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* Definitions for UsbLoop */
osThreadId_t UsbCDCHandle;
const osThreadAttr_t UsbCDC_attributes = {
  .name = "cdc",
  .stack_size = CDC_STACK_SIZE,
  .priority = (osPriority_t) osPriorityHigh1,
};

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for UsbLoop */
osThreadId_t UsbLoopHandle;
const osThreadAttr_t UsbLoop_attributes = {
  .name = "UsbLoop",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

void cdc_task(void *params);

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void UsbLoopTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

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
  for(;;)
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
    .speed = TUSB_SPEED_AUTO
  };
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  //board_init_after_tusb();

  /* Infinite loop */
  for(;;)
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

//--------------------------------------------------------------------+
// USB CDC
//--------------------------------------------------------------------+
void cdc_task(void *params)
{
  (void) params;

  // RTOS forever loop
  while (1)
  {
    // connected() check for DTR bit
    // Most but not all terminal client set this when making connection
    // if ( tud_cdc_connected() )
    {
      // There are data available
      while (tud_cdc_available())
      {
        uint8_t buf[64];

        // read and echo back
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        (void) count;

        // Echo back
        // Note: Skip echo by commenting out write() and write_flush()
        // for throughput test e.g
        //    $ dd if=/dev/zero of=/dev/ttyACM0 count=10000
        tud_cdc_write(buf, count);
      }

      tud_cdc_write_flush();

      // Press on-board button to send Uart status notification
      static uint32_t btn_prev = 0;
      static cdc_notify_uart_state_t uart_state = { .value = 0 };
      //const uint32_t btn = board_button_read();
      const uint32_t btn = 0; // Dummy
      if (!btn_prev && btn)
      {
        uart_state.dsr ^= 1;
        tud_cdc_notify_uart_state(&uart_state);
      }
      btn_prev = btn;
    }

    // For ESP32-Sx this delay is essential to allow idle how to run and reset watchdog
    vTaskDelay(1);
  }
}

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
  (void) itf;
  (void) rts;

  // TODO set some indicator
  if (dtr) {
    // Terminal connected
  } else {
    // Terminal disconnected
  }
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf) {
  (void) itf;
}

/* USER CODE END Application */

