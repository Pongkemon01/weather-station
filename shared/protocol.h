/* protocol.h - Robin Weather Station wire protocol (host <-> STM32 over USB-CDC).
 *
 * This header is the SINGLE SOURCE OF TRUTH for the wire format. It is
 * shared verbatim by:
 *   - firmware/Core/Inc/protocol.h (link or include from this file)
 *   - host/src/   (via -I../shared on the host build)
 *
 * Authoritative reference: firmware/Core/Src/cdctask.c
 *
 * ============================================================================
 * FRAME FORMAT
 * ============================================================================
 *
 *   +---------+---------+---------+----------------------------+---------+---------+
 *   | MAGIC_H | MAGIC_L |   CMD   |         PAYLOAD            | FOOT_H  | FOOT_L  |
 *   |  1 byte |  1 byte |  1 byte |     0..216 bytes           |  1 byte |  1 byte |
 *   +---------+---------+---------+----------------------------+---------+---------+
 *
 *   Host -> Device:   MAGIC = 0xDC 0xB1   FOOTER = 0x23 0x4E   (~MAGIC byte-wise)
 *   Device -> Host:   MAGIC = 0x55 0xAA   FOOTER = 0xAA 0x55   (~MAGIC byte-wise)
 *
 *   - No length field on the wire. Payload length is determined by CMD on
 *     the receiving side (see ROBIN_OP_* below).
 *   - No CRC. Integrity relies on USB CDC's own CRC and the magic/footer
 *     frame bracketing.
 *   - All multi-byte numeric fields are little-endian.
 *   - The 64-byte CDC packet MPS is a USB transport detail; logical frames
 *     up to 216 bytes are spread across multiple USB packets by TinyUSB.
 *
 * ============================================================================
 * COMMAND TABLE
 * ============================================================================
 *
 *   Code   Name             H->D Payload         D->H Response
 *   ----   ----------       ------------------   --------------------------
 *   0x01   REQ_WEATHER      (empty)              Weather_Data_Packed_t (18B)
 *   0x02   REQ_META         (empty)              Meta_Data_t           (216B)
 *   0x03   SET_META         Meta_Data_t (216B)   ACK echoing 0x03  or  NAK + err
 *   0x04   SET_RTC          RTC_DateTime_t (6B)  ACK echoing 0x04  or  NAK + err
 *   0x05   REQ_STATUS       (empty)              System_Ready_Status_t (12B)
 *   0x06   DB_FLUSH         (empty)              ACK echoing 0x06  or  NAK + err
 *   0x07   SYS_RESET        (empty)              ACK echoing 0x07, then link drops
 *   0x08   REQ_RTC          (empty)              RTC_DateTime_t (6B)
 *
 *   0xFE   ACK              (device-originated)  1-byte echoed command code
 *   0xFF   NAK              (device-originated)  1-byte error code (ROBIN_ERR_*)
 *
 *   Any byte > 0x08 in the CMD position (other than ACK/NAK which the host
 *   would never send anyway) is rejected by the device with NAK/UNKNOWN_CMD.
 *
 * ============================================================================
 * ERROR CODES (NAK payload)
 * ============================================================================
 *
 *   0x01   UNKNOWN_CMD       CMD byte was not a recognized opcode
 *   0x02   INVALID_FOOTER    One or both footer bytes did not match ~MAGIC
 *   0x03   WRITE_FAILED      Device-side persistence failed (FRAM, RTC, etc.)
 *   0x04   INVALID_DATA      Payload contents were rejected (e.g. RTC out of range)
 *
 * ============================================================================
 */

#ifndef ROBIN_PROTOCOL_H
#define ROBIN_PROTOCOL_H

#include "protocol_compat.h"
#include "protocol_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Framing constants                                                          */
/* ========================================================================== */

/* Host -> Device magic.
 *
 * The footer bytes are bit-inverted magic bytes. We compute them at
 * compile time via the ~ operator on a uint8_t-promoted-to-int expression,
 * then cast back to uint8_t. The cast uses static_cast under C++ and a
 * C cast under C so this header is friendly to both -Wold-style-cast and C. */
#if defined(__cplusplus)
    #define ROBIN__U8(x) static_cast<uint8_t>(x)
#else
    #define ROBIN__U8(x) ((uint8_t)(x))
#endif

#define ROBIN_MAGIC_H2D_H        0xDCu
#define ROBIN_MAGIC_H2D_L        0xB1u
#define ROBIN_FOOTER_H2D_H       ROBIN__U8(~ROBIN_MAGIC_H2D_H)   /* 0x23 */
#define ROBIN_FOOTER_H2D_L       ROBIN__U8(~ROBIN_MAGIC_H2D_L)   /* 0x4E */

/* Device -> Host magic */
#define ROBIN_MAGIC_D2H_H        0x55u
#define ROBIN_MAGIC_D2H_L        0xAAu
#define ROBIN_FOOTER_D2H_H       ROBIN__U8(~ROBIN_MAGIC_D2H_H)   /* 0xAA */
#define ROBIN_FOOTER_D2H_L       ROBIN__U8(~ROBIN_MAGIC_D2H_L)   /* 0x55 */

