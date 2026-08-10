# Robin Weather Station

An STM32L476RG-based weather station with cellular data upload, field-O.T.A. firmware updates, a FastAPI ingest + administration server, and a USB-connected Qt desktop configuration utility.

Three distinct codebases live in this repository and share one data story:

1. **Firmware** — bare-metal-ish FreeRTOS application + a separate bootloader for the STM32L476RG.
2. **Server** — FastAPI service that ingests weather records over mTLS, serves firmware images over OTA, and hosts an HTMX admin UI.
3. **Host application** — "Robin WSC", a Qt 6 Widgets desktop tool for connecting to a station over USB CDC and configuring/reading it locally.

Documentation, implementation plans, and as-built test results are in the repo root (see [Documentation](#documentation)).

---

## Hardware & System Context

| Component | Part / Tool |
|-----------|-------------|
| MCU       | STM32L476RG (Cortex-M4F, 1 MB Flash, 128 KB SRAM) |
| FRAM      | CY15B116QN 2 MB (SPI1) — persistent sensor ring DB + OTA staging |
| Modem     | SIMCom A7670E LTE (USART3, `AT+HTTP*` only — no CCH/CHTTPS) |
| Sensors   | BMP390 (I2C2), SHT45 (I2C2); RS-485 Modbus light (0x01) + rain (0x02) via USART1 |
| UI        | LCD + LEDs + buttons through MCP23017 I/O expander (I2C1) |
| Storage   | SD card (SDMMC1, FatFs) beside FRAM |
| RTOS      | FreeRTOS (CMSIS-RTOS V2) |
| Peripherals | USART1 RS-485 Modbus, USART2 debug console, USART3 A7670 AT, USB OTG FS TinyUSB CDC |

**Sensor → storage → upload pipeline:** `maintask` reads sensors every second, packs readings (Q9.7 fixed-point), stores them in the SPI FRAM ring database and on SD; `ssluploadtask` POSTs a snapshot to the server at noon and midnight over the A7670 LTE modem.

**OTA:** an 8-state OTA manager task downloads firmware images over the modem in 512-byte chunks, verifies SHA-256/CRC-32, and writes them to the FRAM image buffer; the bootloader applies the update to the application flash on next reset. OTA campaigns are controlled from the server admin UI and gated by rollout version strings (`W.<sec>`).

**Local access (USB):** the station enumerates as a USB CDC device (TinyUSB on OTG FS). This USB port serves two purposes: it carries the binary configuration/readout protocol that the Robin WSC desktop app uses to connect to a station (via `QSerialPort`), and it acts as a secondary debug/console channel alongside USART2.

### Internal Flash Layout

```
0x08000000  Bootloader  (32 KB, pages 0–15, Bank 1)   RDP1 + write-protected
0x08008000  Application (480 KB, pages 16–255, Bank 1) ldscript_app.ld
0x08080000  Bank 2      (512 KB — reserved)
```

### Memory Constraints

| Region | Limit |
|--------|-------|
| App code (`.text` + `.rodata`) | ≤ 480 KB (linker `ASSERT`) |
| App data (`.data` + `.bss`)    | ≤ 96 KB (SRAM1) |
| Bootloader flash                | 32 KB |
| OTA static chunk buffer         | ≤ 512 B |

Current build budget: app ~9.3 % flash / ~22.7 % RAM; bootloader ~0.7 % / ~2.8 % (see `IMPLEMENTATION_STATUS.md`).

---

## Repository Layout

| Path | What it is |
|------|-----------|
| `Src/`              | Application source (CubeMX-generated + `USER CODE` guards). `freertos.c` creates all tasks. |
| `Inc/`              | Application headers |
| `lib/`              | Libraries: `sensors/` (BMP390, SHT45, Modbus, rain/light), `A7670/` (AT channel, HTTPS upload, SSL OTA download), `SPI_FRAM/` (raw SPI + ring DB), `user_interface/` (MCP23017, LCD), `usart_subsystem/`, `time/`, `utils/` (weather_data, fixed-point), `tinyusb/` (vendored) |
| `shared/`           | Compiled into **both** firmware and bootloader: `fram_addresses.h` (single FRAM source of truth), dual-copy `ota_control_block`, CRC-32, SHA-256 |
| `bootloader/`       | Separate PlatformIO env, bare-metal (no FreeRTOS): image apply, OCB read/write, FRAM bounce |
| `html/`             | FastAPI server (mTLS ingest, OTA endpoints, admin JWT + RBAC, HTMX UI, Prometheus metrics) |
| `server_deployment/`| One-shot shell setup scripts for the Debian host (DB, nginx, systemd, PKI, monitoring) |
| `server_test/`      | Python black-box verifiers against the deployed server |
| `host/`             | "Robin WSC" — Qt 6.8 Widgets + QSerialPort desktop app (C++17, CMake, ctest tests) |
| `test/`             | Extra host-side suites (e.g. OTA size guard — `test_ota_size_guard/`) |
| `Altium Schematics/`| Hardware schematic source |

### FreeRTOS Tasks (firmware)

| Task | File | Period | Priority | Role |
|------|------|--------|----------|------|
| `UsbLoopTask`  | `freertos.c`           | event | High   | TinyUSB device loop |
| `cdc_task`     | `Src/cdctask.c`        | event | High   | USB CDC binary protocol (used by the host app) |
| `maintask`     | `Src/maintask.c`       | 1 s   | Normal | Sensor → FRAM + SD |
| `uitask`       | `Src/uitask.c`         | 20 ms | Normal | LCD refresh, LEDs, button debounce |
| `ucctask`      | `Src/ucctask.c`        | 100 ms| Normal | LCD menu |
| `ssluploadtask`| `Src/ssluploadtask.c`  | noon+midnight | Normal | HTTPS upload; notifies OTA manager |
| `OtaManagerTask`| `Src/ota_manager_task.c` | after upload | Normal | OTA state machine |
| `WatchdogTask` | `Src/watchdog_task.c`  | 500 ms| High   | Per-task heartbeat + IWDG refresh |

---

## Building

Built with **PlatformIO** (`nucleo_l476rg` env, STM32Cube framework). On Windows use `platformio`; on Linux/macOS the WSL helpers below also target WSL2 gcc.

| Action | Command |
|--------|---------|
| Build application | `platformio run` |
| Build bootloader  | `pio run -e bootloader` |
| Flash application | `platformio run -t upload` |
| Flash bootloader  | `pio run -e bootloader -t upload` |
| Lint              | `scripts/lint.sh` (clang-format + cpplint) |
| Unit tests (host, mocked HAL) | `wsl bash //scripts/run_native_tests.sh` |
| RAM usage report  | `scripts/monitor_ram.sh` (budget ≤ 96 KB) |
| Size check        | `pio run -t size` |
| Serial monitor    | `pio device monitor -b 115200` (USART2 console) |

**Host app (`host/`):** CMake + `ctest`; tests pass cleanly (frame parser, Q9.7 conversion, device controller). Portable Windows build via windeployqt is packaged under `host/packaging/`.

**Server:** deployed by `bash html/scripts/deploy.sh` (scp to the host; the server has no git repo). Environment setup is scripted in `server_deployment/` (`01_setup_server.sh` … `full_setup.sh`) for a Debian host with PostgreSQL 17 + TimescaleDB, nginx mTLS termination, and systemd unit.

---

## Documentation

| Doc | Covers |
|-----|--------|
| `CLAUDE.md` | Project conventions, directory map, peripheral allocation, task map, coding standards, "must never do" rules. **Read this first.** |
| `IMPLEMENTATION_STATUS.md` | Single source of truth for phase completion and build budgets |
| `OTA_Firmware_Architecture.md` | Firmware OTA: FRAM layout, state machine, bootloader, A7670 modem flow |
| `Server_Architecture.md` | Server: mTLS ingest, OTA endpoints, slot algorithm, DB schema |
| `Server_Implementation_Plan.md` / `User_Management_Implementation_Plan.md` | Server phase plans |
| `Server_Test_Plan.md` / `User_Management_Test_Plan.md` | Black-box verifier coverage |
| `USART_Subsystem_Migration_Plan.md` | UART subsystem plan |
| `https_manual.md`, `ntp_manual.md` | A7670E AT command references (vendor) |
| `host/host_implementation_plan.md`, `host/CLAUDE.md` | Host app plan & conventions |

---

## Status (as of `IMPLEMENTATION_STATUS.md`)

- **Firmware:** OTA phases P0–P3.2 implemented; remaining work is hardware integration/field testing (P1/P2/P3/P4 items). Open risk **R-12**: verify embedded cert arrays in `*_der.c` are DER (not PEM) bytes before the next fleet build.
- **Server:** all phases complete (S0–S11), including mTLS ingestion, OTA campaigns, admin RBAC, and monitoring.
- **User management:** complete (UM1–UM5).
- **Host app:** complete (Ph0–Ph9), all ctest suites pass.

Security-sensitive components use mTLS (device ingest), JWT + RBAC (admin), RDP1 + write-protection on the bootloader, and Ed25519-signed firmware images.

---

## License

See `LICENSE` in the repository root.