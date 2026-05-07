# OTA Phased Implementation Status

Implement phases **strictly in order**. Each phase must pass all listed tests before the next begins.

---

## Phase 0 — Prerequisite Hardening

- [x] P0-1 Add `bootloader` environment to root `platformio.ini`; add `FW_VERSION` build flag to application env
- [x] P0-2 Add `g_fram_spi_mutex` to all FRAM accesses in `lib/SPI_FRAM/nv_database.c` and `cy15b116qn.c`
- [x] P0-3 Replace raw pointer returns in `nv_database.c` with snapshot-copy pattern
- [x] P0-4 Fix ring-buffer boundary operator-precedence bugs in `nv_database.c`
- [x] P0-5 Implement `Src/watchdog_task.c` with per-task heartbeat API and IWDG refresh
- [x] P0-6 Create `shared/fram_addresses.h` with all FRAM address constants (new layout)
- [x] P0-7 Update `nv_database.c` and all callers to use `shared/fram_addresses.h` constants
- [x] P0-8 Verify `scripts/monitor_ram.sh` passes — build ✓, RAM 20% (19,984/96 KB), Flash 9% (93,256 B)

---

## Phase 1 — Bootloader

- [x] P1-1 Create `bootloader/` PlatformIO env; `ldscript_boot.ld` at 0x08000000/32 KB
- [x] P1-2 Create `bootloader/Inc/stm32l4xx_hal_conf.h` + `bootloader/Inc/main.h` (SPI1, IWDG, Flash HAL only — no FreeRTOS); manual `MX_xxx_Init()` in `main.c`
- [x] P1-3 Implement `shared/ota_control_block.h/.c` (dual-copy OCB read/write/validate; CRC-32/MPEG-2; `BOOTLOADER_BUILD` ifdef selects boot_fram vs cy15b116qn backend)
- [x] P1-4 Implement `bootloader/src/boot_fram.h/.c` (polling SPI1, no DMA; WREN + READ + WRITE opcodes)
- [x] P1-5 Implement `bootloader/src/boot_flash.h/.c` (page erase Bank1 pages 16–255, 64-bit double-word write, read-back memcmp verify; IWDG refresh per page; D-cache reset before each `memcmp`; I-cache reset after all pages written)
- [x] P1-6 Rollback in `bootloader/src/main.c`: `ota_tried >= 3` → jump directly to existing app
- [x] P1-7 Stack-pointer sanity check in `jump_to_application()`: SP must be in [0x20000000, 0x20018000)
- [x] P1-8 `ldscript_app.ld` created: FLASH origin 0x08008000/480 KB; RAM 96 KB; SRAM2 32 KB section; `ASSERT` code ≤ 480 KB and data ≤ 96 KB; `lib_ldf_mode = off` in bootloader env; `-I Inc` moved to app build_flags — build ✓ both envs: bootloader 5.7 KB Flash / 2.8 KB RAM; app 20% RAM / 9% Flash
- [ ] P1-9 Test: cold boot, no OTA pending → app boots at 0x08008000 ✓
- [ ] P1-10 Test: valid staging image → programmed, new app boots ✓
- [ ] P1-11 Test: corrupted staging (bad CRC) → old app retained ✓
- [ ] P1-12 Test: power-cycle mid-Flash-program → old app retained on next boot ✓

---

## Phase 2 — Download Infrastructure

- [x] P2-1 Implement `shared/crc32.h/.c`: software table-based CRC-32/MPEG-2; `crc32_update(crc, data, len)` accumulates in 512-byte chunks; HW CRC not used (locked to CRC-16/Modbus)
- [x] P2-2 Implement `lib/A7670/a7670_ssl_downloader.c`: chunked GET via `AT+HTTP*` service; rewritten 2026-04-17
- [x] P2-3 Implement `Src/ota_image_writer.c`: FRAM write, download bitmap, resume scan
- [x] P2-4 Integrate SHA-256 accumulation via standalone `shared/sha256.c`
- [ ] P2-5 Test: download 400 KB binary; CRC + SHA-256 match expected values ✓
- [ ] P2-6 Test: interrupt download mid-way, power-cycle, resume → complete image verified ✓

---

## Phase 2.1 — Upload Migration to HTTPS Service

> CCH (`AT+CCH*`) eliminated. Both upload and download use `AT+HTTP*`.
> POST flow: `AT+HTTPDATA=<size>,30` → `DOWNLOAD` prompt → binary body → `OK` → `AT+HTTPACTION=1` → `+HTTPACTION` URC.
> Blob size raised from 256 B to 512 B; max records per batch: (512−5)/18 = 28.
> Static RAM: 768 B (down 32 B from 800 B).

