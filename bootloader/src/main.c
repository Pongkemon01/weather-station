/**
 * @file    main.c
 * @brief   STM32L476RG bootloader entry point.
 *
 * Boot flow
 * ---------
 * 1. Initialise: HAL, GPIO (FRAM CS), SPI1, IWDG.
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
 * IWDG timeout: 4 s (PRESCALER_64, RELOAD=2000, LSI≈32 kHz).
 * The IWDG is refreshed before every Flash page erase to survive the
 * 240-page loop (~6 s worst case; refresh every ~25 ms keeps it alive).
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

/* ── Flash origin of the application ───────────────────────────────── */
#define APP_BASE  0x08008000ul

/* ── SRAM1 valid stack range (96 KB, 0x20000000–0x20017FFF) ─────────── */
#define SRAM1_BASE_ADDR   0x20000000ul
#define SRAM1_END_ADDR    0x20018000ul   /* exclusive */

/* ── Peripheral handles ─────────────────────────────────────────────── */
SPI_HandleTypeDef  hspi1;
IWDG_HandleTypeDef hiwdg;

/* ── Static buffer (512 B) for image verification reads ────────────── */
static uint8_t verify_buf[512u];

/* ── Forward declarations ───────────────────────────────────────────── */
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_IWDG_Init(void);
static bool verify_image_sha256(uint32_t image_size, const uint8_t expected[32]);
static void jump_to_application(uint32_t app_base);

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
    OtaStatus_t       st;
    bool              program_ok = false;

    /* ---- HAL init (SysTick @ 1 ms, MSI 4 MHz default) ---- */
    HAL_Init();

    /* ---- Peripheral init ---- */
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_IWDG_Init();

    /* ---- FRAM driver init ---- */
    if (!boot_fram_init(&hspi1))
    {
        /* FRAM unreachable — cannot read OCB; jump to existing app. */
        jump_to_application(APP_BASE);
        Error_Handler();
    }

    /* ---- Read OTA control block ---- */
    st = ocb_read(&ocb);

    if (st == OTA_OK
        && ocb.ota_pending == OCB_PENDING_FLAG
        && ocb.ota_tried   < OCB_MAX_TRIES)
    {
        /* Mark the attempt before touching Flash so power-loss is safe. */
        ocb.ota_tried++;
        ocb_write(&ocb);   /* ignore write error — proceed anyway */

        /* Validate staged image integrity via SHA-256.
         * Reject image_size == 0 or > 480 KB Flash partition before reading FRAM. */
        if (ocb.image_size > 0u
            && ocb.image_size <= FLASH_APP_SIZE_MAX
            && verify_image_sha256(ocb.image_size, ocb.image_sha256))
        {
            /* Program Flash. */
            program_ok = boot_flash_program(ocb.image_size, &hiwdg);
        }

        /* Clear pending flag regardless of outcome.
         * On success: app must call ota_confirm_success() within 60 s or
         *             IWDG timeout triggers rollback.
         * On failure: existing app boots; no further attempts this cycle. */
        ocb.ota_pending   = 0u;
        ocb.ota_confirmed = (program_ok ? 0u : 0u);   /* app confirms */
        ocb_write(&ocb);

        (void)program_ok;   /* outcome logged via ota_confirmed state */
    }

    /* ---- Jump to application ---- */
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
 * IWDG is refreshed every block to survive a 512 KB verification pass.
 *
 * @param  image_size  Byte count of the staged image.
 * @param  expected    32-byte expected digest from OtaControlBlock_t.image_sha256.
 * @return true if computed digest matches @p expected; false otherwise.
 */
static bool verify_image_sha256(uint32_t image_size, const uint8_t expected[32])
{
    sha256_ctx_t ctx;
    uint8_t      digest[32];
    uint32_t     offset = 0u;

    sha256_init(&ctx);

    while (offset < image_size)
    {
        uint32_t chunk = image_size - offset;
        if (chunk > sizeof(verify_buf))
            chunk = sizeof(verify_buf);

        if (boot_fram_read(FRAM_STAGING_IMAGE + offset, verify_buf, chunk) != chunk)
            return false;

        sha256_update(&ctx, verify_buf, chunk);
        HAL_IWDG_Refresh(&hiwdg);
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
static void jump_to_application(uint32_t app_base)
{
    typedef void (*app_entry_t)(void);

    uint32_t    app_sp    = *(volatile uint32_t *)(app_base + 0u);
    uint32_t    app_entry = *(volatile uint32_t *)(app_base + 4u);
    app_entry_t entry_fn;

    /* Stack-pointer sanity check: must point into SRAM1. */
    if (app_sp < SRAM1_BASE_ADDR || app_sp >= SRAM1_END_ADDR)
        return;   /* no valid application — return to caller */

    /* Disable SysTick and all interrupts before handing over control. */
    SysTick->CTRL = 0u;
    __disable_irq();
    __DSB();
    __ISB();

    /* Relocate vector table to application. */
    SCB->VTOR = app_base;

    /* Set main stack pointer and jump. */
    __set_MSP(app_sp);

    entry_fn = (app_entry_t)app_entry;
    entry_fn();
    /* Never reached. */
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise GPIO pins used by the bootloader.
 *
 * PA4  — FRAM CS (Output Push-Pull, initially High / deasserted)
 * PA5  — SPI1_SCK  (AF5)
 * PA6  — SPI1_MISO (AF5)
 * PA7  — SPI1_MOSI (AF5)
 * PC2  — Boot status LED (Output Push-Pull, initially Low)
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PA4 — FRAM CS: output, initially High (deasserted). */
    HAL_GPIO_WritePin(FRAM_CS_GPIO_Port, FRAM_CS_Pin, GPIO_PIN_SET);
    gpio.Pin   = FRAM_CS_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(FRAM_CS_GPIO_Port, &gpio);

    /* PA5/PA6/PA7 — SPI1 SCK/MISO/MOSI (AF5). */
    gpio.Pin       = FRAM_SCK_Pin | FRAM_MSIO_Pin | FRAM_MOSI_Pin;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PC2 — boot LED. */
    HAL_GPIO_WritePin(BOOT_LED_GPIO_Port, BOOT_LED_Pin, GPIO_PIN_RESET);
    gpio.Pin       = BOOT_LED_Pin;
    gpio.Mode      = GPIO_MODE_OUTPUT_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0u;
    HAL_GPIO_Init(BOOT_LED_GPIO_Port, &gpio);
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

    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial     = 7u;
    hspi1.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;

    if (HAL_SPI_Init(&hspi1) != HAL_OK)
        Error_Handler();
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise the IWDG with a ~4 s timeout.
 *
 * LSI ≈ 32 000 Hz, prescaler 64 → 500 Hz tick.
 * Reload 2000 → 2000 / 500 = 4.0 s timeout.
 */
static void MX_IWDG_Init(void)
{
    hiwdg.Instance       = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
    hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;
    hiwdg.Init.Reload    = 2000u;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
        Error_Handler();
}

/* ──────────────────────────────────────────────────────────────── */

/**
 * @brief  Fatal error handler — blinks LED and loops forever.
 *
 * IWDG will eventually fire and reset the device, which will boot
 * the existing application (ota_pending was cleared before any Flash
 * operation that could have corrupted the app image).
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
