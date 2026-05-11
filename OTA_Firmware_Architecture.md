# OTA Firmware Update Architecture

STM32L476RG + CY15B116QN FRAM + A7670E LTE modem · FreeRTOS · PlatformIO + STM32CubeMX.
As-built reference. Phase status in `IMPLEMENTATION_STATUS.md`. Server side in `Server_Architecture.md`.

---

## 1. System Overview

### 1.1 Hardware

| Component | Part | Interface | Notes |
|-----------|------|-----------|-------|
| MCU       | STM32L476RG | — | Cortex-M4F, 1 MB Flash, 128 KB SRAM (SRAM1 96 KB + SRAM2 32 KB) |
| FRAM      | CY15B116QN  | SPI1 | 2 MB, byte-addressable, unlimited endurance |
| Modem     | A7670E      | USART3 | TLS 1.2, `AT+HTTP*` (no `AT+CCH*` / no `AT+CHTTPS*` on this variant) |

### 1.2 Size budget

| Region | Limit | Enforcement |
|--------|-------|-------------|
| App code (`.text` + `.rodata`) | ≤ 480 KB (Flash partition) | Linker `ASSERT`, `scripts/monitor_ram.sh` |
| App data (`.data` + `.bss`)    | ≤ 96 KB (SRAM1)            | Linker `ASSERT`, `scripts/monitor_ram.sh` |
| SRAM2 (32 KB)                  | OTA state flags only        | Not counted in 96 KB |
| Bootloader Flash               | 32 KB                       | `bootloader/ldscript_boot.ld` |
| OTA static download buffer     | 740 B                       | `s_chunk_read_buf[516]` + `s_cmd_buf[224]` |

### 1.3 End-to-end OTA flow

```
RTC alarm (noon / midnight)
  → ssluploadtask: POST batched FRAM records via AT+HTTP* (HTTPS POST)
  → xTaskNotify → OtaManagerTask
       │
       ▼
   GET <UPDATE_PATH>/?id=rrrsss
       → "V.#####:L.$$$$$$$:H.<sha256>:W.<sec>"
       │
       │  V > FW_VERSION  AND  W == 0  AND  L ≤ FLASH_APP_SIZE_MAX (480 KB)
       ▼
   GET <UPDATE_PATH>/get_firmware?offset=&length=&id=rrrsss  (loop, 512 B chunks)
       → write FRAM staging, update download bitmap, accumulate SHA-256
       │
       ▼ SHA-256 match
   ocb_write({ota_pending=1, image_size, image_sha256, fw_version})  (dual-copy)
       │
       ▼ HAL_NVIC_SystemReset
   Bootloader: validate OCB, size guard, SHA-256, erase+program pages 16–255, verify, jump 0x08008000
       │
       ▼ new app boots
   ota_confirm_success() within 60 s, else IWDG → bootloader retains old app
```

---

## 2. Internal Flash Layout

```
0x08000000  Bootloader  (32 KB, pages 0–15, Bank 1)   RDP1 + write-protected
0x08008000  Application (480 KB, pages 16–255, Bank 1) ldscript_app.ld origin
0x08080000  Bank 2      (512 KB — reserved for future dual-bank)
```

Linker `MEMORY` (application):

```
FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 480K
RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 96K   /* SRAM1 — hard limit */
RAM2  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K   /* SRAM2 — OTA flags only */
```

---

## 3. FRAM Layout

Single source of truth: `shared/fram_addresses.h`. Do not hardcode addresses.

### 3.1 DB Region (first 1 MB, 0x000000 – 0x0FFFFF)

| Sub-region | Start | Size | Description |
|------------|-------|------|-------------|
| DB ring buffer | 0x000000 | ~1,016 KB | Sensor records, ring buffer (`nv_database.c`) |
| Config sector  | 0x0FE000 | 4 KB      | System config + OTA Control Block (dual-copy) |
| Reserved       | 0x0FF000 | 4 KB      | Future use |

