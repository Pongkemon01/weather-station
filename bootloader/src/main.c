/**
 * @file    main.c
 * @brief   STM32L476RG bootloader entry point.
 *
 * Boot flow
 * ---------
 * 1. Initialise: HAL, GPIO (FRAM CS), SPI1, IWDG (if BOOTLOADER_WDT_ENABLE).
 * 2. Read OtaControlBlock from FRAM (primary then mirror fallback).
 * 3. If ota_pending == 1 and ota_tried < 3:
 *      a. Increment ota_tried, write OCB back (marks attempt before any risk).
 *      b. Validate staged image SHA-256 against ocb.image_sha256.
 *      c. If CRC valid: program application Flash pages 16–255 from staging.
 *         On success: clear ota_pending, leave ota_confirmed = 0 (app confirms).
 *         On failure: clear ota_pending (boot existing image).
 *      d. If CRC invalid: clear ota_pending (boot existing image).
 * 4. Validate application entry point (SP sanity check).
 * 5. Jump to 0x08008000.
 *
 * If ota_tried >= 3 or no pending update: jump directly to existing app.
 *
 * No FreeRTOS.  No dynamic allocation.  Single-threaded.
 */

#include "main.h"
#include "boot_fram.h"
#include "boot_flash.h"
#include "ota_control_block.h"
#include "fram_addresses.h"
#include "sha256.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>

/* ── Flash origin of the application ───────────────────────────────── */
#define APP_BASE 0x08008000ul

/* ── SRAM1 valid stack range (96 KB, 0x20000000–0x20017FFF) ─────────── */
#define SRAM1_BASE_ADDR 0x20000000ul
#define SRAM1_END_ADDR 0x20018000ul /* exclusive */

/* ── Peripheral handles ─────────────────────────────────────────────── */
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart2;
#ifdef BOOTLOADER_WDT_ENABLE
IWDG_HandleTypeDef hiwdg;
#endif

/* ── Static buffer (512 B) for image verification reads ────────────── */
static uint8_t verify_buf[512u];

/* ── Forward declarations ───────────────────────────────────────────── */
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_UART_Init(void);
#ifdef BOOTLOADER_WDT_ENABLE
static void MX_IWDG_Init(void);
#endif
static void boot_deinit(void);
static bool verify_image_sha256(uint32_t image_size, const uint8_t expected[32]);
//static void jump_to_application(uint32_t app_base);
static void jump_to_application(uint32_t start_address);

/* ── printf retargeting for newlib-nano (ARM GCC) ──────────────── */
#if defined(__GNUC__)
int _write(int file, char *ptr, int len)
{
    (void)file;
    HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, 0xFFFF);
    return len;
}
#else
int fputc(int ch, FILE *f)
{
    (void)f;
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}
#endif

/* ================================================================ */
/* SysTick handler — required by HAL for timeout tracking            */
/* ================================================================ */

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ================================================================ */
/* main()                                                            */
/* ================================================================ */

int main(void)
{
    OtaControlBlock_t ocb;
    OtaStatus_t st;
    bool program_ok = false;

    /* ---- HAL init (SysTick @ 1 ms, MSI 4 MHz default) ---- */
    HAL_Init();

    /* ---- Peripheral init ---- */
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_SPI1_Init();
#ifdef BOOTLOADER_WDT_ENABLE
    MX_IWDG_Init();
#endif

    printf("BL: bootloader started\r\n");

    /* ---- FRAM driver init ---- */
    if (!boot_fram_init(&hspi1))
    {
        printf("BL: FRAM init FAILED\r\n");
        /* FRAM unreachable — cannot read OCB; boot existing app. */
        jump_to_application(APP_BASE);
        Error_Handler();
    }
    printf("BL: FRAM init OK\r\n");

    /* ---- Read OTA control block ---- */
    st = ocb_read(&ocb);

    printf("BL: OCB read status=%d pending=%u tried=%u size=%lu\r\n",
           (int)st, (unsigned)ocb.ota_pending,
           (unsigned)ocb.ota_tried, (unsigned long)ocb.image_size);

    if (st == OTA_OK && ocb.ota_pending == OCB_PENDING_FLAG && ocb.ota_tried < OCB_MAX_TRIES)
    {
        /* Mark the attempt before touching Flash so power-loss is safe. */
        ocb.ota_tried++;
        ocb_write(&ocb); /* ignore write error — proceed anyway */

        printf("BL: OTA pending, attempt %u/%u, size=%lu\r\n",
               (unsigned)ocb.ota_tried, (unsigned)OCB_MAX_TRIES,
               (unsigned long)ocb.image_size);

        /* Validate staged image integrity via SHA-256.
         * Reject image_size == 0 or > 480 KB Flash partition before reading FRAM. */
        if (ocb.image_size > 0u && ocb.image_size <= FLASH_APP_SIZE_MAX && verify_image_sha256(ocb.image_size, ocb.image_sha256))
        {
            printf("BL: SHA-256 verified, programming Flash...\r\n");
            /* Program Flash. */
            program_ok = boot_flash_program(ocb.image_size
#ifdef BOOTLOADER_WDT_ENABLE
                                            ,
                                            &hiwdg
#endif
            );
            printf("BL: Flash program %s\r\n", program_ok ? "OK" : "FAILED");
        }
        else
        {
            printf("BL: SHA-256 verify FAILED (size=%lu)\r\n",
                   (unsigned long)ocb.image_size);
        }

        /* Clear pending flag regardless of outcome.
         * On success: app must call ota_confirm_success() within 60 s or
         *             IWDG timeout triggers rollback.
         * On failure: existing app boots; no further attempts this cycle. */
        ocb.ota_pending = 0u;
        ocb.ota_confirmed = (program_ok ? 0u : 0u); /* app confirms */
        ocb_write(&ocb);

        (void)program_ok; /* outcome logged via ota_confirmed state */
    }

    /* ---- Jump to application ---- */
    printf("BL: jumping to app (SP=0x%08lX, entry=0x%08lX)\r\n",
           (unsigned long)*(volatile uint32_t *)(APP_BASE + 0u),
           (unsigned long)*(volatile uint32_t *)(APP_BASE + 4u));
    jump_to_application(APP_BASE);

    /* Never reached — jump_to_application only returns on SP check fail. */
    Error_Handler();
}

