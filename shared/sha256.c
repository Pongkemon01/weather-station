/**
 * @file  sha256.c
 * @brief Standalone FIPS 180-4 SHA-256 implementation.
 *
 * No external dependencies beyond stdint.h and string.h.
 * Suitable for bare-metal / FreeRTOS embedded targets.
 *
 * .rodata cost: K[64] = 256 B round constants.
 * Stack per compress() call: ~80 B (W[16] window + 8 working vars).
 */

#include "sha256.h"
#include <string.h>

/* ─────────────────────────── SHA-256 constants ──────────────────────────── */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* ─────────────────────────── Bit-manipulation macros ────────────────────── */

#define ROTR(x, n)   (((x) >> (n)) | ((x) << (32u - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define SIG0(x)  (ROTR((x),  2u) ^ ROTR((x), 13u) ^ ROTR((x), 22u))
#define SIG1(x)  (ROTR((x),  6u) ^ ROTR((x), 11u) ^ ROTR((x), 25u))
#define sig0(x)  (ROTR((x),  7u) ^ ROTR((x), 18u) ^ ((x) >>  3u))
#define sig1(x)  (ROTR((x), 17u) ^ ROTR((x), 19u) ^ ((x) >> 10u))

/* ─────────────────────────── Private: compress one 64-byte block ─────────── */

/*
 * compress — run the SHA-256 compression function on one 512-bit block.
 *
 * @param state  8-word running hash (modified in place).
 * @param block  64 bytes of message data (big-endian 32-bit words).
 *
 * W[] uses a 16-word sliding window instead of the full 64-word schedule
 * to avoid a 256-byte stack frame on each call.
 */
static void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t W[16];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t T1, T2;
    uint8_t  i;

    /* Load first 16 message words (big-endian) */
    for (i = 0u; i < 16u; i++)
    {
        const uint8_t *p = block + i * 4u;
        W[i] = ((uint32_t)p[0] << 24u)
             | ((uint32_t)p[1] << 16u)
             | ((uint32_t)p[2] <<  8u)
             |  (uint32_t)p[3];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0u; i < 64u; i++)
    {
        uint32_t w;

        if (i < 16u)
        {
            w = W[i];
        }
        else
        {
            /* Expand schedule using 16-word ring buffer */
            uint8_t j = i & 0x0Fu;
            W[j] = sig1(W[(j + 14u) & 0x0Fu])
                 + W[(j +  9u) & 0x0Fu]
                 + sig0(W[(j +  1u) & 0x0Fu])
                 + W[j];
            w = W[j];
        }

        T1 = h + SIG1(e) + CH(e, f, g) + K[i] + w;
        T2 = SIG0(a) + MAJ(a, b, c);

        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* ══════════════════════════ Public API ══════════════════════════════════════*/

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bit_count = 0u;
    ctx->buf_len   = 0u;
}

/* -------------------------------------------------------------------------- */

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    if (len == 0u)
        return;

    ctx->bit_count += (uint64_t)len * 8u;

    /* Fill and drain the partial block buffer first */
    if (ctx->buf_len > 0u)
    {
        uint8_t space = (uint8_t)(64u - ctx->buf_len);
        uint8_t take  = (len < (uint32_t)space) ? (uint8_t)len : space;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len = (uint8_t)(ctx->buf_len + take);
        data += take;
        len  -= (uint32_t)take;

        if (ctx->buf_len == 64u)
        {
            compress(ctx->state, ctx->buf);
            ctx->buf_len = 0u;
        }
    }

    /* Process whole blocks directly from the caller's buffer */
    while (len >= 64u)
    {
        compress(ctx->state, data);
        data += 64u;
        len  -= 64u;
    }

    /* Buffer any remaining partial block */
    if (len > 0u)
    {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = (uint8_t)len;
    }
}

/* -------------------------------------------------------------------------- */

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32])
{
    uint8_t  i;
    uint64_t bit_count = ctx->bit_count;

    /* Append 0x80 padding byte */
    ctx->buf[ctx->buf_len++] = 0x80u;

    /* If not enough room for the 8-byte length field, compress and start over */
    if (ctx->buf_len > 56u)
    {
        memset(ctx->buf + ctx->buf_len, 0, (size_t)(64u - ctx->buf_len));
        compress(ctx->state, ctx->buf);
        ctx->buf_len = 0u;
    }

    /* Zero-pad to byte 56 */
    memset(ctx->buf + ctx->buf_len, 0, (size_t)(56u - ctx->buf_len));

    /* Append big-endian 64-bit message length in bits */
    ctx->buf[56] = (uint8_t)(bit_count >> 56u);
    ctx->buf[57] = (uint8_t)(bit_count >> 48u);
    ctx->buf[58] = (uint8_t)(bit_count >> 40u);
    ctx->buf[59] = (uint8_t)(bit_count >> 32u);
    ctx->buf[60] = (uint8_t)(bit_count >> 24u);
    ctx->buf[61] = (uint8_t)(bit_count >> 16u);
    ctx->buf[62] = (uint8_t)(bit_count >>  8u);
    ctx->buf[63] = (uint8_t)(bit_count);

    compress(ctx->state, ctx->buf);

    /* Serialise state as big-endian bytes */
    for (i = 0u; i < 8u; i++)
    {
        digest[i * 4u + 0u] = (uint8_t)(ctx->state[i] >> 24u);
        digest[i * 4u + 1u] = (uint8_t)(ctx->state[i] >> 16u);
        digest[i * 4u + 2u] = (uint8_t)(ctx->state[i] >>  8u);
        digest[i * 4u + 3u] = (uint8_t)(ctx->state[i]);
    }

    /* Zero the context to prevent residual state leakage */
    memset(ctx, 0, sizeof(*ctx));
}