### 3.2 Config sector @ 0x0FE000 (4 KB)

| Offset | Size | Field |
|--------|------|-------|
| +0x0000 | 128 B | System config (baud rates, server URL, `Meta_Data_t`) |
| +0x0080 | 64 B  | OTA Control Block — primary |
| +0x00C0 | 64 B  | OTA Control Block — mirror |
| +0x0100 | —     | Reserved |

### 3.3 OTA Staging Region (second 1 MB, 0x100000 – 0x1FFFFF)

| Sub-region | Start | Size | Description |
|------------|-------|------|-------------|
| Staging header   | 0x100000 | 256 B    | Mirrors OCB + in-progress URL/size |
| Image data       | 0x100100 | ≤ 480 KB | Raw `.bin`. Hard cap = app Flash partition; bootloader cannot program past page 255. |
| Download bitmap  | 0x17F000 | 1 KB     | 1 bit per 512-byte chunk; set = received |

Bitmap capacity: 8,192 chunks × 512 B = 4 MB (overcommitted vs 480 KB cap). On reconnect, scan bitmap and request only missing chunks.

### 3.4 OtaControlBlock_t

```c
/* shared/ota_control_block.h — compiled into both app and bootloader */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* 0x0AC0FFEE */
    uint8_t  ota_pending;        /* 0x01 = image ready, 0x00 = idle */
    uint8_t  ota_tried;          /* incremented each boot attempt; max 3 */
    uint8_t  ota_confirmed;      /* 0x01 = new app called ota_confirm_success() */
    uint8_t  pad0;
    uint32_t image_size;
    uint8_t  image_sha256[32];   /* sole whole-image integrity check */
    uint32_t fw_version;
    uint8_t  reserved[8];
    uint32_t download_timestamp; /* Y2K epoch */
    uint32_t block_crc32;        /* CRC-32/MPEG-2 over bytes [0..59] */
} OtaControlBlock_t;             /* 64 B */
```

Dual-copy procedure: write primary → verify CRC → write mirror → verify CRC. On read, accept the copy whose `block_crc32` validates; if neither, treat as no pending OTA.

---

## 4. OTA State Machine

State persisted in `OtaControlBlock_t`. Survives power loss.

```
IDLE
  │ RTC alarm (after upload, via xTaskNotify)
  ▼
POLLING_VERSION  ── server V ≤ FW_VERSION ──▶ IDLE
  │                ── L > FLASH_APP_SIZE_MAX ─▶ IDLE  (no retry)
  │                ── W > 0                  ─▶ IDLE  (retry next cycle)
  ▼
DOWNLOADING  ◀── resume on power loss (scan download bitmap)
  │ bitmap full
  ▼
DOWNLOAD_COMPLETE
  │ per-chunk CRC ✓  AND  SHA-256 ✓
  ▼
VERIFIED   → ocb_write(ota_pending=1) → graceful shutdown → flush DB
  │
  ▼
REBOOT_PENDING → HAL_NVIC_SystemReset()
  │ bootloader programs + jumps
  ▼
CONFIRMING (new app)
  │ ota_confirm_success() within 60 s   else IWDG → bootloader rollback
  ▼
IDLE  (ota_confirmed=1, ota_pending=0)

Integrity FAIL          → staging invalidated, old FW retained
ota_tried >= 3          → bootloader boots old Flash unchanged
HTTP 429 (out of slot)  → treated as network error; retry next cycle
```

---

## 5. Bootloader

Bare-metal (no FreeRTOS). SPI1 (polling), IWDG, Flash HAL only. Linked at `0x08000000` / 32 KB. IWDG set to ≈ 4 s, refreshed each page.

### 5.1 Boot decision

