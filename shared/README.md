# shared/ - Robin Weather Station wire protocol

This directory holds the **single source of truth** for the wire protocol
spoken between the Robin Weather Station (STM32L476RG + TinyUSB + FreeRTOS)
and the host configurator application (Qt 6 on Windows / Linux / macOS).

The same `.h` files compile cleanly under:

- `arm-none-eabi-gcc` (firmware target)
- MinGW-w64 GCC (Windows host)
- System GCC (Linux host)
- Apple Clang (macOS host)

## Files

| File | Purpose |
|---|---|
| `protocol_compat.h` | Portability shims: `PACKED`, `STATIC_ASSERT`, endianness guard |
| `protocol_version.h` | Protocol version macros (`ROBIN_PROTOCOL_VERSION_U16`, etc.) |
| `protocol.h` | Authoritative wire-format spec: framing constants, opcodes, error codes, wire structs |

## What's NOT here

- **No CRC.** The protocol relies on USB-CDC's own packet CRC and the
  magic/footer framing for integrity. The firmware's `cdctask.c` does not
  compute a payload CRC; the host must not either.
- **No length field on the wire.** Payload length is implied by the command
  opcode. The receiving side knows how many bytes to read.

## Frame format

```
+---------+---------+---------+----------------------------+---------+---------+
| MAGIC_H | MAGIC_L |   CMD   |         PAYLOAD            | FOOT_H  | FOOT_L  |
|  1 byte |  1 byte |  1 byte |     0..216 bytes           |  1 byte |  1 byte |
+---------+---------+---------+----------------------------+---------+---------+

Host -> Device:   MAGIC = 0xDC 0xB1   FOOTER = 0x23 0x4E   (~MAGIC byte-wise)
Device -> Host:   MAGIC = 0x55 0xAA   FOOTER = 0xAA 0x55
```

All multi-byte numeric fields are **little-endian** (matches STM32 native
order; no byte-swapping needed on x86_64 or ARM64 desktop hosts). The
`__BYTE_ORDER__` check in `protocol_compat.h` enforces this at compile time.

## Commands

| Code | Name | H->D payload | D->H response |
|---|---|---|---|
| `0x01` | `REQ_WEATHER` | (empty) | `Weather_Data_Packed_t` (18B) |
| `0x02` | `REQ_META` | (empty) | `Meta_Data_t` (216B) |
| `0x03` | `SET_META` | `Meta_Data_t` (216B) | ACK echoing `0x03` or NAK + err |
| `0x04` | `SET_RTC` | `RTC_DateTime_t` (6B) | ACK echoing `0x04` or NAK + err |
| `0x05` | `REQ_STATUS` | (empty) | `System_Ready_Status_t` (12B) |
| `0x06` | `DB_FLUSH` | (empty) | ACK echoing `0x06` or NAK + err |
| `0x07` | `SYS_RESET` | (empty) | ACK echoing `0x07`, link then drops |
| `0xFE` | `ACK` | (device only) | 1 byte echoed command code |
| `0xFF` | `NAK` | (device only) | 1 byte error code |

## NAK error codes

| Code | Name | Meaning |
|---|---|---|
| `0x01` | `UNKNOWN_CMD` | CMD byte was not a recognized opcode |
| `0x02` | `INVALID_FOOTER` | One or both footer bytes did not match `~MAGIC` |
| `0x03` | `WRITE_FAILED` | Device-side persistence failed (FRAM, RTC, etc.) |
| `0x04` | `INVALID_DATA` | Payload contents were rejected |

## Wire data types

| Type | Size | Notes |
|---|---|---|
| `RTC_DateTime_t` | 6 B | year/month/day/hours/minutes/seconds, all `uint8_t` |
| `Weather_Data_Packed_t` | 18 B | `time_stamp` is Y2K epoch; sensor fields are Q9.7 fixed-point (`int16_t` / 128.0) |
| `Meta_Data_t` | 216 B | Station configuration; treat as opaque round-trip from host |
| `System_Ready_Status_t` | 12 B | 12 × `uint8_t` boolean readiness flags |

The sizes above are enforced at compile time by `STATIC_ASSERT` in
`protocol.h`. **If any of those static asserts ever fires, the protocol
is broken and host and firmware will not interoperate.** Treat a failing
assert as a release blocker.

## Adding new commands or fields

- **MINOR version bump** is appropriate for: new opcode that older firmware/
  host can safely ignore; new field appended (never inserted) to an existing
  struct.
- **MAJOR version bump** is required for: field reorder, field removal,
  opcode renumbering, magic/footer change, type change.

Either way: update `protocol_version.h` and bump the appropriate macro.

## Why no CRC?

The firmware author chose to rely on the USB-CDC transport's own data
integrity (USB CRC on every bulk packet, automatic retransmission on
errors) plus the start/end magic framing. For a wired full-speed USB
link at 12 Mbit/s this is reasonable. If the protocol is ever extended
to run over a less-reliable transport (wireless, RS-485, etc.) a CRC
would need to be added in a MAJOR version bump.
