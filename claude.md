# CLAUDE.md

> Read automatically by Claude Code at session start. Defines project context, constraints, and conventions.
> Do not delete or rename this file.
>
> **Full OTA architecture:** `OTA_Firmware_Architecture.md` | **OTA phase status:** `IMPLEMENTATION_STATUS.md`
>
> **Server architecture:** `Server_Architecture.md` | **Server impl plan:** `Server_Implementation_Plan.md` | **Server test plan:** `Server_Test_Plan.md`


---

## Project Identity

**Project:** STM32L476RG Weather Station Firmware + OTA Update Refactoring
**MCU:** STM32L476RG (Cortex-M4F, 1 MB Flash, 128 KB SRAM)
**FRAM:** CY15B116QN 2 MB (SPI1)
**Modem:** SIMCom A7670E LTE (USART3)
**RTOS:** FreeRTOS
**Build system:** PlatformIO (`platformio run`, target `nucleo_l476rg`, framework `stm32cube`)
**Peripheral config:** STM32CubeMX — generated files in `Src/`; custom code only inside `USER CODE BEGIN/END` guards

---

## Development Workflow

| Action | Command |
|--------|---------|
| Build application | `platformio run` |
| Build bootloader | `pio run -e bootloader` |
| Flash application | `platformio run -t upload` |
| Flash bootloader | `pio run -e bootloader -t upload` |
| Lint | `scripts/lint.sh` (clang-format + cpplint; fails on violations) |
| Unit tests (host/x86, mocked HAL) | `wsl bash //scripts/run_native_tests.sh` (no MinGW — uses WSL2 gcc; Unity libs auto-installed by `pio test -e native_test`) |
| RAM usage report | `scripts/monitor_ram.sh` (budget ≤ 96 KB) |
| Memory size check | `pio run -t size` |
| Serial monitor | `pio device monitor -b 115200` (console on USART2) |

Debug console: **USART2** — `__io_putchar` routes `printf` there.

---

## Directory Layout

```
project_root/
├── Src/                    ← Application source
│   ├── freertos.c          ← Task creation (MX_FREERTOS_Init), peripheral handles
│   ├── freertos_lock.c     ← Newlib thread-safe locking shim (Strategy #4)
│   ├── maintask.c          ← 1 s sensor read, pack, save to FRAM + SD
│   ├── ssluploadtask.c     ← Noon/midnight HTTPS upload via A7670
│   ├── uitask.c            ← LCD refresh, LEDs, button debounce (20 ms)
│   ├── ucctask.c           ← LCD menu / user interaction (100 ms)
│   ├── cdctask.c           ← USB CDC binary protocol handler
│   ├── ota_manager_task.c/.h   ← OTA state machine task
│   ├── ota_image_writer.c/.h   ← Chunked FRAM write + download bitmap
│   └── watchdog_task.c/.h      ← Per-task heartbeat + IWDG refresh
├── Inc/                    ← Application headers
├── lib/
│   ├── sensors/            ← bmp390.c, sht45.c (I2C); modbus.c, rain_light.c (RS-485)
│   ├── A7670/              ← LTE modem driver (a7670.c, a7670_at_channel.c,
│   │                          a7670_https_uploader.c, a7670_ssl_downloader.c,
│   │                          a7670_ssl_cert_manager.c)
│   ├── SPI_FRAM/           ← cy15b116qn.c (raw SPI), nv_database.c (circular DB)
│   ├── user_interface/     ← mcp23017.c (I2C expander), ui.c (LCD + LED)
│   ├── usart_subsystem/    ← uart_subsystem.c (shared interrupt-driven UART)
│   ├── time/               ← datetime.c (RTC helpers), y2k_time.c (Y2K epoch)
│   ├── tinyusb/            ← Vendored TinyUSB stack (USB CDC device)
│   └── utils/              ← weather_data.h (packed/unpacked types), fixedptc.h
├── shared/                 ← Compiled into BOTH application and bootloader
│   ├── fram_addresses.h    ← Single source of truth for ALL FRAM addresses
│   ├── ota_control_block.h/.c  ← OtaControlBlock_t read/write/validate (dual-copy)
│   ├── crc32.h/.c          ← Software CRC-32/MPEG-2
│   └── sha256.h/.c         ← Standalone FIPS 180-4 SHA-256
├── bootloader/             ← Separate PlatformIO environment (bare-metal, no FreeRTOS)
│   ├── ldscript_boot.ld    ← Origin 0x08000000, length 32 KB
│   ├── Inc/                ← main.h, stm32l4xx_hal_conf.h (SPI1 + IWDG + Flash only)
│   └── src/                ← main.c, boot_flash.c/.h, boot_fram.c/.h
├── html/                   ← Server-side FastAPI code (deployed to Ubuntu via SSH;
│                              see Server_Implementation_Plan.md for layout)
└── server_test/            ← Python black-box verifiers for the deployed server
                              (see Server_Test_Plan.md)
```

---

## Peripheral Allocation

Defined in `Src/freertos.c` and `Src/maintask.c`. Do not reassign without updating both.