- [x] P2.1-1 Confirmed A76XX AT Command Manual V1.09 Ch.16: A7670E uses `AT+HTTP*` (not `AT+CHTTPS*`)
- [x] P2.1-2 Create `lib/A7670/a7670_https_uploader.h`: `HTTPS_UL_FETCH_WINDOW 512u`; public API `https_uploader_start/post/stop`
- [x] P2.1-3 Implement `lib/A7670/a7670_https_uploader.c`: HTTPINIT → SSLCFG → URL → CONTENT → HTTPDATA → binary → HTTPACTION=1 → URC
- [x] P2.1-4 SSL context 0 shared — `AT+HTTPPARA="SSLCFG",0` used by both uploader and downloader; no separate config needed
- [x] P2.1-5 CCH removed from modem init: `AT+CCHSET=1` removed from `Modem_Module_Init()`; `AT+CCHSTART` removed from `at_channel_wait_ready()`
- [x] P2.1-6 `a7670_ssl_downloader.c/.h` rewritten for `AT+HTTP*`; `at_channel_http_read()` replaces `at_channel_recv_https_chunk()`
- [x] P2.1-7 Rewrite `Src/ssluploadtask.c`: `https_uploader_start/post/stop`; `MAX_RECORDS_PER_UPLOAD` raised to 28; fixed `DB_ToUploadwithOffset` call (was passing byte offset, now record index)
- [x] P2.1-8 Deleted `lib/A7670/a7670_ssl_uploader.c/.h`
- [x] P2.1-9 Build verification: app 92 KB Flash (18%), 24.2 KB RAM (25%) ✓
- [ ] P2.1-10 Test: single-batch POST (≤ 512 B blob) via HTTPS → server returns 2xx ✓
- [ ] P2.1-11 Test: multi-batch upload → all batches confirmed; `DB_IncUploadTail` advances correctly ✓
- [ ] P2.1-12 Test: upload session followed by OTA version-check GET in same modem power-on ✓

---

## Phase 3 — OTA Manager Task

- [x] P3-1 Implement `Src/ota_manager_task.c` full state machine (8 states)
- [x] P3-2 Implement version response scanner: locate `V.\d+:L.\d+:H.[0-9a-f]{64}` in buffer; extract version, image_size, sha256
- [x] P3-3 Implement `server_version_is_newer()`: `srv_version > FW_VERSION`
- [x] P3-4 Add `OtaManagerTask` to `MX_FREERTOS_Init()` in `Src/freertos.c`
- [x] P3-5 Add `xTaskNotify` in `ssluploadtask` to wake `OtaManagerTask` after upload
- [x] P3-6 Implement `ota_confirm_success()`; added `build_src_filter = +<*> +<../shared/*>` to app env — build ✓: app 22.7% RAM / 9.3% Flash
- [x] P3-6.1 Fix R-11 (NTP URC never awaited): add `at_channel_send_cntp(12000u)` to AT channel; suppresses immediate OK, signals AT_OK only on `+CNTP: 0` URC and AT_ERROR on any non-zero err code; modem init in `Modem_Module_Init()` now fails fast if NTP sync fails — build ✓ both envs
- [ ] P3-7 Test: server version == device version → no download initiated ✓
- [ ] P3-8 Test: server version > device version → full OTA state machine completes ✓

---

## Phase 3.1 — Firmware Protocol Update (Rollout Gate + Device Identity)

> Server-side rollout mechanism introduced: 20-slot schedule with `W.<seconds>` wait times.
> Device must send `id` parameter on both metadata and download endpoints.
> Protocol now: `V.XXXXX:L.YYYYYYY:H.<sha256>:W.<seconds>`

- [x] P3.1-1 Update version response parser in `Src/ota_manager_task.c`: `parse_version_response()` now takes `uint32_t *wait_seconds` and extracts optional `:W.<seconds>` suffix; defaults to `0` when absent
- [x] P3.1-2 Implement rollout gate logic in `Src/ota_manager_task.c`: on `wait_seconds > 0`, POLLING_VERSION returns to `OTA_STATE_IDLE` without downloading; retry on next scheduled poll
- [x] P3.1-3 `region_id` / `station_id` read from `s_meta` (`Meta_Data_t`); URL build uses `%03u%03u` format with `% 1000u` crop per Q-S7
- [x] P3.1-4 Update version endpoint in `Src/ota_manager_task.c`: GET to `<UPDATE_PATH>/?id=rrrsss` (was `<UPDATE_PATH>/`)
- [x] P3.1-5 Update download endpoint in `Src/ota_manager_task.c` DOWNLOADING state: chunk URL now `get_firmware?offset=X&length=Y&id=rrrsss` (URL is caller-built, downloader lib unchanged)
- [x] P3.1-6 HTTP `429 Too Many Requests` on `/get_firmware` already maps to `SSL_DL_ERR_HTTP` (retryable) in `a7670_ssl_downloader.c`; per-chunk retry loop re-requests and server re-checks slot eligibility
- [x] P3.1-7 Build verification 2026-04-21: app 22.7% RAM / 9.3% Flash (unchanged from Phase 3); bootloader 2.8% / 0.7%; backward compatibility with pre-rollout server maintained (missing `W.` → `W.0`)
- [ ] P3.1-8 Test: server returns `V.100:L.512:H.[sha]:W.3600` → device waits 1 hour, retries next cycle ✓
- [ ] P3.1-9 Test: server returns `V.100:L.512:H.[sha]:W.0` → device proceeds to download immediately ✓
- [ ] P3.1-10 Test: server returns `V.100:L.512:H.[sha]` (no `:W.`) → device treats as `W.0` (backward compat) ✓

