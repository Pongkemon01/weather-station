/**
 * @file  test_main.c
 * @brief P3.2-5: Unit tests for OTA image size guard.
 *
 * Verifies that ovp_parse() correctly extracts img_size from server responses
 * and that the FLASH_APP_SIZE_MAX check (480 KB) accepts/rejects as required.
 *
 * Mirrors the decision logic in ota_manager_task.c POLLING_VERSION state:
 *   ovp_parse() && img_size > 0 && img_size <= FLASH_APP_SIZE_MAX → accept
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "unity.h"
#include "ota_version_parser.h"
#include "fram_addresses.h"

/* ── Helpers ── */

#define SHA_ZEROS \
    "0000000000000000000000000000000000000000000000000000000000000000"

/** Reproduces the accept/reject decision from POLLING_VERSION state. */
static bool check_size(const char *response_str)
{
    const uint8_t *buf = (const uint8_t *)response_str;
    uint16_t       len = (uint16_t)strlen(response_str);
    uint32_t version, img_size, wait_secs;
    uint8_t  sha[32];

    if (!ovp_parse(buf, len, &version, &img_size, sha, &wait_secs))
        return false;
    return (img_size > 0u && img_size <= FLASH_APP_SIZE_MAX);
}

/* ── Test cases ── */

/* 512 KB = 524288 bytes > FLASH_APP_SIZE_MAX (480 KB) → must reject */
void test_oversize_512KB_rejected(void)
{
    TEST_ASSERT_FALSE(check_size("V.2:L.524288:H." SHA_ZEROS));
}

/* 480 KB = 491520 bytes == FLASH_APP_SIZE_MAX → must accept (at-limit) */
void test_exactly_480KB_accepted(void)
{
    TEST_ASSERT_TRUE(check_size("V.2:L.491520:H." SHA_ZEROS));
}

/* One byte over limit → reject */
void test_one_byte_over_limit_rejected(void)
{
    TEST_ASSERT_FALSE(check_size("V.2:L.491521:H." SHA_ZEROS));
}

/* Typical valid image (200 KB) → accept */
void test_typical_image_accepted(void)
{
    TEST_ASSERT_TRUE(check_size("V.2:L.204800:H." SHA_ZEROS));
}

/* image_size = 0 → reject (guard: img_size > 0) */
void test_zero_size_rejected(void)
{
    TEST_ASSERT_FALSE(check_size("V.2:L.0:H." SHA_ZEROS));
}

/* Corrupt / missing token → parse failure → reject */
void test_malformed_response_rejected(void)
{
    TEST_ASSERT_FALSE(check_size("no version token here"));
}

/* ── Unity boilerplate ── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_oversize_512KB_rejected);
    RUN_TEST(test_exactly_480KB_accepted);
    RUN_TEST(test_one_byte_over_limit_rejected);
    RUN_TEST(test_typical_image_accepted);
    RUN_TEST(test_zero_size_rejected);
    RUN_TEST(test_malformed_response_rejected);
    return UNITY_END();
}