| Peripheral | Use |
|-----------|-----|
| USART1 | RS-485 Modbus RTU — Light sensor (addr 0x01), Rain sensor (addr 0x02) |
| USART2 | Debug / console (`printf` via `__io_putchar`) |
| USART3 | A7670E LTE modem (AT command channel) |
| I2C1 | MCP23017 I/O expander — UI buttons & LEDs (addr 0x20) |
| I2C2 | BMP390 pressure/temp (addr 0x76), SHT45 humidity/temp (addr 0x44) |
| SPI1 | CY15B116QN F-RAM (2 MB) — protected by `g_fram_spi_mutex` |
| USB OTG FS | TinyUSB CDC device |
| SDMMC1 | SD card (via FatFs) |

---

## FreeRTOS Task Map

All tasks created in `Src/freertos.c` → `MX_FREERTOS_Init()`.

| Task | File | Period | Priority | Role |
|------|------|--------|----------|------|
| `UsbLoopTask` | `freertos.c` | event-driven | High | TinyUSB device loop (`tud_task`) |
| `cdc_task` | `Src/cdctask.c` | event-driven | High | USB CDC binary protocol handler |
| `maintask` | `Src/maintask.c` | 1 s (RTC-sync) | Normal | Reads sensors, saves to FRAM & SD |
| `uitask` | `Src/uitask.c` | 20 ms | Normal | LCD refresh, LEDs, button debounce |
| `ucctask` | `Src/ucctask.c` | 100 ms | Normal | LCD menu / user interaction |
| `ssluploadtask` | `Src/ssluploadtask.c` | noon & midnight | Normal | Batches FRAM records, POSTs via HTTPS; notifies OtaManagerTask |
| `OtaManagerTask` | `Src/ota_manager_task.c` | after upload (xTaskNotify) | Normal | OTA version poll + download state machine |
| `WatchdogTask` | `Src/watchdog_task.c` | 500 ms | High | Per-task heartbeat + IWDG refresh |

---

## A7670 Modem — HTTP(S) Service

CCH (`AT+CCH*`) is eliminated. Both upload and download use `AT+HTTP*`. Only one HTTP session active at a time.

| Operation | AT flow | File |
|-----------|---------|------|
| Data upload (POST) | `HTTPINIT` → `HTTPPARA URL+CONTENT+SSLCFG` → `HTTPDATA` → `DOWNLOAD` prompt → binary → `HTTPACTION=1` → URC | `a7670_https_uploader.c` |
| OTA download (GET) | `HTTPINIT` → `HTTPPARA URL+SSLCFG` → `HTTPACTION=0` → URC → `HTTPREAD` → `HTTPTERM` | `a7670_ssl_downloader.c` |

SSL context 0 configured once in `Modem_Module_Init()` via `AT+CSSLCFG`. POST prompt is `DOWNLOAD` (not `>`).

**Known modem issues:**

- Cert data must be in **DER binary format** (no PEM headers); `*_der.c` filenames are accurate. Convert PEM → DER with `openssl x509 -outform DER` (certs) and `openssl rsa -outform DER` (keys) before generating C byte arrays.

---

## Internal Flash Layout

```
0x08000000  Bootloader  (32 KB, pages 0–15,   Bank 1)  ← RDP1 + write-protected
0x08008000  Application (480 KB, pages 16–255, Bank 1)  ← ldscript_app.ld
0x08080000  Bank 2      (512 KB — reserved)
```

---

## Hard Memory Constraints

| Region | Limit | Enforced by |
|--------|-------|-------------|
| Application code (`.text` + `.rodata`) | ≤ 480 KB | Linker `ASSERT`; `scripts/monitor_ram.sh` |
| Application data (`.data` + `.bss`) | ≤ 96 KB (SRAM1) | Linker `ASSERT`; `scripts/monitor_ram.sh` |
| SRAM2 (0x10000000, 32 KB) | OTA state flags only | Not counted in 96 KB budget |
| Bootloader Flash | 32 KB | `bootloader/ldscript_boot.ld` |
| OTA static download buffer | 740 bytes, `static` | `s_chunk_read_buf[516]` + `s_cmd_buf[224]` in `a7670_ssl_downloader.c` |

---

## Mutex Rules

| Mutex | Protects | Acquisition order |
|-------|---------|------------------|
| `g_fram_spi_mutex` | All SPI1 / FRAM transactions | **Always acquired first** |
| `g_ota_state_mutex` | `OtaControlBlock_t` read-modify-write | Always acquired second |

Never acquire in reverse order — deadlock results.

---

## Coding Standards

- **Language:** C99 only. No C++.
- **Indentation:** 4 spaces; max 100 chars per line.
- **Naming:** Existing modules: `Module_FunctionName()`; OTA modules: `snake_case` with prefix (`ocb_`, `oiw_`). Globals `g_PascalCase`. Macros `ALL_CAPS_WITH_UNDERSCORES`.
- **Documentation:** Doxygen comment for every public function.
- **Error handling:** Always check HAL return values (`HAL_OK`). Call `Error_Handler()` or return a typed status code. Never silently ignore.
- **No dynamic allocation:** `malloc`/`free`/`calloc`/`realloc` forbidden after init; forbidden entirely in OTA and bootloader.
- **No polling:** Use interrupt-driven or timer-based reads; never busy-poll peripherals.
- **Large buffers:** Declare `static` at file scope. Never place buffers > 32 bytes on a FreeRTOS task stack.
- **Watchdog:** Every FreeRTOS task must call the `WatchdogTask` heartbeat API within 500 ms.
- **CubeMX files:** Edit only inside `/* USER CODE BEGIN */` / `/* USER CODE END */` guards.
- **Minimize instructions:** Always minimize total instruction count.
- **Prefer Flash:** Use constant data in Flash over mutable data in RAM.