---

## Phase 3.2 — Image Size Guard (Device-Side Flash Partition Limit)

> Cross-document review 2026-04-21 revealed the firmware accepts any `L.` ≤ `OIW_MAX_IMAGE_SIZE` (4 MB, the download-bitmap ceiling) but does not enforce the **actual** Flash partition limit (`FLASH_APP_SIZE_MAX = 480 KB`, per `OTA_Firmware_Architecture.md §6`). A mis-built or mis-uploaded server binary larger than 480 KB would be downloaded in full, then fail either at bootloader SHA-256 verify or silently corrupt beyond page 255 during Flash programming. Server-side gate added at `Server_Architecture.md §3.4` ("Firmware size ceiling") rejects oversize uploads with HTTP 413; device must enforce the same limit as defence-in-depth.

- [x] P3.2-1 Add `FLASH_APP_SIZE_MAX` constant (= 480 * 1024) to `shared/fram_addresses.h`; cites `OTA_Firmware_Architecture.md §6`
- [x] P3.2-2 In `Src/ota_manager_task.c` `POLLING_VERSION` state: replaced `OIW_MAX_IMAGE_SIZE` (512 KB) with `FLASH_APP_SIZE_MAX` (480 KB) — rejects oversized image before any download
- [x] P3.2-3 In `bootloader/src/main.c`, before `verify_image_sha256()`: added `ocb.image_size <= FLASH_APP_SIZE_MAX` guard — `image_size == 0` or `> 480 KB` falls through to old app
- [x] P3.2-4 Build verification 2026-04-26: app 22.7% RAM / 9.3% Flash (unchanged); bootloader 2.8% / 0.7% (unchanged) ✓
- [x] P3.2-5 Unit test (host) 2026-04-26: 6/6 pass — `ovp_parse()` + FLASH_APP_SIZE_MAX guard; 512 KB rejects, 480 KB accepts, 1-byte-over rejects, zero rejects, malformed rejects; run via `bash scripts/run_native_tests.sh` (WSL2)
- [ ] P3.2-6 Hardware test: server advertises oversize image → device polls, parses, rejects without downloading; WatchdogTask unaffected
- [ ] P3.2-7 Hardware test: corrupt OCB with `image_size = 0xFFFFFFFF` → bootloader falls through to old app (covers P1-11 boundary)

---

## Server Phases (see `Server_Implementation_Plan.md` for full detail)

| Phase | Description | Status |
|-------|-------------|--------|
| S1 | FastAPI scaffold, gunicorn, systemd | ✓ complete (2026-04-26) |
| S2 | DB schema, migrations, asyncpg pool | ✓ complete (2026-04-26) |
| S3 | mTLS ingestion (`/api/v1/weather/upload`) | ✓ complete (2026-04-26) |
| S4 | Nginx mTLS termination, PKI, CRL, Let's Encrypt | ✓ complete (2026-05-06); **T4 mTLS tests complete** (2026-05-07) — 6/6 tests pass (T4-3, T4-6 N/A) |
| S5 | OTA device endpoints (`/`, `/get_firmware`) | ✓ complete (2026-05-06) |
| S6 | Admin JWT auth & RBAC | ✓ complete (2026-05-06) |
| S7 | Admin OTA campaign management REST API | ✓ complete (2026-05-07) — 25/25 T3 tests pass |
| S8 | Admin UI (HTMX + Jinja2) | ✓ complete (2026-05-07) — 14/14 admin UI E2E tests pass |
| S9 | Observability (Prometheus, Loki, Grafana) | not started |
| S10 | CI/CD & hardening (optional) | not started |

> **Deploy:** `bash html/scripts/deploy.sh` (scp-based; server has no git repo).
> **Host:** `akp@robin-gpu.cpe.ku.ac.th` · key `~/.ssh/akrapong.key`

---

## Phase 4 — Integration & Field Testing

- [ ] P4-1 End-to-end: upload → version check → download → OTA → confirm ✓
- [ ] P4-2 Power-loss injection at each OTA state transition ✓
- [ ] P4-3 10 consecutive OTA cycles — FRAM DB ring buffer and SD card CSV unaffected ✓
- [ ] P4-4 `pio run -t size`: code ≤ 480 KB, data ≤ 96 KB ✓
- [ ] P4-5 Enable RDP1 on bootloader Flash partition; document unlock procedure ✓
- [ ] P4-6 Write factory programming procedure (bootloader first, then application) ✓
- [ ] P4-7 Code review: mutex discipline, no dynamic allocation in any OTA path ✓