1. Read OCB primary + mirror; accept the copy with valid CRC.
2. If `ota_pending == 0x01` and `ota_tried < 3`:
   - Increment `ota_tried`; rewrite both copies.
   - Reject `image_size == 0` or `image_size > FLASH_APP_SIZE_MAX` (480 KB) → jump to old app.
   - Validate SHA-256 over `image_size` bytes of FRAM staging against `image_sha256`. **No whole-image CRC**; SHA-256 is sole integrity check.
   - Program pages 16–255 (erase → 64-bit write → read-back `memcmp`).
   - All pages verified: clear `ota_pending`, set `ota_confirmed = 0`, jump.
   - Any failure: clear `ota_pending`, set error, jump to old app.
3. Else: jump directly to `0x08008000`.

Stack-pointer sanity check before jump: SP must be in `[0x20000000, 0x20018000)`.

### 5.2 Cache management (critical)

STM32L476 ART Accelerator has read-only Flash I-cache + D-cache (`FLASH_ACR` `ICEN`/`DCEN`). Both enabled by default. Two failure modes:

| Failure | Cause | Fix |
|---------|-------|-----|
| Stale `memcmp` after page program | D-cache holds pre-erase data | Reset D-cache before each `memcmp` |
| Old instructions execute after jump | I-cache holds previous app code | Reset I-cache after all pages programmed |

Required HAL sequence per page (after `HAL_FLASH_Lock`, before `memcmp`):

```c
__HAL_FLASH_DATA_CACHE_DISABLE();
__HAL_FLASH_DATA_CACHE_RESET();
__HAL_FLASH_DATA_CACHE_ENABLE();
```

After all pages programmed (before return):

```c
__HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
__HAL_FLASH_INSTRUCTION_CACHE_RESET();
__HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
```

`HAL_FLASHEx_Erase` does **not** invalidate D-cache on all HAL versions — always do it explicitly.

### 5.3 Security

- RDP Level 1 + WRP on pages 0–15 (after production programming).
- Ed25519 image signing: deferred to a future phase. Server-side signing is wired (S10-4 — `v{n}.sig` produced); bootloader verification not yet implemented.

---

## 6. OTA Protocol

### 6.1 Base URL

`UPDATE_PATH` is stored in `Meta_Data_t.update_path` (64 B, Config Sector). Set via CDC `set config`. Server mounts all device traffic under the same base path (Q-S1 Option B): `update_path = "/api/v1/weather"`. Concrete URLs:

- `POST <server>/api/v1/weather/upload` — ingest
- `GET  <server>/api/v1/weather/?id=rrrsss` — OTA version + wait token
- `GET  <server>/api/v1/weather/get_firmware?offset=X&length=512&id=rrrsss` — OTA chunk

### 6.2 Device identity

`?id=` is a 6-char decimal `{region:03d}{station:03d}` built from `Meta_Data_t.region_id`/`.station_id`. Both fields constrained to 0–999; firmware applies `% 1000` before formatting `%03u%03u` (Q-S7).

### 6.3 Endpoint 1 — Version + size

```
GET <UPDATE_PATH>/?id=rrrsss
```

Response: HTML body containing a plain-text token (locate via regex, not byte 0):

```
V.\d+:L.\d+:H.[0-9a-f]{64}(?::W\.\d+)?
```

- `V` — uint32 server firmware version
- `L` — image size, bytes
- `H` — SHA-256, 64 lowercase hex chars
- `W` — optional rollout wait (seconds). Missing or `W.0` = download permitted. `W>0` = skip this cycle, retry next poll.

Parser action:
- No match → "no update", silent skip, no retry.
- `V ≤ FW_VERSION` → no update.
- `L > FLASH_APP_SIZE_MAX (480 KB)` → reject, return to IDLE (no retry).
- `W > 0` → return to IDLE.
- Retries: up to 3 on modem/network errors; 0 on parseable non-matching body.

### 6.4 Endpoint 2 — Chunk download

```
GET <UPDATE_PATH>/get_firmware?offset=N&length=512&id=rrrsss
```

