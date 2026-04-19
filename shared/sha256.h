/**
 * @file  sha256.h
 * @brief Standalone FIPS 180-4 SHA-256 — no external dependencies.
 *
 * ── RAM budget ─────────────────────────────────────────────────────────────
 *   sha256_ctx_t: 8×4 (state) + 8 (bit_count) + 64 (buf) + 4 (buf_len) = 108 B
 *
 * ── Usage ──────────────────────────────────────────────────────────────────
 *   sha256_ctx_t ctx;
 *   sha256_init(&ctx);
 *   sha256_update(&ctx, chunk, chunk_len);   // call repeatedly
 *   sha256_update(&ctx, more,  more_len);
 *   uint8_t digest[32];
 *   sha256_final(&ctx, digest);
 *
 * ── Thread safety ──────────────────────────────────────────────────────────
 *   Each sha256_ctx_t is independent.  Concurrent use of distinct contexts
 *   from different tasks is safe.  A single context must not be shared
 *   concurrently without external serialisation.
 *
 * This file is compiled into BOTH the application and the bootloader
 * (it lives in shared/ which both build environments include).
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 context — treat as opaque; do not access fields directly. */
typedef struct {
    uint32_t state[8];   /**< Running hash state (H0..H7).                */
    uint64_t bit_count;  /**< Total message bits processed.                */
    uint8_t  buf[64];    /**< Partial message block (< 64 bytes pending).  */
    uint8_t  buf_len;    /**< Valid bytes currently in buf (0..63).        */
} sha256_ctx_t;

/**
 * @brief Initialise a SHA-256 context to the standard H0 initial values.
 * @param ctx  Context to initialise; must not be NULL.
 */
void sha256_init(sha256_ctx_t *ctx);

/**
 * @brief Feed data into the running hash.
 *
 * May be called any number of times.  The total message length is not
 * limited within a uint64_t bit count (> 2 EB).
 *
 * @param ctx   Active context (after sha256_init and before sha256_final).
 * @param data  Input bytes; may be NULL only when len == 0.
 * @param len   Number of bytes to process.
 */
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);

/**
 * @brief Finalise the hash and write the 32-byte digest.
 *
 * Applies FIPS 180-4 padding, compresses the padded block(s), and writes
 * the big-endian result to @p digest.  The context is zeroed after this
 * call to prevent residual state leakage; it must not be reused without
 * a fresh sha256_init().
 *
 * @param ctx     Active context.
 * @param digest  Output buffer; must be exactly 32 bytes.
 */
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif /* SHA256_H */
