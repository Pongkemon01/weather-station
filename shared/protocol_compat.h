/* protocol_compat.h - compiler portability shims for shared protocol headers.
 *
 * These macros let the same headers compile under:
 *   - arm-none-eabi-gcc        (STM32 firmware, the source of truth)
 *   - MinGW-w64 GCC            (Windows host)
 *   - System GCC               (Linux host)
 *   - Apple Clang              (macOS host)
 *   - MSVC                     (not currently used, but supported)
 *
 * Rules for headers in shared/:
 *   1. Plain C only, no C++ features. Wrap with extern "C" guards for C++
 *      consumers (see protocol.h).
 *   2. <stdint.h> / <stdbool.h> types only. No platform-specific includes,
 *      no HAL, no Qt, no FreeRTOS.
 *   3. Wire structs use the PACKED macro (GCC/Clang __attribute__((packed))
 *      semantics).  MSVC users get an equivalent via #pragma pack push/pop.
 *   4. Every wire struct MUST be guarded with STATIC_ASSERT on its sizeof —
 *      this is the early-warning system for host/device protocol drift.
 *
 * Endianness: all multi-byte fields on the wire are little-endian, which
 * matches the STM32 native byte order. No byte-swapping is performed on the
 * firmware side, so the host must also be little-endian (Windows/Linux/macOS
 * on x86_64 and ARM64 desktop are all LE — confirmed for every supported
 * target).
 */

#ifndef ROBIN_PROTOCOL_COMPAT_H
#define ROBIN_PROTOCOL_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Packed struct attributes -------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
    #define PACKED_STRUCT_BEGIN
    #define PACKED_STRUCT_END
    #define PACKED __attribute__((packed))
#elif defined(_MSC_VER)
    #define PACKED_STRUCT_BEGIN __pragma(pack(push, 1))
    #define PACKED_STRUCT_END   __pragma(pack(pop))
    #define PACKED
#else
    #error "Unsupported compiler - add packed-struct shims here."
#endif

/* ---- Static assertion ---------------------------------------------------- */
#if defined(__cplusplus)
    #define STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
    /* _Static_assert is C11; STM32 firmware already uses it. */
    #define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* ---- Little-endian sanity check ------------------------------------------ */
/* Catch any future port to a big-endian platform at compile time. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
    #if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
        #error "Robin Weather Station protocol is little-endian only."
    #endif
#endif

#endif /* ROBIN_PROTOCOL_COMPAT_H */