Response body = `<length>` bytes of image + 4-byte little-endian CRC-32/MPEG-2 trailer. Total = `length + 4`. Device reads via `AT+HTTPREAD=0,516`.

| Server response | Device action |
|-----------------|---------------|
| `200` + chunk + CRC | Validate CRC, write to FRAM staging, set bitmap bit, update SHA-256 |
| `429 Too Many Requests` | Server re-evaluated rollout slot; treat as network error; back off, retry |
| Other / CRC mismatch | Retry up to 3× per chunk |

### 6.5 Download loop (current `ota_manager_task.c`)

```c
/* Phase 1 — fetch metadata */
snprintf(version_url, sizeof version_url, "%s/?id=%03u%03u",
         update_path, region_id % 1000, station_id % 1000);
if (parse_response(buf, &v, &l, sha_expected, &wait_sec) != OK) return IDLE;
if (v <= FW_VERSION) return IDLE;
if (l > FLASH_APP_SIZE_MAX) return IDLE;
if (wait_sec > 0) return IDLE;

/* Phase 2 — chunked download with resume */
uint32_t off = oiw_resume_info(&next_chunk) ? next_chunk * CHUNK_SIZE : 0;
sha256_init(&ctx);  /* re-hash already-received chunks from FRAM if resuming */
while (off < l) {
    uint32_t req = MIN(CHUNK_SIZE, l - off);
    snprintf(url, sizeof url, "%s/get_firmware?offset=%lu&length=%lu&id=%03u%03u",
             update_path, off, req, region_id % 1000, station_id % 1000);
    if (ssl_downloader_get_chunk(url, buf, CHUNK_SIZE, &got) != OK) {
        if (++retries >= 3) abort;
        continue;
    }
    oiw_write_chunk(off / CHUNK_SIZE, buf, got);
    sha256_update(&ctx, buf, got);
    off += got;
    HAL_IWDG_Refresh(&hiwdg);
}
sha256_final(&ctx, digest);
if (memcmp(digest, sha_expected, 32) != 0) { invalidate(); return IDLE; }
ocb_write(...);  /* dual-copy; ota_pending = 1 */
```

---

## 7. A7670E Modem Service

All TLS crypto runs on the modem. STM32 pays zero crypto cost.

**Service:** A7670E exposes only `AT+HTTP*` (no `AT+CCH*`, no `AT+CHTTPS*`). One HTTP session at a time. Both upload (POST) and OTA download (GET) use the same flow.

### 7.1 Upload POST (`a7670_https_uploader.c`)

```
AT+HTTPINIT
AT+HTTPPARA="URL",...
AT+HTTPPARA="CONTENT","application/octet-stream"
AT+HTTPPARA="SSLCFG",0
AT+HTTPDATA=<size>,30
  ← DOWNLOAD       (prompt — not '>')
  → <binary body>
  ← OK
AT+HTTPACTION=1
  ← +HTTPACTION: 1,<status>,<datalen>   (URC)
AT+HTTPTERM
```

### 7.2 Download GET (`a7670_ssl_downloader.c`)

```
AT+HTTPINIT
AT+HTTPPARA="SSLCFG",0
AT+HTTPPARA="URL",".../get_firmware?offset=X&length=512&id=rrrsss"
AT+HTTPACTION=0
  ← +HTTPACTION: 0,<status>,<datalen>   (URC)
AT+HTTPREAD=0,516
  ← <chunk bytes + 4-byte CRC>
AT+HTTPTERM
```

### 7.3 Modem init (`Modem_Module_Init` in `a7670.c`)