/* Largest payload any command can carry. Used to size receive buffers. */
#define ROBIN_MAX_PAYLOAD        216u

/* Largest complete frame on the wire: 2 magic + 1 cmd + 216 payload + 2 footer */
#define ROBIN_MAX_FRAME_BYTES    (2u + 1u + ROBIN_MAX_PAYLOAD + 2u)   /* 221 */

/* ========================================================================== */
/* Opcodes                                                                    */
/* ========================================================================== */

typedef enum {
    ROBIN_OP_REQ_WEATHER  = 0x01u,  /* H->D: empty;          D->H: Weather_Data_Packed_t */
    ROBIN_OP_REQ_META     = 0x02u,  /* H->D: empty;          D->H: Meta_Data_t           */
    ROBIN_OP_SET_META     = 0x03u,  /* H->D: Meta_Data_t;    D->H: ACK / NAK             */
    ROBIN_OP_SET_RTC      = 0x04u,  /* H->D: RTC_DateTime_t; D->H: ACK / NAK             */
    ROBIN_OP_REQ_STATUS   = 0x05u,  /* H->D: empty;          D->H: System_Ready_Status_t */
    ROBIN_OP_DB_FLUSH     = 0x06u,  /* H->D: empty;          D->H: ACK / NAK             */
    ROBIN_OP_SYS_RESET    = 0x07u,  /* H->D: empty;          D->H: ACK, then link drops  */
    ROBIN_OP_REQ_RTC      = 0x08u,  /* H->D: empty;          D->H: RTC_DateTime_t        */
    ROBIN_OP_ACK          = 0xFEu,  /* D->H only: payload = 1 byte echoed cmd            */
    ROBIN_OP_NAK          = 0xFFu   /* D->H only: payload = 1 byte error code            */
} robin_opcode_t;

/* Highest valid host-originated opcode. Anything above (except ACK/NAK,
 * which the host never sends) is rejected by the device. */
#define ROBIN_OP_MAX_HOST_CMD    ROBIN_OP_REQ_RTC

/* ========================================================================== */
/* NAK error codes                                                            */
/* ========================================================================== */

typedef enum {
    ROBIN_ERR_UNKNOWN_CMD    = 0x01u,
    ROBIN_ERR_INVALID_FOOTER = 0x02u,
    ROBIN_ERR_WRITE_FAILED   = 0x03u,
    ROBIN_ERR_INVALID_DATA   = 0x04u
} robin_error_t;

/* ========================================================================== */
/* Wire data types                                                            */
/* ========================================================================== */

/* -------------------------------------------------------------------------- */
/* RTC_DateTime_t - 6 bytes                                                   */
/* Source: firmware/Core/Inc/datetime.h                                       */
/* -------------------------------------------------------------------------- */
PACKED_STRUCT_BEGIN
typedef struct {
    uint8_t year;     /* 0..99 (represents 2000..2099). Plausibility: 24..30 */
    uint8_t month;    /* 1..12                                               */
    uint8_t day;      /* 1..31                                               */
    uint8_t hours;    /* 0..23                                               */
    uint8_t minutes;  /* 0..59                                               */
    uint8_t seconds;  /* 0..59                                               */
} PACKED RTC_DateTime_t;
PACKED_STRUCT_END

STATIC_ASSERT(sizeof(RTC_DateTime_t) == 6,
              "RTC_DateTime_t wire layout drift");

/* -------------------------------------------------------------------------- */
/* Weather_Data_Packed_t - 18 bytes                                           */
/* Source: firmware/Core/Inc/weather_data.h                                   */
/*                                                                            */
/* The `fixedpt` fields are Q9.7 signed fixed-point (FIXEDPT_BITS = 16,       */
/* FIXEDPT_WBITS = 9 including sign, so FBITS = 7).                           */
/*   Range:  -256.000 .. +255.992                                             */
/*   LSB:    1 / 128  ~= 0.0078125                                            */
/* To convert wire int16_t value v to float: v / 128.0f                       */
/* To convert float f to wire int16_t:       (int16_t)lroundf(f * 128.0f)     */
/* -------------------------------------------------------------------------- */
PACKED_STRUCT_BEGIN
typedef struct {
    uint32_t time_stamp;    /* Y2K epoch (seconds since 2000-01-01 00:00:00) */
    int16_t  temperature;   /* Q9.7 degC                                     */
    int16_t  humidity;      /* Q9.7 %RH                                      */
    int16_t  pressure;      /* Q9.7 kPa                                      */
    uint16_t light_par;     /* 0..2500 umol/(s*m^2), integer                 */
    int16_t  rainfall;      /* Q9.7 mm/hr cumulative                         */
    int16_t  dew_point;     /* Q9.7 degC                                     */
    int16_t  bus_value;     /* Q9.7 Blast Unit of Severity                   */
} PACKED Weather_Data_Packed_t;
PACKED_STRUCT_END

STATIC_ASSERT(sizeof(Weather_Data_Packed_t) == 18,
              "Weather_Data_Packed_t wire layout drift");

