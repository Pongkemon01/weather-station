# CLAUDE.md

> Loaded at session start. Project context, constraints, conventions.
> Do not delete or rename.

**Status:** `IMPLEMENTATION_STATUS.md`
**Firmware:** `OTA_Firmware_Architecture.md` · **Server:** `Server_Architecture.md`
**Plans:** `Server_Implementation_Plan.md`, `User_Management_Implementation_Plan.md`
**Tests:** `Server_Test_Plan.md`, `User_Management_Test_Plan.md`

---

## Project

STM32L476RG Weather Station Firmware + OTA Update + FastAPI Server.

| Component | Part / Tool |
|-----------|-------------|
| MCU       | STM32L476RG (Cortex-M4F, 1 MB Flash, 128 KB SRAM) |
| FRAM      | CY15B116QN 2 MB (SPI1) |
| Modem     | SIMCom A7670E LTE (USART3, `AT+HTTP*` only — no CCH/CHTTPS) |
| RTOS      | FreeRTOS |
| Build     | PlatformIO (`platformio run`, target `nucleo_l476rg`, framework `stm32cube`) |
| Peripheral config | STM32CubeMX (Src/* generated; edits only inside USER CODE guards) |
| Server    | Python 3.13 + FastAPI on `akp@robin-gpu.cpe.ku.ac.th` |

---

## Workflow

| Action | Command |
|--------|---------|
| Build application                 | `platformio run` |
| Build bootloader                  | `pio run -e bootloader` |
| Flash application                 | `platformio run -t upload` |
| Flash bootloader                  | `pio run -e bootloader -t upload` |
| Lint                              | `scripts/lint.sh` (clang-format + cpplint; fails on violations) |
| Unit tests (host, mocked HAL)     | `wsl bash //scripts/run_native_tests.sh` (WSL2 gcc; Unity auto-installed by `pio test -e native_test`) |
| RAM usage report                  | `scripts/monitor_ram.sh` (budget ≤ 96 KB) |
| Memory size check                 | `pio run -t size` |
| Serial monitor                    | `pio device monitor -b 115200` (console on USART2) |
| Server deploy                     | `bash html/scripts/deploy.sh` (scp; server has no git repo) |

Debug console: **USART2** — `__io_putchar` routes `printf` there.

---

## Directory Layout

```
Src/                          ; application source (CubeMX + USER CODE guards)
  freertos.c                  ; MX_FREERTOS_Init — all tasks created here
  freertos_lock.c             ; newlib thread-safe locking shim
  maintask.c                  ; 1 s sensor read → FRAM + SD
  ssluploadtask.c             ; noon/midnight HTTPS upload via AT+HTTP*
  uitask.c / ucctask.c        ; LCD, LEDs, button debounce
  cdctask.c                   ; USB CDC binary protocol
  ota_manager_task.{c,h}      ; OTA state machine
  ota_image_writer.{c,h}      ; chunked FRAM write + bitmap
  watchdog_task.{c,h}         ; per-task heartbeat + IWDG refresh
Inc/                          ; application headers
lib/
  sensors/                    ; bmp390, sht45 (I2C); modbus, rain_light (RS-485)
  A7670/                      ; a7670.c, a7670_at_channel.c, a7670_https_uploader.c,
                              ;   a7670_ssl_downloader.c, a7670_ssl_cert_manager.c
  SPI_FRAM/                   ; cy15b116qn.c (raw SPI), nv_database.c (ring DB)
  user_interface/             ; mcp23017.c, ui.c
  usart_subsystem/            ; uart_subsystem.c (shared interrupt-driven UART)
  time/                       ; datetime.c, y2k_time.c
  tinyusb/                    ; vendored TinyUSB stack
  utils/                      ; weather_data.h, fixedptc.h
shared/                       ; compiled into BOTH application and bootloader
  fram_addresses.h            ; single source of truth for all FRAM addresses
  ota_control_block.{c,h}     ; dual-copy OCB read/write/validate
  crc32.{c,h}                 ; CRC-32/MPEG-2 software
  sha256.{c,h}                ; FIPS 180-4 SHA-256
bootloader/                   ; separate PIO env, bare-metal, no FreeRTOS
  ldscript_boot.ld            ; 0x08000000 / 32 KB
  Inc/                        ; main.h, stm32l4xx_hal_conf.h (SPI1+IWDG+Flash only)
  src/                        ; main.c, boot_flash.{c,h}, boot_fram.{c,h}
html/                         ; FastAPI server (deployed via scp; see Server_Implementation_Plan.md)
server_test/                  ; Python black-box verifiers for deployed server
```

---

## Peripherals

Defined in `Src/freertos.c` and `Src/maintask.c`. Reassign requires updating both.

| Peripheral | Use |
|-----------|-----|
| USART1     | RS-485 Modbus RTU — Light (0x01), Rain (0x02) |
| USART2     | Debug / console (`printf` via `__io_putchar`) |
| USART3     | A7670E LTE modem (AT channel) |
| I2C1       | MCP23017 I/O expander — UI buttons + LEDs (0x20) |
| I2C2       | BMP390 pressure/temp (0x76), SHT45 humidity/temp (0x44) |
| SPI1       | CY15B116QN FRAM — protected by `g_fram_spi_mutex` |
| USB OTG FS | TinyUSB CDC device |
| SDMMC1     | SD card (FatFs) |

---

## FreeRTOS Tasks

| Task | File | Period | Priority | Role |
|------|------|--------|----------|------|
| `UsbLoopTask`    | `freertos.c`           | event   | High   | TinyUSB device loop |
| `cdc_task`       | `Src/cdctask.c`        | event   | High   | USB CDC binary protocol |
| `maintask`       | `Src/maintask.c`       | 1 s     | Normal | Sensor → FRAM + SD |
| `uitask`         | `Src/uitask.c`         | 20 ms   | Normal | LCD refresh, LEDs, button debounce |
| `ucctask`        | `Src/ucctask.c`        | 100 ms  | Normal | LCD menu |
| `ssluploadtask`  | `Src/ssluploadtask.c`  | noon+midnight | Normal | HTTPS upload; notifies OtaManagerTask |
| `OtaManagerTask` | `Src/ota_manager_task.c` | after upload | Normal | OTA state machine |
| `WatchdogTask`   | `Src/watchdog_task.c`  | 500 ms  | High   | Heartbeat + IWDG refresh |

Every task must call the watchdog heartbeat within 500 ms.

---

## A7670 Modem — HTTP(S)

Only `AT+HTTP*` available on this variant. CCH (`AT+CCH*`) eliminated. One HTTP session at a time. SSL context 0 configured once in `Modem_Module_Init()`.

| Operation | AT flow | File |
|-----------|---------|------|
| Upload POST   | `HTTPINIT` → `HTTPPARA URL+CONTENT+SSLCFG` → `HTTPDATA` → `DOWNLOAD` prompt → binary → `HTTPACTION=1` → URC | `a7670_https_uploader.c` |
| OTA GET       | `HTTPINIT` → `HTTPPARA URL+SSLCFG` → `HTTPACTION=0` → URC → `HTTPREAD` → `HTTPTERM`                       | `a7670_ssl_downloader.c` |

POST prompt is **`DOWNLOAD`** (not `>`).

**Cert format (R-12, open):** `AT+CCERTDOWN` expects **DER binary** on this variant. Convert with `openssl x509/rsa -outform DER`. `*_der.c` filenames are accurate; verify array contents are DER (no `-----BEGIN` headers) before each fleet build.

---

## Internal Flash Layout

```
0x08000000  Bootloader  (32 KB, pages 0–15, Bank 1)   RDP1 + write-protected
0x08008000  Application (480 KB, pages 16–255, Bank 1) ldscript_app.ld
0x08080000  Bank 2      (512 KB — reserved)
```

---

## Memory Constraints

| Region | Limit | Enforced by |
|--------|-------|-------------|
| App code (`.text` + `.rodata`)  | ≤ 480 KB     | Linker `ASSERT`, `scripts/monitor_ram.sh` |
| App data (`.data` + `.bss`)     | ≤ 96 KB (SRAM1) | Linker `ASSERT`, `scripts/monitor_ram.sh` |
| SRAM2 (32 KB)                   | OTA state flags only | Not in 96 KB |
| Bootloader Flash                | 32 KB        | `bootloader/ldscript_boot.ld` |
| OTA static download buffer      | 740 B        | `s_chunk_read_buf[516]` + `s_cmd_buf[224]` in `a7670_ssl_downloader.c` |

---

## Mutex Rules

| Mutex | Protects | Order |
|-------|----------|-------|
| `g_fram_spi_mutex` | All SPI1 / FRAM transactions    | **First** |
| `g_ota_state_mutex` | `OtaControlBlock_t` RMW         | Second |

Never acquire in reverse — deadlock.

---

## Coding Standards

- **C99 only.** No C++.
- **4 spaces; ≤ 100 chars / line.**
- **Naming:** existing modules `Module_FunctionName()`; OTA modules `snake_case` with prefix (`ocb_`, `oiw_`). Globals `g_PascalCase`. Macros `ALL_CAPS`.
- **Doxygen** comment for every public function.
- **Error handling:** check every HAL return; `Error_Handler()` or typed status. Never silently ignore.
- **No dynamic allocation** after init. Forbidden entirely in OTA and bootloader.
- **No polling.** Interrupt- or timer-driven only.
- **Large buffers `static` at file scope.** Never place buffers > 32 B on a task stack.
- **CubeMX files:** edits only inside `/* USER CODE BEGIN/END */` guards.
- **Minimize total instruction count.** Prefer constant data in Flash over mutable in RAM.

---

## Must Never Do

- Edit CubeMX-generated files outside `USER CODE BEGIN/END`.
- Use `malloc`/`calloc`/`new` or any heap allocation in embedded target code.
- Hardcode FRAM addresses — always use `shared/fram_addresses.h`.
- Introduce FreeRTOS calls inside the bootloader.
- Exceed the 512-byte static OTA chunk buffer.
- Access peripherals without HAL.
- Reassign peripherals without updating both `Src/freertos.c` and `Src/maintask.c`.
- Program application Flash without D-cache reset before each `memcmp` and I-cache reset after all pages — `OTA_Firmware_Architecture.md §5.2`.

---

## CubeMX-Generated Files

Edit only inside guards. Committed to version control.

`Src/gpio.c`, `Src/i2c.c`, `Src/spi.c`, `Src/usart.c`, `Src/rtc.c`, `Src/sdmmc.c`, `Src/usb_otg.c`, `Src/dma.c`, `Src/main.c`.

Bootloader: `bootloader/CubeMX/Bootloader.ioc` with SPI1 + IWDG + Flash HAL only.

---

## Read Before Implementing

| Before working on… | Read |
|---------------------|------|
| Any FRAM access          | `shared/fram_addresses.h`, `lib/SPI_FRAM/nv_database.h` |
| HTTPS upload/download    | `https_manual.md` ch. 16 |
| NTP sync                 | `ntp_manual.md` |
| OTA state logic          | `shared/ota_control_block.h`, `OTA_Firmware_Architecture.md §4` |
| OTA download             | `lib/A7670/a7670_ssl_downloader.h`, `OTA_Firmware_Architecture.md §6, §7` |
| Bootloader Flash write   | `OTA_Firmware_Architecture.md §5` (incl. §5.2 cache mgmt) |
| Any new FreeRTOS task    | `OTA_Firmware_Architecture.md §9`, `Src/watchdog_task.h` |
| Modem AT commands        | `lib/A7670/a7670_at_channel.h` |
| Sensor data packing      | `lib/utils/weather_data.h`, `lib/utils/fixedptc.h` |
| USB CDC protocol         | `Src/cdctask.c` header |
| Server code (`html/`)    | `Server_Architecture.md`, `Server_Implementation_Plan.md` |
| Server tests (`server_test/`) | `Server_Test_Plan.md` |
| User management code     | `User_Management_Implementation_Plan.md` |
| User management tests    | `User_Management_Test_Plan.md` |

---

## MCP Tools

### code-review-graph

This project has a structural knowledge graph. **Use it BEFORE Grep/Glob/Read** for codebase exploration — it's faster, cheaper, and exposes structural context (callers, dependents, tests).

| Tool | Use when |
|------|----------|
| `detect_changes`           | Reviewing code changes (risk-scored) |
| `get_review_context`       | Source snippets for review |
| `get_impact_radius`        | Blast radius of a change |
| `get_affected_flows`       | Execution paths impacted |
| `query_graph`              | Trace callers / callees / imports / tests / dependencies |
| `semantic_search_nodes`    | Find functions/classes by name or keyword |
| `get_architecture_overview`| High-level structure |
| `refactor_tool`            | Plan renames, find dead code |

Graph auto-updates on file changes (hooks). Fall back to Grep/Glob/Read only when the graph doesn't cover it.

### graphify

`graphify-out/GRAPH_REPORT.md` for god nodes + community structure. `graphify-out/wiki/index.md` if present.

For cross-module "how does X relate to Y" questions, prefer `graphify query "..."`, `graphify path "A" "B"`, or `graphify explain "concept"` over grep.

After modifying code files this session, run `graphify update .` to keep the graph current (AST-only, no API cost).