1. AT alive ping
2. `AT+CTZU=1` — NITZ time-zone update
3. `AT+CNTP="server",28` then `at_channel_send_cntp(12000)` — suppresses immediate OK; AT_OK only on `+CNTP: 0` URC, AT_ERROR on non-zero err (12 s timeout)
4. `AT+CCERTDOWN` × 3 — upload CA cert, fleet client cert, fleet private key (see R-12)
5. `AT+CSSLCFG="sslversion",0,3` — TLS 1.2 on SSL context 0
6. `AT+CSSLCFG="authmode",0,2` — mutual TLS on SSL context 0
7. `AT+CSSLCFG="cacert/clientcert/clientkey/sni",0,…` — bind certs
8. `at_channel_wait_ready()` — network registration

**Cert format (R-12).** `AT+CCERTDOWN` expects **DER binary** on this variant — no `-----BEGIN` headers. Convert from PEM with `openssl x509 -outform DER` (certs) and `openssl rsa -outform DER` (keys), then embed as C byte arrays. `*_der.c` filenames are accurate; verify the array contents are DER before each fleet deployment.

---

## 8. Integrity

| Layer | Algorithm | Implementation | Purpose |
|-------|-----------|----------------|---------|
| Per-chunk | CRC-32/MPEG-2 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection, no XOR) | `shared/crc32.c` (1 KB lookup table, Flash) | Catches UART bit-flips; mismatch → `SSL_DL_ERR_CRC` → retry chunk |
| Whole-image | SHA-256 (FIPS 180-4) | `shared/sha256.c` (standalone, no mbedTLS) | Accumulated during download. App verifies vs `H.` from metadata before OCB commit. Bootloader re-verifies before Flash program. |

HW CRC unit on the STM32L476 is **not used** — `modbus_init()` configures it for CRC-16/Modbus and it cannot be shared. CRC-32/MPEG-2 is software-only.

There is no whole-image CRC. SHA-256 is the sole whole-image check.

RAM cost: `s_chunk_read_buf[516]` static (downloader) + 108 B SHA context on stack + 32 B digest = under 1 KB total.

---

## 9. FreeRTOS Tasks

All tasks created in `Src/freertos.c → MX_FREERTOS_Init()`. Every task must call the heartbeat API within 500 ms.

| Task | File | Period / wake | Priority | Role |
|------|------|---------------|----------|------|
| `UsbLoopTask`     | `freertos.c`      | event       | High   | TinyUSB device loop (`tud_task`) |
| `cdc_task`        | `cdctask.c`       | event       | High   | USB CDC binary protocol |
| `maintask`        | `maintask.c`      | 1 s         | Normal | Sensor read → FRAM + SD |
| `uitask`          | `uitask.c`        | 20 ms       | Normal | LCD refresh, LEDs, button debounce |
| `ucctask`         | `ucctask.c`       | 100 ms      | Normal | LCD menu / user interaction |
| `ssluploadtask`   | `ssluploadtask.c` | noon + midnight | Normal | Batched HTTPS upload; `xTaskNotify` to OtaManagerTask |
| `OtaManagerTask`  | `ota_manager_task.c` | after upload | Normal | OTA state machine |
| `WatchdogTask`    | `watchdog_task.c` | 500 ms      | High   | Per-task heartbeat + IWDG refresh |

**Mutex order (deadlock guard):** `g_fram_spi_mutex` always taken before `g_ota_state_mutex`. Never reverse.

---

## 10. Repository Structure