/* Fixed-point conversion helpers for callers that prefer not to depend on
 * fixedptc.h. Inline so they cost nothing when not used. */
#define ROBIN_FIXEDPT_FBITS    7
#define ROBIN_FIXEDPT_ONE      (1 << ROBIN_FIXEDPT_FBITS)   /* 128 */

#ifdef __cplusplus
#include <cmath>
static inline float robin_fixedpt_to_float(int16_t v) {
    return (float)v / (float)ROBIN_FIXEDPT_ONE;
}
static inline int16_t robin_float_to_fixedpt(float f) {
    /* Saturating round-to-nearest. */
    float scaled = f * (float)ROBIN_FIXEDPT_ONE;
    if (scaled >  32767.0f) return  32767;
    if (scaled < -32768.0f) return -32768;
    return (int16_t)std::lround(scaled);
}
#endif

/* -------------------------------------------------------------------------- */
/* Meta_Data_t - 216 bytes                                                    */
/* Source: firmware/Core/Inc/nv_database.h                                    */
/*                                                                            */
/* This is the station configuration. The host treats it as opaque payload    */
/* and must round-trip it byte-for-byte. The cdctask.c comment says           */
/* "(220 bytes)" but the actual sizeof() the firmware sends is 216 — the     */
/* STATIC_ASSERT below catches any future drift.                              */
/* -------------------------------------------------------------------------- */
#define ROBIN_META_SERVER_NAME_LEN  64u
#define ROBIN_META_SERVER_PATH_LEN  64u
#define ROBIN_META_UPDATE_PATH_LEN  64u

/* validation_value sentinel: any other value means the meta block in F-RAM
 * is uninitialized / corrupted. The firmware checks this on boot. */
#define ROBIN_META_VALIDATION_VALUE 0xA5u

PACKED_STRUCT_BEGIN
typedef struct {
    uint8_t  validation_value;        /* must be ROBIN_META_VALIDATION_VALUE */
    uint16_t region_id;               /* 0..999                              */
    uint16_t station_id;              /* 0..999                              */
    uint8_t  sampling_interval;       /* 1..60 minutes                       */
    float    temperature_adj;         /* -999.99 .. +999.99 degC offset      */
    float    humidity_adj;            /* -999.99 .. +999.99 %RH offset       */
    float    pressure_adj;            /* -999.99 .. +999.99 kPa offset       */
    int16_t  light_adj;               /* -9999 .. +9999  umol/(s*m^2) offset */
    float    rainfall_adj;            /* -999.99 .. +999.99 mm/hr offset     */
    char     server_name[ROBIN_META_SERVER_NAME_LEN];  /* NUL-terminated     */
    char     server_path[ROBIN_META_SERVER_PATH_LEN];  /* NUL-terminated     */
    char     update_path[ROBIN_META_UPDATE_PATH_LEN];  /* NUL-terminated     */
} PACKED Meta_Data_t;
PACKED_STRUCT_END

STATIC_ASSERT(sizeof(Meta_Data_t) == 216,
              "Meta_Data_t wire layout drift");

/* -------------------------------------------------------------------------- */
/* System_Ready_Status_t - 12 bytes                                           */
/* Source: firmware/Core/Inc/main.h                                           */
/*                                                                            */
/* On the firmware side these are C99 `bool` (1 byte each, packed). The host  */
/* uses uint8_t for portability: any non-zero value means "ready" / "ok".     */
/* -------------------------------------------------------------------------- */
PACKED_STRUCT_BEGIN
typedef struct {
    uint8_t ui_ready;
    uint8_t usart_ready;
    uint8_t modbus_ready;
    uint8_t a7670_ready;
    uint8_t bmp390_ready;
    uint8_t sht45_ready;
    uint8_t fram_ready;
    uint8_t datetime_ready;
    uint8_t rainfall_ok;
    uint8_t light_ok;
    uint8_t sd_detected;
    uint8_t sd_write_protected;
} PACKED System_Ready_Status_t;
PACKED_STRUCT_END

STATIC_ASSERT(sizeof(System_Ready_Status_t) == 12,
              "System_Ready_Status_t wire layout drift");

/* ========================================================================== */
/* Plausibility constants (host-side validation before transmit)              */
/* ========================================================================== */

/* RTC bounds — match firmware/Core/Inc/datetime.h */
#define ROBIN_DATETIME_YEAR_MIN     24u
#define ROBIN_DATETIME_YEAR_MAX     30u

/* Meta_Data_t range checks */
#define ROBIN_META_REGION_ID_MAX           999u
#define ROBIN_META_STATION_ID_MAX          999u
#define ROBIN_META_SAMPLING_INTERVAL_MIN   1u
#define ROBIN_META_SAMPLING_INTERVAL_MAX   60u
#define ROBIN_META_ADJ_FLOAT_MIN           -999.99f
#define ROBIN_META_ADJ_FLOAT_MAX            999.99f
#define ROBIN_META_LIGHT_ADJ_MIN           -9999
#define ROBIN_META_LIGHT_ADJ_MAX            9999

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ROBIN_PROTOCOL_H */
