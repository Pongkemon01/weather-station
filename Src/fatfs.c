/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file   fatfs.c
  * @brief  Code for fatfs applications
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
#include "rtc.h"
/* USER CODE END Header */
#include "fatfs.h"

uint8_t retSD;    /* Return value for SD */
char SDPath[4];   /* SD logical drive path */
FATFS SDFatFS;    /* File system object for SD logical drive */
FIL SDFile;       /* File object for SD */

/* USER CODE BEGIN VolToPart */
/* Volume - Partition resolution table should be user defined in case of Multiple partition */
/* When multi-partition feature is enabled (1), each logical drive number is bound to arbitrary physical drive and partition
listed in the VolToPart[] */
#if _MULTI_PARTITION
PARTITION *VolToPart  = {
  {0, 1},
  {0, 2},
  {0, 3},
  {0, 4}
};
#endif
/* USER CODE END VolToPart */

/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

void MX_FATFS_Init(void)
{
  /*## FatFS: Link the SD driver ###########################*/
  retSD = FATFS_LinkDriver(&SD_Driver, SDPath);

  /* USER CODE BEGIN Init */
  /* additional user code for init */
  /* USER CODE END Init */
}

/**
  * @brief  Gets Time from RTC
  * @param  None
  * @retval Time in DWORD
  */
DWORD get_fattime(void)
{
  /* USER CODE BEGIN get_fattime */
    RTC_TimeTypeDef sTime;
    RTC_DateTypeDef sDate;

    // Call HAL_RTC_GetDate() after HAL_RTC_GetTime() to unlock shadow registers
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

    // Pack the date and time as required by FatFs
    DWORD fat_date = (((DWORD)(sDate.Year + 2000 - 1980)) << 25) |
                     (((DWORD)sDate.Month) << 21) |
                     (((DWORD)sDate.Date) << 16);

    DWORD fat_time = (((DWORD)sTime.Hours) << 11) |
                     (((DWORD)sTime.Minutes) << 5) |
                     (((DWORD)sTime.Seconds) >> 1);

    return (fat_date | fat_time);
  /* USER CODE END get_fattime */
}

/* USER CODE BEGIN Application */

/* USER CODE END Application */
