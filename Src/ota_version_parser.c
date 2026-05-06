/**
 * @file  ota_version_parser.c
 * @brief Parser for OTA version-check server response tokens.
 */

#include "ota_version_parser.h"

#include <stdlib.h>

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10u);
    return 0xFFu;
}

bool ovp_parse(const uint8_t *buf, uint16_t len,
               uint32_t *version, uint32_t *img_size,
               uint8_t sha256_out[32], uint32_t *wait_seconds)
{
    const char *p   = (const char *)buf;
    const char *end = p + len;

    for (; p + 3 < end; p++) {
        if (p[0] != 'V' || p[1] != '.') continue;

        char *q;
        unsigned long v = strtoul(p + 2, &q, 10);
        if (q == p + 2 || q + 3 > end ||
            q[0] != ':' || q[1] != 'L' || q[2] != '.') continue;

        unsigned long l = strtoul(q + 3, &q, 10);
        if (q + 3 > end ||
            q[0] != ':' || q[1] != 'H' || q[2] != '.') continue;

        const char *h = q + 3;
        if (h + 64 > end) return false;

        bool hex_ok = true;
        for (uint8_t i = 0u; i < 32u; i++) {
            uint8_t hi = hex_nibble(h[2u * i]);
            uint8_t lo = hex_nibble(h[2u * i + 1u]);
            if (hi > 15u || lo > 15u) { hex_ok = false; break; }
            sha256_out[i] = (uint8_t)((hi << 4u) | lo);
        }
        if (!hex_ok) continue;

        /* Optional :W.<seconds> suffix — absent means W.0 (download now). */
        unsigned long w = 0u;
        const char *after_hash = h + 64;
        if (after_hash + 3 <= end &&
            after_hash[0] == ':' && after_hash[1] == 'W' && after_hash[2] == '.') {
            char *wend;
            unsigned long parsed = strtoul(after_hash + 3, &wend, 10);
            if (wend != after_hash + 3) {
                w = parsed;
            }
        }

        *version      = (uint32_t)v;
        *img_size     = (uint32_t)l;
        *wait_seconds = (uint32_t)w;
        return true;
    }
    return false;
}
