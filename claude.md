# CLAUDE.md

## Development Workflow

- **Build**: `platformio run` builds the firmware. Use `platformio run -t upload` to flash after a successful build.
- **Lint**: Run `scripts/lint.sh` which invokes clang-format and cpplint; the script fails on style violations.
- **Test**: `platformio test` executes unit tests. To run a single test, use `platformio test --target <test-name>` (e.g., `platformio test --target test_sensor_driver`).

## Architecture Overview

- **HAL / Drivers** (`lib/`):
  - Sensor drivers: `bmp390.c`, `sht45.c`, `modbus.c`, `rain_light.c`.
  - SPI_FRAM drivers: `cy15b116qn.c`, `nc_database.c`.
  - USB/TinyUSB: `tinyusb/`.
  - USART subsystem: `usart_subsystem/`.
- **FreeRTOS Tasks** (`src/`):
  - `maintask.c` – main 1 s sensor processing loop (synchronized with RTC).
  - `uitask.c` – UI update every 20 ms (LCD, LEDs, button handling).
  - `ucctask.c` – User interaction task via LCD and buttons on IO-expander board for every 100ms.
  - `cdctask.c` - USB CDC communication task.
  - `ssluptask.c` – HTTPS upload task (runs at noon/midnight).
  - `freertos.c` – task creation and initialization.
- **Memory Limits**:
  - Flash: ≤512 KB application code (leaves 512 KB for bootloader).
  - RAM: ≤96 KB usage (monitor with `size -A build/firmware.elf`).
- **Non‑volatile storage**: CY15B116QN F‑RAM (2 MB) accessed via SPI_FRAM.
- **Startup**: `main.c` runs CubeMX generated init code, creates FreeRTOS tasks, starts scheduler.

## Coding Standards

- **Language**: C99 only; no C++.
- **Indentation**: 4 spaces; max 100 chars per line.
- **Naming**:
  - Functions: `Peripheral_Function()`.
  - Global variables: `g_PascalCase`.
  - Macros: `ALL_CAPS_WITH_UNDERSCORES`.
- **Documentation**: Doxygen comment for every public function.
- **Error handling**: Always check HAL return values (`HAL_OK`).

## Critical Restrictions

- **Never** use `malloc`, `free`, or any dynamic allocation after initialization.
- **Never** access peripherals directly; always go through HAL functions.
- **Never** poll sensors; use interrupt-driven or timer‑based reads.
- Keep RAM usage within budget; avoid hidden stack growth.

## Testing Strategy

- **Host unit tests**: Run on x86 with mocked HAL.
- **Integration tests**: Deploy to target hardware via automated harness.
- **Coverage goals**: 80 % for application logic, 100 % for critical state machines.

## Useful Scripts

- `scripts/lint.sh` – runs style checks.
- `scripts/run_tests.sh` – wrapper that builds and runs `platformio test`.
- `scripts/monitor_ram.sh` – prints current RAM usage after each build.

</details>