/* ================================================================ */
/* Private functions                                                 */
/* ================================================================ */

/**
 * @brief  Compute SHA-256 over the staged image in FRAM and compare to expected.
 *
 * Reads in 512-byte blocks to bound stack and static RAM usage.
 * IWDG refreshed every block when BOOTLOADER_WDT_ENABLE.
 *
 * @param  image_size  Byte count of the staged image.
 * @param  expected    32-byte expected digest from OtaControlBlock_t.image_sha256.
 * @return true if computed digest matches @p expected; false otherwise.
 */
static bool verify_image_sha256(uint32_t image_size, const uint8_t expected[32])
{
    sha256_ctx_t ctx;
    uint8_t digest[32];
    uint32_t offset = 0u;

    sha256_init(&ctx);

    while (offset < image_size)
    {
        uint32_t chunk = image_size - offset;
        if (chunk > sizeof(verify_buf))
            chunk = sizeof(verify_buf);

        if (boot_fram_read(FRAM_STAGING_IMAGE + offset, verify_buf, chunk) != chunk)
            return false;

        sha256_update(&ctx, verify_buf, chunk);
#ifdef BOOTLOADER_WDT_ENABLE
        HAL_IWDG_Refresh(&hiwdg);
#endif
        offset += chunk;
    }

    sha256_final(&ctx, digest);
    return (memcmp(digest, expected, 32u) == 0);
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Validate stack pointer then jump to application.
 *
 * Reads the initial SP from APP_BASE+0 and the reset vector from
 * APP_BASE+4.  If the SP does not fall within SRAM1, the function
 * returns without jumping (caller should call Error_Handler).
 */
static void jump_to_application(uint32_t start_address)
{
    // 1. Check if application space contains a valid Stack Pointer address
    if (((*(__IO uint32_t *)start_address) & 0x2FFE0000) == 0x20000000)
    {

        // 2. Get the Application Reset Handler address
        uint32_t jump_address = *(__IO uint32_t *)(start_address + 4);
        void (*p_app)(void) = (void (*)(void))jump_address;

        // // 2.1 De-init peripherals
        // HAL_SPI_MspDeInit(&hspi1);
        // HAL_UART_MspDeInit(&huart2);

        // // 3. De-initialize peripherals and pointers
        boot_deinit();
        SysTick->CTRL = 0;

        // 4. Set the Main Stack Pointer
        __set_MSP(*(__IO uint32_t *)start_address);

        // 5. Execution jump
        p_app();
    }
    else
    {
        printf("BL: Stack check error!!\r\n");
        return;
    }
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  De-initialise peripherals and reset system clock before jumping
 *         to the application.
 *
 * The bootloader leaves SPI1, USART2, and GPIOs configured.  If we jump
 * without clean-up, the application's HAL init may fault on leftover
 * peripheral state.  This function:
 *   1. De-inits UART, SPI, and GPIO (analog mode = lowest power / safest).
 *   2. Resets the RCC so the application's SystemInit() starts clean.
 *   3. De-inits the HAL (stops SysTick).
 */
static void boot_deinit(void)
{
    /* De-init peripherals that were initialised by the bootloader. */
    HAL_UART_DeInit(&huart2);
    HAL_SPI_DeInit(&hspi1);

    /* Reset all GPIO pins used by the bootloader to analog input.
     * This covers PA2–PA7 (USART2 + SPI1) and PC0–PC3 (LEDs). */
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0u;

    gpio.Pin = FRAM_CS_Pin | FRAM_SCK_Pin | FRAM_MSIO_Pin | FRAM_MOSI_Pin | CONSOLE_TX_Pin | CONSOLE_RX_Pin;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = LED_RED_Pin | LED_YELLOW_Pin | LED_GREEN_Pin | LED_BLUE_Pin;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* Disable only the peripheral clocks the bootloader enabled.
     * Do NOT call HAL_RCC_DeInit() — that resets the entire RCC including
     * the system clock source, which leaves the application's GPIO access
     * (e.g. LED_DEBUG_YELLOW_ON at the top of main()) causing a bus fault
     * because the peripheral clock is off. */
    __HAL_RCC_SPI1_CLK_DISABLE();
    __HAL_RCC_USART2_CLK_DISABLE();
    __HAL_RCC_GPIOA_CLK_DISABLE();
    __HAL_RCC_GPIOC_CLK_DISABLE();
    HAL_RCC_DeInit();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    /* De-init HAL — stops SysTick, resets HAL internals. */
    HAL_DeInit();
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise GPIO pins used by the bootloader.
 *
 * PA4  — FRAM CS (Output Push-Pull, initially High / deasserted)
 * PA5  — SPI1_SCK  (AF5)
 * PA6  — SPI1_MISO (AF5)
 * PA7  — SPI1_MOSI (AF5)
 * PC0  — LED_RED   (Output Push-Pull, initially Low)
 * PC1  — LED_YELLOW (Output Push-Pull, initially Low)
 * PC2  — LED_GREEN (Output Push-Pull, initially Low; boot status LED)
 * PC3  — LED_BLUE  (Output Push-Pull, initially Low)
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PA4 — FRAM CS: output, initially High (deasserted). */
    HAL_GPIO_WritePin(FRAM_CS_GPIO_Port, FRAM_CS_Pin, GPIO_PIN_SET);
    gpio.Pin = FRAM_CS_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(FRAM_CS_GPIO_Port, &gpio);

    /* PA5/PA6/PA7 — SPI1 SCK/MISO/MOSI (AF5). */
    gpio.Pin = FRAM_SCK_Pin | FRAM_MSIO_Pin | FRAM_MOSI_Pin;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PA2/PA3 — USART2 TX/RX (AF7), debug console. */
    gpio.Pin = CONSOLE_TX_Pin | CONSOLE_RX_Pin;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PC0–PC3 — four debug LEDs: Red, Yellow, Green, Blue. All off initially. */
    HAL_GPIO_WritePin(GPIOC, LED_RED_Pin | LED_YELLOW_Pin | LED_GREEN_Pin | LED_BLUE_Pin,
                      GPIO_PIN_RESET);
    gpio.Pin = LED_RED_Pin | LED_YELLOW_Pin | LED_GREEN_Pin | LED_BLUE_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0u;
    HAL_GPIO_Init(GPIOC, &gpio);
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise SPI1 in master, Mode 0, 8-bit, polling mode.
 *
 * SPI clock: MSI 4 MHz / APB2 4 MHz / prescaler 4 = 1 MHz.
 * The CY15B116QN supports up to 40 MHz — 1 MHz is well within spec.
 */
static void MX_SPI1_Init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7u;
    hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;

    if (HAL_SPI_Init(&hspi1) != HAL_OK)
        Error_Handler();
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise USART2 for debug console — 115200 8N1, polling TX.
 *
 * Clock source: PCLK1 (MSI 4 MHz → APB1 4 MHz).  No DMA, no interrupts —
 * bootloader uses blocking HAL_UART_Transmit only.
 */
static void MX_USART2_UART_Init(void)
{
    __HAL_RCC_USART2_CLK_ENABLE();

    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&huart2) != HAL_OK)
        Error_Handler();
}

/* ──────────────────────────────────────────────────────────────── */

#ifdef BOOTLOADER_WDT_ENABLE
/**
 * @brief  Initialise the IWDG with a ~4 s timeout.
 *
 * LSI ≈ 32 000 Hz, prescaler 64 → 500 Hz tick.
 * Reload 2000 → 2000 / 500 = 4.0 s timeout.
 */
static void MX_IWDG_Init(void)
{
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
    hiwdg.Init.Window = IWDG_WINDOW_DISABLE;
    hiwdg.Init.Reload = 2000u;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
        Error_Handler();
}
#endif /* BOOTLOADER_WDT_ENABLE */

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Fatal error handler — blinks LED and loops forever.
 */
void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
        HAL_GPIO_TogglePin(BOOT_LED_GPIO_Port, BOOT_LED_Pin);
        HAL_Delay(200u);
    }
}
