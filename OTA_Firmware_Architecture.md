# OTA Firmware Update Architecture
## STM32L476RG + CY15B116QN FRAM + A7670E LTE Modem

**Document Version:** 1.5  
**Status:** Implementation In Progress — Phase 3 Pending  
**Platform:** STM32L476RG / FreeRTOS / SIMCom A7670E / Cypress CY15B116QN  
**Build Environment:** PlatformIO + STM32CubeMX  

---

## Revision History

| Version | Change Summary |
|---------|---------------|
| 1.0 | Initial architecture draft |
| 1.1 | Applied firmware size constraints (512 KB code / 96 KB data); adopted PlatformIO + CubeMX toolchain; changed OTA version check to device-initiated polling with separate URL paths; revised FRAM layout (DB at 0x000000, Config at 0x0FE000); formatted for Claude Code consumption |
| 1.2 | Replaced three-endpoint JSON protocol with single UPDATE_PATH text protocol; version changed from major.minor pair to single uint32 FW_VERSION; fw_version_major/minor fields in OtaControlBlock_t merged into fw_version uint32_t; Meta_Data_t server_path reduced to 64 B and update_path[64] added; download now via GET to get_firmware.php with offset/length query params |
| 1.3 | Documented A7670E dual SSL service model (CCH vs HTTPS); clarified that data upload uses CCH service and OTA download must use HTTPS service; noted NTP async-confirm bug, PEM cert format requirement, CCH/HTTPS service-switch requirement for Phase 2; updated risks R-11–R-14; resolved Q-7 (mbedTLS not present) |
| 1.4 | Added §9.4 Cache Management — STM32L476 I-cache and D-cache must be explicitly reset after Flash programming to prevent stale read-back verify and wrong instruction fetch; updated §9.2 pseudocode, Risk R-15 |
| 1.5 | Phase 0–2.1 code-complete: CCH service eliminated entirely; both data upload and OTA download now use A7670E `AT+HTTP*` service (`AT+HTTPINIT/HTTPPARA/HTTPACTION/HTTPREAD/HTTPTERM`); §2.3, §10.3, §10.5 rewritten; §11.1 corrected (software CRC-32, HW CRC locked to Modbus CRC-16); Phase 2.1 added to §14; R-13/R-14 resolved; §7.1 repo layout corrected; P0–P2.1 tasks marked complete |

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Hardware Constraints & Memory Map](#2-hardware-constraints--memory-map)
3. [Current Firmware Limitations](#3-current-firmware-limitations)
4. [OTA Architecture Design](#4-ota-architecture-design)
5. [FRAM Layout](#5-fram-layout)
6. [Internal Flash Layout](#6-internal-flash-layout)
7. [Software Module Architecture](#7-software-module-architecture)
8. [OTA State Machine](#8-ota-state-machine)
9. [Bootloader Design](#9-bootloader-design)
10. [OTA Version Check & Download Protocol](#10-ota-version-check--download-protocol) *(includes §10.5 A7670E SSL/TLS Service Architecture)*
11. [Image Integrity & Security](#11-image-integrity--security)
12. [FreeRTOS Task Architecture](#12-freertos-task-architecture)
13. [PlatformIO + CubeMX Project Structure](#13-platformio--cubemx-project-structure)
14. [Refactoring Plan & Phased Roadmap](#14-refactoring-plan--phased-roadmap)
15. [Risk Register](#15-risk-register)
16. [Open Questions & Decisions Required](#16-open-questions--decisions-required)
17. [References](#17-references)

---

## 1. System Overview

### 1.1 Hardware Platform

| Component | Part | Interface | Notes |
|-----------|------|-----------|-------|
| MCU | STM32L476RG | — | Cortex-M4F, 1 MB Flash, 128 KB SRAM |
| FRAM | CY15B116QN | SPI1 | 2 MB (16 Mbit), byte-addressable, unlimited endurance |
| LTE Modem | SIMCom A7670E | USART3 | AT command set, supports TLS 1.2, HTTPS |
| RTOS | FreeRTOS | — | Preemptive scheduler |

### 1.2 Development Toolchain

| Tool | Role |
|------|------|
| **PlatformIO** | Build system, dependency management, upload, unit test runner |
| **STM32CubeMX** | Peripheral configuration, clock tree, FreeRTOS kernel config, HAL generation |
| **GCC ARM Embedded** | Compiler (via PlatformIO `framework = stm32cube`) |
| **OpenOCD / ST-Link** | Flash programming and debug |

> CubeMX generates HAL and FreeRTOS initialisation code into `Core/` and `Middlewares/`. PlatformIO consumes this output directly via its `stm32cube` framework. CubeMX must be re-run whenever peripheral configuration changes; its generated files are committed to the repository.

### 1.3 Firmware Size Budget

| Region | Limit | Enforced By |
|--------|-------|-------------|
| Code (`.text` + `.rodata`) | 512 KB | Linker script `FLASH` region size |
| Data (`.data` + `.bss` + heap + stack) | 96 KB | Linker script `RAM` region size (SRAM1 only) |

> SRAM2 (32 KB) is reserved for battery-backed OTA state flags and is **not** counted in the 96 KB data budget. The bootloader uses its own 32 KB Flash partition and a separate, minimal RAM footprint with no FreeRTOS overhead.

### 1.4 System Behavior (Current)

- Firmware collects data continuously and stores it in the FRAM database (first 1 MB).
- Twice daily, firmware establishes HTTPS connection via A7670E and uploads collected data.
- No OTA capability exists. The server does not push version notifications.
- **No OTA update capability exists today.**

### 1.5 Desired End-State

```
Scheduled OTA check (device-initiated)
        │
        ▼
GET <UPDATE_PATH>/  →  plain text "V.#####:L.$$$$$$$"
        │
        │  V > FW_VERSION?
        │  YES
        ▼
GET <UPDATE_PATH>/get_firmware.php?offset=<byte_offset>&length=<bytes>  (chunked)
        →  raw binary image written to FRAM staging
        │
        ▼
Integrity check (CRC-32 + SHA-256) passes
        │
        ▼
OtaControlBlock written to FRAM config sector (ota_pending = 1)
        │
        ▼
Controlled reboot → Bootloader reads OtaControlBlock
        │
        ▼
Bootloader copies FRAM image → STM32 internal Flash (page by page, verified)
        │
        ▼
Bootloader jumps to new application at 0x08008000
        │
        ▼
New app calls ota_confirm_success() within 60 s
        │
        ▼
System resumes normal operation
```

---

## 2. Hardware Constraints & Memory Map

### 2.1 STM32L476RG Flash & SRAM

| Resource | Size | Usage |
|----------|------|-------|
| Internal Flash | 1 MB total | 32 KB bootloader + 512 KB application (≤ 480 KB code used) |
| SRAM1 | 96 KB | Application data, heap, FreeRTOS stacks — **hard limit** |
| SRAM2 | 32 KB | OTA state flags, battery-backed across resets — not in data budget |
| Backup registers | 32 × 32-bit | Supplementary OTA flags across deep sleep / power loss |

> **Flash layout note:** The STM32L476RG Flash is dual-bank (2 × 512 KB). Bank 1 holds bootloader + application. Bank 2 is reserved for future dual-bank atomic swap; it is unused in v1 of this design.

Reference: *STM32L476xx Reference Manual* RM0351, Section 3.3.

### 2.2 CY15B116QN FRAM

| Parameter | Value |
|-----------|-------|
| Total capacity | 2 MB (2,097,152 bytes) |
| SPI max clock | 40 MHz |
| Write endurance | Unlimited (ferroelectric cell) |
| Data retention | 151 years at 85 °C |
| Addressing | Byte-addressable, no page boundary restriction |

> FRAM requires no erase cycle and has no page-write latency. This makes it ideal for both the ring-buffer database and the OTA image staging area.

Reference: *CY15B116QN Datasheet*, Cypress/Infineon, Doc. 001-99272.

### 2.3 SIMCom A7670E LTE Modem

| Capability | Detail |
|-----------|--------|
| SSL/TLS | TLS 1.2, mutual authentication (client + server cert), SNI |
| SSL service — HTTP(S) | Modem-managed HTTP(S); `AT+HTTPINIT/HTTPPARA/HTTPACTION/HTTPREAD/HTTPTERM`; modem handles HTTP framing |
| RX buffer per AT read | `AT+HTTPREAD=0,<len>` returns up to `<len>` bytes; firmware loops until all chunk bytes received |
| UART baud | 115200 bps (application default); RTS/CTS enabled |
| Flow control | Hardware RTS/CTS recommended for image download |

> **Phase 2.1 note:** The A7670E exposes only the `AT+HTTP*` service (confirmed from AT Command Manual V1.09 Chapter 16). The CCH service (`AT+CCH*`) does not exist on this variant. Both data upload (`a7670_https_uploader.c`) and OTA download (`a7670_ssl_downloader.c`) use `AT+HTTP*`. `a7670_ssl_uploader.c` has been deleted.

Reference: *SIMCom A7670 Series AT Command Manual*, V1.09, Chapter 16 (HTTP).

---

## 3. Current Firmware Limitations

| # | Gap | Impact |
|---|-----|--------|
| G-1 | No bootloader — reset vector goes directly to application | Cannot redirect execution to a new image |
| G-2 | No firmware version query to server | No mechanism to detect available updates |
| G-3 | No firmware image download logic | Cannot retrieve binary via HTTPS |
| G-4 | Second 1 MB FRAM space unused and undefined | No staging area for incoming image |
| G-5 | No image integrity check (CRC/hash) | Cannot validate image before Flash write |
| G-6 | No OTA state persistence across resets | Power loss during OTA leaves system undefined |
| G-7 | FRAM config sector does not include OTA control fields | No way to signal bootloader about pending update |
| G-8 | No rollback mechanism | Bad update could permanently brick device |
| G-9 | FRAM database module uses raw pointer returns | Race condition risk during concurrent OTA + DB access |
| G-10 | No PlatformIO project structure | Build reproducibility not guaranteed |

---

## 4. OTA Architecture Design

### 4.1 Design Principles

1. **Fail-safe first.** An interrupted or corrupted OTA always leaves the device bootable on the previous firmware.
2. **Atomic commit.** The switch from old to new firmware is a single flag write (`ota_pending = 1`) + reboot. No partial state exists.
3. **Minimal RAM budget.** Download buffer is a static 512-byte chunk buffer. No dynamic allocation during OTA. Total OTA subsystem static RAM ≤ 2 KB.
4. **Separation of concerns.** Bootloader, OTA manager, download subsystem, and database are independent modules with defined C interfaces.
5. **Idempotent operations.** Restarting any OTA phase (download, verify, flash) produces the same result as completing it once.
6. **Device-initiated only.** The embedded system polls the server for version information. The server never pushes or triggers an update.

### 4.2 Top-Level Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    STM32L476RG Internal Flash                │
│                                                             │
│  ┌─────────────────────┐   ┌─────────────────────────────┐  │
│  │   Bank 1 (512 KB)   │   │      Bank 2 (512 KB)        │  │
│  │  ┌───────────────┐  │   │  ┌─────────────────────┐    │  │
│  │  │  Bootloader   │  │   │  │   Reserved           │    │  │
│  │  │  (~32 KB)     │  │   │  │   (future dual-bank) │    │  │
│  │  ├───────────────┤  │   │  └─────────────────────┘    │  │
│  │  │  Application  │  │   │                             │  │
│  │  │  (≤ 512 KB)   │  │   └─────────────────────────────┘  │
│  │  └───────────────┘  │                                    │
│  └─────────────────────┘                                    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                 CY15B116QN FRAM (2 MB, SPI1)                 │
│                                                             │
│  ┌──────────────────────────────────┐                       │
│  │        DB Region  (first 1 MB)   │                       │
│  │  0x000000 – 0x0FFFFF             │                       │
│  │                                  │                       │
│  │  ┌──────────────────────────┐    │                       │
│  │  │ DB Ring Buffer           │    │                       │
│  │  │ 0x000000 – 0x0FDFFF     │    │                       │
│  │  │ (~1,016 KB)              │    │                       │
│  │  ├──────────────────────────┤    │                       │
│  │  │ Config Sector            │    │                       │
│  │  │ 0x0FE000 – 0x0FEFFF     │    │                       │
│  │  │ (4 KB)                   │    │                       │
│  │  └──────────────────────────┘    │                       │
│  └──────────────────────────────────┘                       │
│                                                             │
│  ┌──────────────────────────────────┐                       │
│  │     OTA Staging Region (1 MB)    │                       │
│  │  0x100000 – 0x1FFFFF             │                       │
│  │                                  │                       │
│  │  ┌──────────────────────────┐    │                       │
│  │  │ OTA Header  (256 B)      │    │                       │
│  │  │ 0x100000                 │    │                       │
│  │  ├──────────────────────────┤    │                       │
│  │  │ Image Data (≤ 512 KB)    │    │                       │
│  │  │ 0x100100                 │    │                       │
│  │  ├──────────────────────────┤    │                       │
│  │  │ Download Bitmap (1 KB)   │    │                       │
│  │  │ 0x17F000                 │    │                       │
│  │  ├──────────────────────────┤    │                       │
│  │  │ CRC-32 Footer (4 B)      │    │                       │
│  │  │ 0x17F400                 │    │                       │
│  │  └──────────────────────────┘    │                       │
│  └──────────────────────────────────┘                       │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. FRAM Layout

### 5.1 First 1 MB — Database Region (0x000000 – 0x0FFFFF)

| Sub-region | Start Address | End Address | Size | Description |
|-----------|--------------|-------------|------|-------------|
| DB Ring Buffer | 0x000000 | 0x0FDFFF | ~1,016 KB | Sensor/data records, existing ring-buffer logic |
| Config Sector | 0x0FE000 | 0x0FEFFF | 4 KB | System config + OTA Control Block |
| Reserved | 0x0FF000 | 0x0FFFFF | 4 KB | Future use |

> **Layout rationale:** The DB ring buffer grows from address 0 upward, maximising contiguous space and simplifying address arithmetic. The Config Sector is fixed at the top of the first megabyte (0x0FE000) so its address is a compile-time constant independent of DB capacity.

### 5.2 Config Sector Layout (0x0FE000, 4 KB)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0x0000 | 128 B | System Config | Existing fields (baud rates, server URLs, etc.) |
| +0x0080 | 64 B | OTA Control Block — Primary | See struct below |
| +0x00C0 | 64 B | OTA Control Block — Mirror | Redundant copy for power-loss tolerance |
| +0x0100 | — | Reserved | Future config fields |

### 5.3 OtaControlBlock_t Definition

```c
/* Shared between application and bootloader — place in shared/ota_control_block.h */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* 0x0AC0FFEE — marks a valid block                */
    uint8_t  ota_pending;        /* 0x01 = image ready to flash, 0x00 = idle         */
    uint8_t  ota_tried;          /* incremented each boot attempt; max 3             */
    uint8_t  ota_confirmed;      /* 0x01 = new app called ota_confirm_success()      */
    uint8_t  pad0;               /* reserved — write 0                               */
    uint32_t image_size;         /* byte count of valid image in staging area         */
    uint8_t  image_sha256[32];   /* SHA-256 digest — sole whole-image integrity check */
    uint32_t fw_version;         /* monotonic version number of staged image         */
    uint8_t  reserved[8];        /* future use — write 0                             */
    uint32_t download_timestamp; /* Y2K epoch of download completion                 */
    uint32_t block_crc32;        /* CRC-32/MPEG-2 over bytes [0..59] of this struct  */
} OtaControlBlock_t;             /* 64 bytes total: 4+4+32+4+8+4+4+4 = 64           */
```

> **Dual-copy write procedure:** Write primary → verify primary CRC → write mirror → verify mirror CRC. On read, validate both copies by their `block_crc32`. If copies differ, use the copy with a valid CRC. If neither is valid, treat as no pending OTA.

### 5.4 Second 1 MB — OTA Staging Region (0x100000 – 0x1FFFFF)

| Sub-region | Start Address | Size | Description |
|-----------|--------------|------|-------------|
| OTA Staging Header | 0x100000 | 256 B | Mirrors OtaControlBlock + in-progress download URL/size |
| Image Data | 0x100100 | ≤ 512 KB | Raw `.bin` image (no ELF headers) |
| Download Bitmap | 0x17F000 | 1 KB | 1 bit per 512-byte chunk; set = received and written |
| (Unused) | 0x17F400 | 4 B | Previously CRC-32 footer; whole-image CRC removed — SHA-256 in OCB |
| Reserved | 0x17F404 | — | Remainder of staging region |

> **Resume support:** The 1 KB download bitmap supports up to 8,192 chunks × 512 bytes = 4 MB of resumable download capacity, which is sufficient for this 512 KB image constraint. On reconnect after power loss, the firmware scans the bitmap and requests only missing chunk ranges.

---

## 6. Internal Flash Layout

```
0x08000000 ┌──────────────────────────────┐
           │  Bootloader                  │  32 KB  (pages 0–15, Bank 1)
           │  Read-out protection active  │
0x08008000 ├──────────────────────────────┤
           │  Application                 │  480 KB (pages 16–255, Bank 1)
           │  Code ≤ 512 KB (fits here)   │
           │  Linked origin: 0x08008000   │
0x08080000 ├──────────────────────────────┤  ← Bank 2 boundary
           │  Bank 2 (512 KB)             │
           │  Reserved — future dual-bank │
0x08100000 └──────────────────────────────┘
```

> **Linker constraint:** The application `.text` + `.rodata` sections must not exceed 480 KB (0x78000 bytes). This is enforced by the linker script `FLASH` region definition. The 512 KB firmware size budget is the combined code + rodata ceiling stated by the hardware constraint; 480 KB is the actual available region after the 32 KB bootloader.

**PlatformIO linker script excerpt (application):**

```
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 480K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
  RAM2  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K   /* SRAM2 — OTA flags only */
}
```

Reference: *STM32L476xx Reference Manual* RM0351, Section 3.3.

---

## 7. Software Module Architecture

### 7.1 Repository Structure (PlatformIO)

```
project_root/
├── platformio.ini                  # Root PlatformIO workspace (two envs: nucleo_l476rg, bootloader)
│
├── Src/                            # Application source (CubeMX-generated + custom)
│   ├── main.c                      # CubeMX-generated entry; custom code in USER CODE guards
│   ├── freertos.c                  # Task creation (MX_FREERTOS_Init)
│   ├── maintask.c                  # 1 s sensor read, pack, save to FRAM + SD
│   ├── ssluploadtask.c             # Noon/midnight HTTPS upload via A7670 (AT+HTTP*)
│   ├── uitask.c / ucctask.c        # LCD, LEDs, button debounce
│   ├── cdctask.c                   # USB CDC binary protocol handler
│   ├── ota_image_writer.c/.h       # Chunked FRAM write + download bitmap
│   ├── watchdog_task.c/.h          # Per-task heartbeat + IWDG refresh
│   └── sha256.c/.h                 # Standalone SHA-256 (no mbedTLS)
│
├── Inc/                            # Application headers
│
├── lib/                            # Custom library modules
│   ├── sensors/                    # bmp390, sht45, modbus, rain_light
│   ├── A7670/                      # LTE modem driver
│   │   ├── a7670.c/.h              # Modem init: NTP, cert injection, SSL context 0
│   │   ├── a7670_at_channel.c/.h   # AT command channel
│   │   ├── a7670_https_uploader.c/.h  # Data upload via AT+HTTP* (Phase 2.1)
│   │   └── a7670_ssl_downloader.c/.h  # OTA download via AT+HTTP* (Phase 2)
│   ├── SPI_FRAM/                   # cy15b116qn.c, nv_database.c
│   ├── user_interface/             # mcp23017.c, ui.c
│   ├── usart_subsystem/            # uart_subsystem.c
│   ├── time/                       # datetime.c, y2k_time.c
│   └── utils/                     # weather_data.h, fixedptc.h
│
├── bootloader/                     # Separate PlatformIO environment (bare-metal, no FreeRTOS)
│   ├── platformio.ini
│   ├── ldscript_boot.ld            # FLASH origin 0x08000000, 32 KB
│   └── src/
│       ├── main.c                  # Bootloader entry
│       ├── boot_flash.c/.h         # Flash page erase/write/read-back verify
│       └── boot_fram.c/.h          # Polling SPI1 FRAM read (no DMA)
│
├── shared/                         # Compiled into both bootloader and application
│   ├── ota_control_block.c/.h      # OtaControlBlock_t read/write/validate (dual-copy)
│   ├── crc32.c/.h                  # Software CRC-32/MPEG-2 (HW CRC locked to Modbus)
│   └── fram_addresses.h            # FRAM address constants (single source of truth)
│
└── ldscript_app.ld                 # Application linker: FLASH origin 0x08008000, 480 KB
```

### 7.2 Module Dependency Graph

```
┌──────────────────────────────────────────────────────────┐
│                    Application Layer                      │
│  ┌──────────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │ DataCollection   │  │  OtaManager  │  │  Upload   │  │
│  │ Task             │  │  Task        │  │  Task     │  │
│  └────────┬─────────┘  └──────┬───────┘  └─────┬─────┘  │
│           │                   │                 │         │
│  ┌────────▼───────────────────▼─────────────────▼──────┐  │
│  │                   Service Layer                      │  │
│  │  ┌──────────┐  ┌─────────────────┐  ┌───────────┐  │  │
│  │  │ fram_db  │  │ ota_image_writer │  │  https_   │  │  │
│  │  │          │  │                 │  │  client   │  │  │
│  │  └────┬─────┘  └────────┬────────┘  └─────┬─────┘  │  │
│  └───────┼─────────────────┼─────────────────┼─────────┘  │
│          │                 │                 │             │
│  ┌───────▼─────────────────▼─────────────────▼─────────┐  │
│  │                  HAL / Driver Layer                   │  │
│  │  ┌──────────┐  ┌─────────────────┐  ┌───────────┐  │  │
│  │  │ SPI1     │  │ USART3 AT chan.  │  │ RTC/IWDG  │  │  │
│  │  │ (CubeMX) │  │ (CubeMX)        │  │ (CubeMX)  │  │  │
│  │  └──────────┘  └─────────────────┘  └───────────┘  │  │
│  └──────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│              Bootloader (separate PlatformIO env)         │
│  ┌────────────────────────────────────────────────────┐  │
│  │  ota_control_block  (shared/)                      │  │
│  │  boot_fram  (polling SPI1, no RTOS)                │  │
│  │  boot_flash  (HAL_FLASH erase/write/verify)        │  │
│  │  IWDG refresh loop                                 │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### 7.3 New Module Interfaces

#### `ota_control_block.h` (shared)
```c
OtaStatus_t ocb_read(OtaControlBlock_t *out);
OtaStatus_t ocb_write(const OtaControlBlock_t *in);
OtaStatus_t ocb_clear(void);
bool        ocb_is_valid(const OtaControlBlock_t *cb);
```

#### `ota_image_writer.h` (application)
```c
OiwResult_t oiw_begin(uint32_t image_size);                        /* loads bitmap from FRAM */
OiwResult_t oiw_write_chunk(uint16_t chunk_index, const uint8_t *data, uint16_t len);
bool        oiw_chunk_received(uint16_t chunk_index);
OiwResult_t oiw_finalize(void);                                    /* no-op; returns OIW_OK */
bool        oiw_resume_info(uint16_t *next_missing_chunk);
```

#### `ota_manager_task.h` (application)
```c
void        OtaManagerTask(void *pvParameters);
void        ota_confirm_success(void);   /* called by app after stable boot */
OtaState_t  ota_get_state(void);
```

---

## 8. OTA State Machine

All states are persisted in `OtaControlBlock_t` (FRAM config sector). The state survives power loss.

```
         ┌─────────────────────────────────────────────┐
         │            OTA_STATE_IDLE                   │
         │  Normal operation. No update pending.       │
         └──────────────────┬──────────────────────────┘
                            │  OTA check timer fires
                            │  (twice daily, same schedule as upload)
                            ▼
         ┌─────────────────────────────────────────────┐
         │       OTA_STATE_POLLING_VERSION             │
         │  GET /ota/version                           │
         │  Response parsed: {"version": "X.Y"}        │
         └──────────────────┬──────────────────────────┘
                            │  server version > running version
                            ▼
         ┌─────────────────────────────────────────────┐
         │       OTA_STATE_FETCHING_METADATA           │
         │  GET /ota/metadata                          │
         │  Response: {size, crc32, sha256, url}        │
         │  Stored in OTA Staging Header (FRAM)         │
         └──────────────────┬──────────────────────────┘
                            │  metadata valid
                            ▼
         ┌─────────────────────────────────────────────┐
         │         OTA_STATE_DOWNLOADING               │◄──┐
         │  GET /ota/download  (chunked loop)          │   │ resume after
         │  512-byte chunks → FRAM staging             │   │ power loss
         │  Download bitmap updated per chunk          │   │
         └──────────────────┬──────────────────────────┘   │
                            │  bitmap full (all chunks)─────┘
                            ▼
         ┌─────────────────────────────────────────────┐
         │       OTA_STATE_DOWNLOAD_COMPLETE           │
         │  CRC-32 + SHA-256 verification              │
         └──────────────────┬──────────────────────────┘
                            │  integrity check PASS
                            ▼
         ┌─────────────────────────────────────────────┐
         │         OTA_STATE_VERIFIED                  │
         │  OtaControlBlock written: ota_pending=1     │
         │  Graceful shutdown: DataCollection flushed  │
         └──────────────────┬──────────────────────────┘
                            │  safe to reboot
                            ▼
         ┌─────────────────────────────────────────────┐
         │       OTA_STATE_REBOOT_PENDING              │
         │  HAL_NVIC_SystemReset() called              │
         │  → Bootloader takes over                    │
         └──────────────────┬──────────────────────────┘
                            │  bootloader flashes + jumps to new app
                            ▼
         ┌─────────────────────────────────────────────┐
         │       OTA_STATE_CONFIRMING                  │
         │  New firmware running.                      │
         │  App must call ota_confirm_success()        │
         │  within T_confirm = 60 s                    │
         └──────────────────┬──────────────────────────┘
                            │  confirmed
                            ▼
         ┌─────────────────────────────────────────────┐
         │            OTA_STATE_IDLE                   │
         │  ota_confirmed=1, ota_pending=0             │
         └─────────────────────────────────────────────┘

  Integrity FAIL    → OTA_STATE_IDLE (staging invalidated, old FW retained)
  ota_tried >= 3    → Bootloader boots old Flash image unchanged
  T_confirm timeout → IWDG fires → bootloader rollback
```

---

## 9. Bootloader Design

### 9.1 Responsibilities

The bootloader is a **separate PlatformIO environment** (no FreeRTOS) linked at `0x08000000`. It must complete within the IWDG timeout window (recommended: set IWDG to 4 seconds, refresh every 1 second during Flash programming).

1. Initialise SPI1 (polling mode), IWDG, Flash HAL — nothing else.
2. Read `OtaControlBlock_t` (primary + mirror) from FRAM `0x0FE080`.
3. If `ota_pending == 0x01` and `ota_tried < 3`:
   - Increment `ota_tried`, write back both copies.
   - Validate image CRC-32 over FRAM staging area.
   - If valid: program application Flash pages (erase → write → read-back verify).
   - On all pages verified: clear `ota_pending`, set `ota_confirmed = 0`, jump to `0x08008000`.
   - On any page failure: clear `ota_pending`, set error code, jump to old `0x08008000`.
4. Otherwise: jump directly to existing application at `0x08008000`.

### 9.2 Flash Programming Loop

```c
/* Pseudo-code — not production code */
for (uint32_t page = APP_FIRST_PAGE; page <= APP_LAST_PAGE; page++) {
    HAL_IWDG_Refresh(&hiwdg);

    uint32_t src = FRAM_STAGING_IMAGE_BASE + (page - APP_FIRST_PAGE) * FLASH_PAGE_SIZE;
    uint32_t dst = FLASH_APP_ORIGIN       + (page - APP_FIRST_PAGE) * FLASH_PAGE_SIZE;

    fram_read_blocking(src, page_buf, FLASH_PAGE_SIZE);

    HAL_FLASH_Unlock();
    FLASH_Erase_Page(page, FLASH_BANK_1);
    FLASH_Program_Block(dst, page_buf, FLASH_PAGE_SIZE / 8);  /* 64-bit writes */
    HAL_FLASH_Lock();

    /* Reset D-cache before read-back verify — see §9.4. */
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_ENABLE();

    if (memcmp((void *)dst, page_buf, FLASH_PAGE_SIZE) != 0) {
        boot_abort(BOOT_ERR_FLASH_VERIFY);
        return;   /* jump to old app */
    }
}

/* Reset I-cache after all pages written — see §9.4. */
__HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
__HAL_FLASH_INSTRUCTION_CACHE_RESET();
__HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
```

> STM32L476 Flash requires 64-bit (double-word) write granularity. `page_buf` must be 8-byte aligned; declare as `static uint64_t page_buf[FLASH_PAGE_SIZE / 8]` in bootloader scope.

Reference: *STM32L476xx Reference Manual* RM0351, Section 3.3.7.

### 9.3 Security

- Enable **RDP Level 1** on bootloader Flash pages after production programming to prevent debugger readout.
- Set **FLASH_WRP** write-protection bits on pages 0–15 (bootloader partition) so the application cannot overwrite the bootloader.
- Bootloader validates that the first word of the staged image (initial stack pointer) falls within `[0x20000000, 0x20018000)` before jumping.

### 9.4 Cache Management During Flash Programming

The STM32L476RG's Flash controller includes an ST-proprietary **instruction cache (ICACHE)** and **data cache (DCACHE)**, part of the ART Accelerator (`FLASH_ACR` register bits `ICEN`/`ICRST` and `DCEN`/`DCRST`). These are **not** ARM Cortex-M7 SCB caches — they are read-only caches for Flash accesses. **Both are enabled by default after reset** and by CubeMX-generated `SystemInit()`.

These caches create two OTA failure modes:

| Failure | Root Cause | Consequence |
| ------- | --------- | ----------- |
| Stale read-back verify | D-cache returns cached (pre-erase) data when `memcmp` reads freshly-programmed Flash | Verify passes on stale data; corrupted page goes undetected |
| Wrong instructions after jump | I-cache retains old application's instructions at 0x08008000–0x0807FFFF | New application executes old code after bootloader jump |

**Required HAL macros** (defined in `stm32l4xx_hal_flash.h`):

```c
/* Reset D-cache — must disable before reset, re-enable after */
__HAL_FLASH_DATA_CACHE_DISABLE();
__HAL_FLASH_DATA_CACHE_RESET();
__HAL_FLASH_DATA_CACHE_ENABLE();

/* Reset I-cache — same disable/reset/enable sequence */
__HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
__HAL_FLASH_INSTRUCTION_CACHE_RESET();
__HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
```

**Implementation rules for `boot_flash.c`:**

1. After `HAL_FLASH_Lock()` on each page and **before** the `memcmp` read-back verify: reset the D-cache.
2. After **all** pages have been successfully programmed (end of loop, before returning `true`): reset the I-cache.

> `HAL_FLASHEx_Erase()` does **not** automatically invalidate the D-cache for the erased page in all HAL versions — always do it explicitly. Never rely on the HAL to handle post-programming cache maintenance.

---

## 10. OTA Version Check & Download Protocol

### 10.1 Device-Initiated Check Schedule

The `OtaManagerTask` wakes on the same RTC alarm used by `UploadTask` (twice daily). The two tasks share the same wakeup event but execute sequentially via a task notification. The OTA check is performed **after** the upload completes, in the same modem session where possible.

### 10.2 API — Single UPDATE_PATH

`UPDATE_PATH` is a base URL stored in `Meta_Data_t.update_path` (64 bytes) in the Config Sector.  It is runtime-configurable via the CDC `set config` command.  No JSON is used — the protocol uses plain text and raw binary to minimise modem bandwidth.

#### Endpoint 1 — Version + Size Check

```
GET <UPDATE_PATH>/
```

Response: an HTML page whose body contains **only** the version/size/hash token:

```text
V.#####:L.$$$$$$$:H.<sha256hex>
```

- `V.#####` — server firmware version as a decimal `uint32_t` (no padding, no leading zeros required)
- `L.$$$$$$$` — firmware image size in bytes as a decimal `uint32_t` (no padding)
- `H.<sha256hex>` — SHA-256 of the firmware image as 64 lowercase hex characters
- Fields delimited by `:`, terminated by `\n`; trailing whitespace is ignored
- The response is `Content-Type: text/html`; the parser must **scan** the full response buffer for `V.\d+:L.\d+:H.[0-9a-f]{64}` — do not assume the token starts at byte 0
- Any response that does not contain this pattern → OTA skipped silently (treat as "no update"); **do not retry**
- The device compares `V` against the compile-time constant `FW_VERSION`. If `V` ≤ `FW_VERSION` → stop.
- Retry policy: up to 3 retries on modem/network errors; 0 retries on a parseable but non-matching body.

#### Endpoint 2 — Binary Download

```
GET <UPDATE_PATH>/get_firmware.php?offset=<byte_offset>&length=<byte_count>
```

Response: `<byte_count>` bytes of firmware data followed by a 4-byte little-endian CRC-32/MPEG-2 of that chunk.  Total HTTP response body = `<byte_count> + 4`.

- `offset` and `length` are optional; omit both to download the image from the beginning.
- For resume: supply `offset` equal to the byte offset of the first missing chunk (derived from the download bitmap in FRAM).
- Per-chunk CRC is validated by `ssl_downloader_get_chunk()` before writing to FRAM; `SSL_DL_ERR_CRC` is returned on mismatch.
- The device reads via `AT+HTTPREAD=0,516` (512 data + 4 CRC); A7670E **HTTP(S) service** — see §10.3.
- Retry policy: up to 3 retries per chunk on modem/network errors or CRC mismatch.

### 10.3 Chunked Download Loop

```c
/* Pseudo-code */
uint16_t next_chunk;
uint32_t offset   = oiw_resume_info(&next_chunk) ? next_chunk * CHUNK_SIZE : 0u;
uint32_t retries  = 0u;
char     chunk_url[HTTPS_DL_URL_MAX_LEN];

sha256_ctx_t sha_ctx;
sha256_init(&sha_ctx);
/* Re-hash already-received chunks from FRAM on resume (ensures SHA context is complete). */

while (offset < image_size_bytes) {
    uint32_t req_len = MIN(CHUNK_SIZE, image_size_bytes - offset);
    snprintf(chunk_url, sizeof(chunk_url),
             "%s/get_firmware.php?offset=%lu&length=%lu",
             update_path, (unsigned long)offset, (unsigned long)req_len);

    uint16_t got;
    SslDlResult_t rc = ssl_downloader_get_chunk(chunk_url, chunk_buf,
                                                CHUNK_SIZE, &got);
    if (rc != SSL_DL_OK) {
        if (++retries >= 3u) { /* abort download */ break; }
        continue;
    }
    retries = 0u;

    uint16_t chunk_idx = (uint16_t)(offset / CHUNK_SIZE);
    oiw_write_chunk(chunk_idx, chunk_buf, got);
    sha256_update(&sha_ctx, chunk_buf, got);
    offset += got;
    HAL_IWDG_Refresh(&hiwdg);
}

/* After all chunks: verify SHA-256 against value received from version endpoint. */
uint8_t digest[32];
sha256_final(&sha_ctx, digest);
if (memcmp(digest, expected_sha256, 32u) != 0) {
    /* Integrity failure — invalidate staging, remain in idle. */
}
```

### 10.4 Version Comparison

```c
/* In ota_manager_task.c */
static bool server_version_is_newer(uint32_t srv_version) {
    return srv_version > FW_VERSION;
}
```

### 10.5 A7670E SSL/TLS Service Architecture

All TLS crypto runs on the A7670E modem's internal processor. The STM32 pays zero crypto cost.

> **Phase 2.1 finding:** The A7670E exposes only the `AT+HTTP*` service (confirmed from AT Command Manual V1.09 Chapter 16). There is no separate CCH (`AT+CCH*`) service or `AT+CHTTPS*` service on this variant. Both data upload and OTA download use the same `AT+HTTP*` flow.

```
┌─────────────────────────────────────────────────────────────┐
│                A7670E modem (internal)                       │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  HTTP(S) Service                                     │  │
│  │  AT+HTTPINIT                                         │  │
│  │  AT+HTTPPARA="URL","https://..."                     │  │
│  │  AT+HTTPPARA="SSLCFG",0  (bind SSL context 0)        │  │
│  │  AT+HTTPACTION=0 (GET) / AT+HTTPACTION=1 (POST)      │  │
│  │  +HTTPACTION: <method>,<status>,<datalen>  (URC)     │  │
│  │  AT+HTTPREAD=0,<len>  (read response body)           │  │
│  │  AT+HTTPTERM                                         │  │
│  │  SSL context 0 — configured once in Modem_Module_Init│  │
│  │  Used by: uploader (POST) AND OTA downloader (GET)   │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

**Upload POST flow (`a7670_https_uploader.c`):**

```
AT+HTTPPARA="URL","https://..."
AT+HTTPPARA="CONTENT","application/octet-stream"
AT+HTTPDATA=<size>,30
→ DOWNLOAD  (prompt; not '>')
→ <binary body>
→ OK
AT+HTTPACTION=1
← +HTTPACTION: 1,<status>,<datalen>  (URC)
AT+HTTPTERM
```

**Download GET flow (`a7670_ssl_downloader.c`):**

```
AT+HTTPINIT
AT+HTTPPARA="SSLCFG",0
AT+HTTPPARA="URL","https://.../get_firmware.php?offset=X&length=512"
AT+HTTPACTION=0
← +HTTPACTION: 0,200,<datalen>  (URC)
AT+HTTPREAD=0,512
← <binary chunk>
AT+HTTPTERM
```

**Initialisation sequence (`a7670.c` `Modem_Module_Init` — current after Phase 2.1):**

1. AT alive ping
2. `AT+CTZU=1` — enable NITZ automatic time-zone update
3. `AT+CNTP="server",28` + `AT+CNTP` (execute) — NTP sync
4. `AT+CCERTDOWN` × 3 — upload CA cert, client cert, client private key (**must be PEM format with `-----BEGIN`/`-----END` headers**)
5. `AT+CSSLCFG="sslversion",0,3` — TLS 1.2, SSL context 0
6. `AT+CSSLCFG="authmode",0,2` — mutual TLS, SSL context 0
7. `AT+CSSLCFG="cacert/clientcert/clientkey/sni",0,…` — bind certs to context 0
8. `at_channel_wait_ready()` → network registration (no `AT+CCHSTART`)

**Known issues still requiring resolution:**

| Issue | Location | Risk | Fix |
|-------|----------|------|-----|
| NTP confirm not awaited | `a7670.c` | TLS cert validity-window failure on cold boot | Capture `+CNTP:` URC after `AT+CNTP` execute; fail init if URC param ≠ 0 |
| Cert format ambiguity | cert array files | Silent cert corruption on modem FS | Verify `-----BEGIN` header in each cert array; rename `*_der.c` files to `*_pem.c` if confirmed PEM |

---

## 11. Image Integrity & Security

### 11.1 Per-Chunk CRC-32 (Transport Integrity)

Each 512-byte firmware chunk is transmitted with a 4-byte CRC-32/MPEG-2 trailer appended by the server.  `ssl_downloader_get_chunk()` validates this trailer before writing the chunk to FRAM; a mismatch returns `SSL_DL_ERR_CRC` and the OTA manager retries that chunk.

```c
/* shared/crc32.h — used by ssl_downloader_get_chunk() */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
```

> CRC-32/MPEG-2 (polynomial `0x04C11DB7`, initial `0xFFFFFFFF`) is computed in software (`shared/crc32.c`, 1 KB lookup table in Flash).  The STM32L476 hardware CRC unit is **not used** — `modbus_init()` reconfigures it for CRC-16/Modbus and it cannot be shared.

Reference: *STM32L476xx Reference Manual* RM0351, Section 14.

### 11.2 SHA-256 Verification (Whole-Image Integrity)

A standalone SHA-256 implementation (`shared/sha256.c` — FIPS 180-4, no mbedTLS dependency; all TLS runs on the A7670E modem) is accumulated **during download** as each chunk is received and written to FRAM.  The expected digest is obtained from the `H.<sha256hex>` field of the version-check response and stored in `OtaControlBlock_t.image_sha256`.

After all chunks are received, the OTA manager finalises the hash and compares it to `OCB.image_sha256`.  The bootloader re-computes the same SHA-256 over the FRAM staging area before programming Flash, using the same expected value from the OCB.

> **Whole-image CRC-32 has been removed.**  Per-chunk CRC-32 catches transport errors.  SHA-256 is the sole whole-image integrity check.  There is no `image_crc32` field in the OCB and no `FRAM_STAGING_CRC` footer.

### 11.3 RAM Budget for Integrity Check

| Buffer | Size | Allocation |
|--------|------|-----------|
| `s_chunk_read_buf` (chunk + CRC trailer) | 516 B | `static` in `a7670_ssl_downloader.c` |
| SHA-256 context (`sha256_ctx_t`) | 108 B | On stack in OTA manager / bootloader |
| SHA-256 digest output | 32 B | On stack in OTA manager / bootloader |
| Total static | 516 B | Within budget |

### 11.4 Firmware Signing (Phase 5 Enhancement)

For production, firmware images should be signed with ECDSA P-256. The bootloader holds the public key in write-protected Flash and verifies the signature before programming. This is out of scope for the initial release but the `OtaControlBlock_t` includes a `reserved` field that can accommodate a signature reference.

---

## 12. FreeRTOS Task Architecture

### 12.1 Task Table

| Task Name | Priority | Stack (words) | Wake Condition | Notes |
|-----------|----------|---------------|---------------|-------|
| `DataCollectionTask` | 3 (Normal) | 512 | RTC wakeup / sensor IRQ | Existing |
| `UploadTask` | 2 (BelowNormal) | 1024 | RTC twice-daily alarm | Existing; notifies OtaManagerTask on completion |
| `OtaManagerTask` | 2 (BelowNormal) | 1024 | `xTaskNotify` from UploadTask | **New** |
| `ModemTask` | 4 (AboveNormal) | 768 | Queue message | Existing; AT channel arbiter |
| `WatchdogTask` | 5 (High) | 256 | 500 ms `vTaskDelay` | **New**; per-task heartbeat + IWDG refresh |

**Total FreeRTOS stack commitment:** (512 + 1024 + 1024 + 768 + 256) × 4 = ~14.3 KB  
**Remaining for heap + static globals:** 96 KB − 14.3 KB − HAL/BSS ≈ 70 KB — within budget.

### 12.2 Inter-Task Communication

```
UploadTask ──(xTaskNotify)──────────► OtaManagerTask
                                            │
                        ┌───────────────────┤
                        │                   │
                (xQueueSend)        (FRAM SPI mutex)
                        │                   │
                        ▼                   ▼
                   ModemTask         ota_image_writer
                        │            (staging writes)
                   AT+HTTP*
                   AT+HTTPREAD=0,512
```

### 12.3 Mutex Strategy

| Mutex | Type | Guards |
|-------|------|--------|
| `g_fram_spi_mutex` | `xSemaphoreCreateMutex` (priority inheritance) | All SPI1 / FRAM transactions |
| `g_ota_state_mutex` | `xSemaphoreCreateMutex` | `OtaControlBlock_t` read-modify-write |

> **Acquisition order rule:** Always take `g_fram_spi_mutex` first, then `g_ota_state_mutex` if both are needed. Violating this order causes deadlock.

---

## 13. PlatformIO + CubeMX Project Structure

### 13.1 Root `platformio.ini`

```ini
; Root workspace — declares both environments
[platformio]
default_envs = application

[env:application]
extends         = env_common
build_src_filter = +<application/src/> +<shared/>
build_flags     = ${env_common.build_flags}
                  -DAPP_ENV
                  -DFW_VERSION=1
custom_ldscript = application/ldscript_app.ld

[env:bootloader]
extends         = env_common
build_src_filter = +<bootloader/src/> +<shared/>
build_flags     = ${env_common.build_flags}
                  -DBOOT_ENV
custom_ldscript = bootloader/ldscript_boot.ld

[env_common]
platform        = ststm32
board           = nucleo_l476rg
framework       = stm32cube
board_build.mcu = stm32l476rgt6
upload_protocol = stlink
debug_tool      = stlink
monitor_speed   = 115200
```

### 13.2 Application Linker Script (`ldscript_app.ld` — key regions)

```ld
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 480K   /* app code; 32 K reserved for bootloader */
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K    /* SRAM1 — hard limit */
  RAM2  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K    /* SRAM2 — OTA flags only */
}

/* Enforce code size at link time */
ASSERT(SIZEOF(.text) + SIZEOF(.rodata) <= 512K, "Code exceeds 512 KB limit");
ASSERT(SIZEOF(.data) + SIZEOF(.bss) <= 96K,     "Data exceeds 96 KB limit");
```

### 13.3 Bootloader Linker Script (`ldscript_boot.ld` — key regions)

```ld
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 16K   /* bootloader needs no FreeRTOS */
}
```

### 13.4 CubeMX Workflow

1. Open `Application.ioc` in CubeMX. Configure peripherals, clocks, FreeRTOS.
2. Click **Generate Code** → CubeMX writes to `application/CubeMX/Core/`.
3. PlatformIO's `build_src_filter` picks up generated files automatically.
4. **Never hand-edit CubeMX-generated files.** All custom code lives in `application/src/` or `shared/`.
5. Bootloader uses a separate `Bootloader.ioc` with only SPI1, IWDG, Flash — no FreeRTOS.

> CubeMX-generated files are **committed to version control**. This ensures the build is reproducible without requiring CubeMX to be installed on the CI machine.

### 13.5 Useful PlatformIO Commands

```bash
# Build application firmware
pio run -e application

# Build bootloader
pio run -e bootloader

# Flash application (requires ST-Link)
pio run -e application -t upload

# Flash bootloader
pio run -e bootloader -t upload

# Run unit tests (native environment)
pio test -e native

# Check memory usage
pio run -e application -t size

# Open serial monitor
pio device monitor -b 115200
```

---

## 14. Refactoring Plan & Phased Roadmap

### Phase 0 — Prerequisite Hardening ✅ COMPLETE (2026-04-16)

These tasks fix known stability issues that would cause silent OTA failures.

| Task | Status | Description | Gap |
|------|--------|-------------|-----|
| P0-1 | ✅ | Add `bootloader` env to root `platformio.ini`; add `FW_VERSION` build flag | G-10 |
| P0-2 | ✅ | Add `g_fram_spi_mutex` to all FRAM DB accesses in `nv_database.c` and `cy15b116qn.c` | G-9 |
| P0-3 | ✅ | Replace raw pointer returns in FRAM DB with snapshot-copy pattern | G-9 |
| P0-4 | ✅ | Fix ring-buffer boundary operator-precedence bugs in `nv_database.c` | G-9 |
| P0-5 | ✅ | Implement `Src/watchdog_task.c` with per-task heartbeat API and IWDG refresh | G-8 |
| P0-6 | ✅ | Create `shared/fram_addresses.h` with all FRAM address constants (new layout) | — |
| P0-7 | ✅ | Update `nv_database.c` and all callers to use `shared/fram_addresses.h` constants | — |
| P0-8 | ✅ | Build passes: RAM 20% (19,984 / 96 KB), Flash 9% (93,256 B) | — |

### Phase 1 — Bootloader ✅ CODE COMPLETE (2026-04-16; hardware tests pending)

| Task | Status | Description |
|------|--------|-------------|
| P1-1 | ✅ | Create `bootloader/` PlatformIO env; `ldscript_boot.ld` at 0x08000000 / 32 KB |
| P1-2 | ✅ | Create `bootloader/Inc/stm32l4xx_hal_conf.h` + `bootloader/Inc/main.h` (SPI1, IWDG, Flash HAL only); manual `MX_xxx_Init()` in `main.c` |
| P1-3 | ✅ | Implement `shared/ota_control_block.h/.c`: dual-copy OCB read/write/validate; CRC-32/MPEG-2; `BOOTLOADER_BUILD` ifdef selects `boot_fram` vs `cy15b116qn` backend |
| P1-4 | ✅ | Implement `bootloader/src/boot_fram.c`: polling SPI1, no DMA, no interrupts; WREN + READ + WRITE opcodes |
| P1-5 | ✅ | Implement `bootloader/src/boot_flash.c`: page erase Bank1 pages 16–255, 64-bit writes, read-back `memcmp` verify; IWDG refresh per page; D-cache reset before each `memcmp`; I-cache reset after all pages written |
| P1-6 | ✅ | Rollback: `ota_tried >= 3` → jump directly to existing app |
| P1-7 | ✅ | SP sanity check in `jump_to_application()`: SP must be in [0x20000000, 0x20018000) |
| P1-8 | ✅ | `ldscript_app.ld` created; both envs build: bootloader 5.7 KB Flash / 2.8 KB RAM |
| P1-9 | ⏳ | **Hardware test:** cold boot, no OTA pending → app boots at 0x08008000 |
| P1-10 | ⏳ | **Hardware test:** valid staging image → programmed, new app boots |
| P1-11 | ⏳ | **Hardware test:** corrupted staging (bad CRC) → old app retained |
| P1-12 | ⏳ | **Hardware test:** power-cycle mid-Flash-program → old app retained on next boot |

### Phase 2 — Download Infrastructure ✅ CODE COMPLETE (2026-04-17; hardware tests pending)

| Task | Status | Description |
|------|--------|-------------|
| P2-1 | ✅ | Implement `shared/crc32.c`: software table-based CRC-32/MPEG-2; `crc32_update()` accumulates in 512-byte chunks (HW CRC unit locked to CRC-16/Modbus by `modbus_init()`) |
| P2-2 | ✅ | Implement `lib/A7670/a7670_ssl_downloader.c`: chunked GET via `AT+HTTP*`; SSL context 0 via `AT+HTTPPARA="SSLCFG",0` |
| P2-3 | ✅ | Implement `Src/ota_image_writer.c`: FRAM write, download bitmap, resume scan |
| P2-4 | ✅ | `sha256.c/.h` moved to `shared/` (accessible to bootloader); SHA-256 accumulated during download per received chunk |
| P2-5 | ⏳ | **Hardware test:** download 400 KB binary; CRC + SHA-256 match expected values |
| P2-6 | ⏳ | **Hardware test:** interrupt download mid-way, power-cycle, resume → complete image verified |

### Phase 2.1 — Upload Migration to HTTP(S) Service ✅ COMPLETE (2026-04-18)

> Eliminated CCH service entirely. Both upload and OTA download now use `AT+HTTP*`. Blob capacity raised from 256 B to 512 B (28 records/batch, up from 12). Static RAM reduced by 32 B net.

| Task | Status | Description |
|------|--------|-------------|
| P2.1-1 | ✅ | Confirmed A7670E uses `AT+HTTP*` only (AT Command Manual V1.09 Chapter 16) |
| P2.1-2 | ✅ | Create `lib/A7670/a7670_https_uploader.h`: API + static buffer documentation (768 B total) |
| P2.1-3 | ✅ | Implement `lib/A7670/a7670_https_uploader.c`: `AT+HTTPDATA` → `DOWNLOAD` prompt → binary body → `AT+HTTPACTION=1` → `+HTTPACTION` URC |
| P2.1-4 | ✅ | No separate SSL context needed — context 0 shared by upload and download via `AT+HTTPPARA="SSLCFG",0` |
| P2.1-5 | ✅ | CCH removed from modem init: `AT+CCHSET=1` removed from `Modem_Module_Init()`; `AT+CCHSTART` removed from `at_channel_wait_ready()` |
| P2.1-6 | ✅ | `a7670_ssl_downloader.c/.h` rewritten for `AT+HTTP*` |
| P2.1-7 | ✅ | `Src/ssluploadtask.c` rewritten: `https_uploader_start/post/stop`; `MAX_RECORDS_PER_UPLOAD` raised to 28; pre-existing `DB_ToUploadwithOffset` index bug fixed |
| P2.1-8 | ✅ | `a7670_ssl_uploader.c/.h` deleted (no remaining callers) |
| P2.1-9 | ✅ | Build: app 92 KB Flash (18%), 24.2 KB RAM (25%) — within budgets |
| P2.1-10 | ⏳ | **Hardware test:** single-batch POST (≤ 512 B blob) via HTTPS → server returns 2xx |
| P2.1-11 | ⏳ | **Hardware test:** multi-batch upload loop → all batches confirmed; `DB_IncUploadTail` advances correctly |
| P2.1-12 | ⏳ | **Hardware test:** upload session → OTA version-check GET in same modem power-on succeeds |

### Phase 3 — OTA Manager Task (next)

| Task | Description |
|------|-------------|
| P3-1 | Implement `Src/ota_manager_task.c` full state machine (8 states) |
| P3-2 | Implement `GET <UPDATE_PATH>/` plain-text response parser (`V.#####:L.$$$$$$$` format) |
| P3-3 | Implement `server_version_is_newer()` comparison (`srv_version > FW_VERSION`) |
| P3-4 | Integrate `OtaManagerTask` into `MX_FREERTOS_Init()` in `Src/freertos.c` |
| P3-5 | Add `xTaskNotify` call in `ssluploadtask` to wake `OtaManagerTask` after upload |
| P3-6 | Implement `ota_confirm_success()` called at application startup after stable boot |
| P3-7 | Test: version check when server == device version → no download initiated |
| P3-8 | Test: version check when server > device version → full OTA flow |

### Phase 4 — Integration & Field Testing

| Task | Description |
|------|-------------|
| P4-1 | End-to-end test: upload → version check → download → OTA → confirm |
| P4-2 | Power-loss injection at each OTA state (download, verify, reboot, confirming) |
| P4-3 | 10 consecutive OTA cycles — verify DB ring buffer unaffected |
| P4-4 | `pio run -t size` — verify code ≤ 512 KB, data ≤ 96 KB |
| P4-5 | Enable RDP1 on bootloader Flash partition; document unlock procedure |
| P4-6 | Write factory programming procedure (bootloader first, then app) |
| P4-7 | Code review: mutex discipline, no dynamic allocation in OTA path |

---

## 15. Risk Register

| ID | Risk | Probability | Impact | Mitigation |
|----|------|-------------|--------|-----------|
| R-1 | Power loss during Flash programming bricks device | Medium | Critical | Bootloader keeps old image until all pages pass read-back; IWDG prevents infinite hang |
| R-2 | Code + rodata exceeds 512 KB budget | Medium | High | Linker `ASSERT` fails the build; monitor with `pio size` at each phase |
| R-3 | Data (BSS + heap + stack) exceeds 96 KB | Medium | High | Linker `ASSERT`; WatchdogTask detects stack overflow at runtime |
| R-4 | `AT+HTTPREAD` returns fewer bytes than requested — partial chunk | Low | High | CRC-32 + SHA-256 final check catches corruption; retry per-chunk on mismatch |
| R-5 | New firmware hangs before `ota_confirm_success()` | Medium | High | T_confirm = 60 s; IWDG fires → bootloader rollback |
| R-6 | FRAM config sector address mismatch between app and bootloader | Medium | Critical | Single `shared/fram_addresses.h` included by both environments |
| R-7 | SPI1 bus contention (DB reads racing OTA writes) | Medium | Medium | `g_fram_spi_mutex` serialises all access; OTA yields promptly |
| R-8 | Server returns metadata CRC that does not match image | Low | Medium | Locally computed CRC cross-checked against both server values |
| R-9 | OTA download time exceeds modem session timeout | Low | Medium | Resume bitmap allows re-connection and continuation |
| R-10 | CubeMX regeneration overwrites hand-edited HAL files | Medium | Medium | All custom code in `src/`; CubeMX only writes to `CubeMX/Core/` |
| R-11 | NTP sync not confirmed before TLS opens — clock at epoch 0 fails cert validity check | Medium | High | Await `+CNTP: 0` URC in `Modem_Module_Init()` before proceeding |
| R-12 | Cert arrays compiled as DER but `AT+CCERTDOWN` requires PEM — silent FS corruption | Medium | Critical | Verify `-----BEGIN` header in each cert array; rename `*_der.c` files to `*_pem.c` if confirmed PEM |
| ~~R-13~~ | ~~CCH service still running when OTA downloader calls `AT+CHTTPSSTART`~~ | — | — | **RESOLVED (Phase 2.1):** CCH service eliminated entirely; both upload and download use `AT+HTTP*` |
| ~~R-14~~ | ~~`AT+CCHRECV` 64-byte capture truncated~~ | — | — | **RESOLVED (Phase 2.1):** `a7670_ssl_uploader.c` deleted; CCH no longer used |
| R-15 | I-cache/D-cache stale data after Flash programming — D-cache returns pre-erase data to read-back `memcmp`; I-cache serves old app instructions after jump | High | Critical | Reset D-cache before each `memcmp` verify; reset I-cache after all pages written; see §9.4 |

---

## 16. Open Questions & Decisions Required

| # | Question | Options | Recommendation |
|---|----------|---------|---------------|
| Q-1 | Dual-bank Flash swap or copy-in-place? | Dual-bank atomic swap vs copy-in-place with old-image retention | Copy-in-place for v1 (simpler); dual-bank as Phase 5 |
| Q-2 | Firmware image signing for v1? | ECDSA P-256 vs CRC-32 + SHA-256 only | CRC-32 + SHA-256 for v1; signing in Phase 5 |
| Q-3 | OTA download in background or pause data collection? | Pause vs background with mutex | Background; OTA yields SPI mutex between chunks |
| Q-4 | Confirmation window duration? | 30 / 60 / 120 seconds | 60 seconds |
| Q-5 | TLS server authentication? | None / CA validation / certificate pinning | CA validation for v1; pinning for production |
| Q-6 | ~~JSON parser library or hand-rolled?~~ | N/A — protocol uses plain text, no JSON | Plain text `sscanf`-style parse; no parser library needed |
| Q-7 | ~~SHA-256 source?~~ | **Resolved** — mbedTLS is NOT present; A7670E TLS runs entirely on the modem processor, not the STM32 | Use standalone `sha256.c` port (no mbedTLS dependency) |
| Q-8 | OTA check frequency? | Twice daily (with upload) vs separate schedule | Twice daily, same session as upload to save modem wake cost |

---

## 17. References

1. STMicroelectronics. *STM32L476xx Reference Manual* RM0351 Rev 9. Sections 3.3 (Flash), 3.4 (Flash protection), 14 (CRC unit). https://www.st.com/resource/en/reference_manual/rm0351-stm32l47xxx-stm32l48xxx-stm32l49xxx-and-stm32l4axxx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf

2. STMicroelectronics. *STM32L476RG Datasheet*. DS10199 Rev 7. https://www.st.com/resource/en/datasheet/stm32l476rg.pdf

3. Cypress / Infineon. *CY15B116QN Datasheet* (16-Mbit (2M × 8) Serial (SPI) F-RAM). Doc. 001-99272. https://www.infineon.com/dgdl/Infineon-CY15B116QN-DataSheet-v17_00-EN.pdf

4. SIMCom. *A7670 Series AT Command Manual* V1.xx. Section 12 (SSL), Section 13 (HTTP). Available from SIMCom partner portal.

5. FreeRTOS. *FreeRTOS Reference Manual* — Task Notifications, Mutexes with Priority Inheritance. https://www.freertos.org/Documentation/RTOS_book.html

6. ARM. *Cortex-M4 Devices Generic User Guide*. Section 2.3 (Memory map). https://developer.arm.com/documentation/dui0553/latest

7. STMicroelectronics. *AN4657 — STM32 in-application programming (IAP) using the USART*. https://www.st.com/resource/en/application_note/an4657-stm32-incircuit-programming-over-usart-stmicroelectronics.pdf

8. PlatformIO. *PlatformIO Documentation — STM32Cube framework*. https://docs.platformio.org/en/stable/frameworks/stm32cube.html

9. IETF. *RFC 7233 — Hypertext Transfer Protocol (HTTP/1.1): Range Requests*. Used for resume-capable chunked download design. https://www.rfc-editor.org/rfc/rfc7233

---

*Document Version 1.5 — prepared for Claude Code consumption*  
*Target audience: Firmware engineering team / Claude Code AI agent*  
*Next review: After Phase 3 completion*