```
project_root/
├── platformio.ini              ; two envs: application + bootloader
├── Src/                        ; app source (CubeMX + USER CODE guards)
│   ├── freertos.c              ; task creation
│   ├── maintask.c              ; sensor 1 s loop → FRAM + SD
│   ├── ssluploadtask.c         ; HTTPS upload via AT+HTTP*
│   ├── ota_manager_task.{c,h}  ; OTA state machine
│   ├── ota_image_writer.{c,h}  ; chunked FRAM write + bitmap
│   ├── watchdog_task.{c,h}     ; heartbeat + IWDG
│   ├── ui/cdc/uart tasks
│   └── *.c (CubeMX peripherals — gpio/i2c/spi/usart/rtc/sdmmc/usb_otg/dma)
├── Inc/                        ; app headers
├── lib/
│   ├── sensors/                ; bmp390, sht45 (I2C), modbus, rain_light (RS-485)
│   ├── A7670/                  ; a7670.c, a7670_at_channel.c, a7670_https_uploader.c, a7670_ssl_downloader.c, a7670_ssl_cert_manager.c
│   ├── SPI_FRAM/               ; cy15b116qn.c (raw SPI), nv_database.c (ring DB)
│   ├── user_interface/         ; mcp23017.c, ui.c
│   ├── usart_subsystem/        ; uart_subsystem.c (shared interrupt-driven UART)
│   ├── time/                   ; datetime.c, y2k_time.c
│   ├── tinyusb/                ; vendored TinyUSB
│   └── utils/                  ; weather_data.h, fixedptc.h
├── shared/                     ; compiled into BOTH app and bootloader
│   ├── fram_addresses.h        ; single source for all FRAM addresses + FLASH_APP_SIZE_MAX
│   ├── ota_control_block.{c,h} ; dual-copy OCB read/write/validate
│   ├── crc32.{c,h}             ; CRC-32/MPEG-2
│   └── sha256.{c,h}            ; FIPS 180-4
└── bootloader/                 ; separate PIO env, bare-metal
    ├── ldscript_boot.ld        ; 0x08000000 / 32 KB
    ├── Inc/                    ; main.h, stm32l4xx_hal_conf.h (SPI1+IWDG+Flash only)
    └── src/                    ; main.c, boot_flash.{c,h}, boot_fram.{c,h}
```

---

## 11. Design Decisions

| # | Decision |
|---|----------|
| Q-1 | Copy-in-place: bootloader programs FRAM staging → Bank 1 pages 16–255. Dual-bank atomic swap deferred. |
| Q-2 | Per-chunk CRC-32/MPEG-2 + whole-image SHA-256. Ed25519 signing deferred (server S10-4 emits `v{n}.sig` already). |
| Q-3 | Background download: `OtaManagerTask` yields `g_fram_spi_mutex` between chunks; sensor + upload continue uninterrupted. |
| Q-4 | 60 s confirm window: new app calls `ota_confirm_success()`; otherwise IWDG → bootloader rollback. |
| Q-5 | Private-CA mTLS (Server_Architecture.md §2.1). Cert pinning deferred. |
| Q-6 | Plain-text protocol `V.#####:L.$$$$$$$:H.<sha256>:W.<seconds>`. No JSON parser. |
| Q-7 | Standalone `shared/sha256.c`. mbedTLS not present on STM32; TLS lives on modem. |
| Q-8 | Twice daily: `OtaManagerTask` notified by `ssluploadtask` after each upload session. |

---

## 12. Risk Register

Only open risks listed. All resolved risks (R-1..R-11, R-13..R-15) removed.

| ID | Risk | Probability | Impact | Mitigation |
|----|------|-------------|--------|-----------|
| R-12 | Cert arrays compiled as DER but `*_der.c` may contain PEM bytes — silent FS corruption | Medium | Critical | Verify each `*_der.c` array has no `-----BEGIN` header before next fleet build; regenerate via `openssl x509/rsa -outform DER` if needed |

---

## 13. References

- STM32L476xx Reference Manual RM0351 — Sections 3.3 (Flash), 3.4 (Flash protection), 14 (CRC unit).
- STM32L476RG Datasheet DS10199.
- CY15B116QN Datasheet (Doc 001-99272, Infineon).
- A7670 Series AT Command Manual V1.09 — Chapter 16 (HTTP). Local: `https_manual.md`.
- NTP usage: local `ntp_manual.md`.
- FreeRTOS Reference Manual — Task Notifications, Mutexes with Priority Inheritance.
- ARM Cortex-M4 Devices Generic User Guide.
- PlatformIO — STM32Cube framework.