---

## CubeMX-Generated Files

Do not edit outside guards. Committed to version control.

`Src/gpio.c`, `Src/i2c.c`, `Src/spi.c`, `Src/usart.c`, `Src/rtc.c`, `Src/sdmmc.c`, `Src/usb_otg.c`, `Src/dma.c`, `Src/main.c`

Bootloader uses `bootloader/CubeMX/Bootloader.ioc` with SPI1, IWDG, Flash HAL only.

---

## What Claude Code Must Never Do

- Edit CubeMX-generated files outside `USER CODE BEGIN/END` blocks.
- Use `malloc`, `calloc`, `new`, or any heap allocation in embedded target code.
- Hardcode FRAM addresses — always use constants from `shared/fram_addresses.h`.
- Introduce FreeRTOS calls inside the bootloader.
- Exceed the 512-byte static OTA download chunk buffer.
- Access peripherals directly — always use HAL functions.
- Reassign peripherals without updating both `Src/freertos.c` and `Src/maintask.c`.
- Program application Flash in the bootloader without D-cache reset before each `memcmp` and I-cache reset after all pages written — see `OTA_Firmware_Architecture.md §9.4`.

---

## Key Files to Read Before Implementing

| Before implementing… | Read these first |
|----------------------|-----------------|
| Any FRAM access | `shared/fram_addresses.h`, `lib/SPI_FRAM/nv_database.h` |
| HTTPS upload/download | `https_manual.md` (Chapter 16 — AT+HTTP* methods) |
| NTP sync | `ntp_manual.md` |
| OTA state logic | `shared/ota_control_block.h`, `OTA_Firmware_Architecture.md §8` |
| OTA download loop | `lib/A7670/a7670_ssl_downloader.h`, `OTA_Firmware_Architecture.md §10.3, §10.5` |
| Bootloader Flash write | `OTA_Firmware_Architecture.md §9` (incl. §9.4 Cache Management) |
| Any new FreeRTOS task | `OTA_Firmware_Architecture.md §12`, `Src/watchdog_task.h` |
| Modem AT commands | `lib/A7670/a7670_at_channel.h` |
| Sensor data packing | `lib/utils/weather_data.h`, `lib/utils/fixedptc.h` |
| USB CDC protocol | `Src/cdctask.c` header block |
| Server-side code (in `html/`) | `Server_Architecture.md`, `Server_Implementation_Plan.md` |
| Server verification tests (in `server_test/`) | `Server_Test_Plan.md`, `Server_Architecture.md §3` |

<!-- code-review-graph MCP tools -->
## MCP Tools: code-review-graph

**IMPORTANT: This project has a knowledge graph. ALWAYS use the
code-review-graph MCP tools BEFORE using Grep/Glob/Read to explore
the codebase.** The graph is faster, cheaper (fewer tokens), and gives
you structural context (callers, dependents, test coverage) that file
scanning cannot.

### When to use graph tools FIRST

- **Exploring code**: `semantic_search_nodes` or `query_graph` instead of Grep
- **Understanding impact**: `get_impact_radius` instead of manually tracing imports
- **Code review**: `detect_changes` + `get_review_context` instead of reading entire files
- **Finding relationships**: `query_graph` with callers_of/callees_of/imports_of/tests_for
- **Architecture questions**: `get_architecture_overview` + `list_communities`

Fall back to Grep/Glob/Read **only** when the graph doesn't cover what you need.

### Key Tools

| Tool | Use when |
|------|----------|
| `detect_changes` | Reviewing code changes — gives risk-scored analysis |
| `get_review_context` | Need source snippets for review — token-efficient |
| `get_impact_radius` | Understanding blast radius of a change |
| `get_affected_flows` | Finding which execution paths are impacted |
| `query_graph` | Tracing callers, callees, imports, tests, dependencies |
| `semantic_search_nodes` | Finding functions/classes by name or keyword |
| `get_architecture_overview` | Understanding high-level codebase structure |
| `refactor_tool` | Planning renames, finding dead code |

### Workflow

1. The graph auto-updates on file changes (via hooks).
2. Use `detect_changes` for code review.
3. Use `get_affected_flows` to understand impact.
4. Use `query_graph` pattern="tests_for" to check coverage.

## graphify

This project has a graphify knowledge graph at graphify-out/.

Rules:
- Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
- If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
- For cross-module "how does X relate to Y" questions, prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, or `graphify explain "<concept>"` over grep — these traverse the graph's EXTRACTED + INFERRED edges instead of scanning files
- After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)
