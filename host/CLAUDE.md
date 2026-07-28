# Robin Weather Station Configurator — Host Application

## Project context

This is the cross-platform GUI host utility for the **Robin Weather Station**,
an STM32L476RG-based device running FreeRTOS + TinyUSB. The host talks to
the device exclusively over **USB CDC-ACM** (12 Mbit/s full-speed).

The firmware project lives in a sibling directory: `../firmware/`.
Shared protocol headers live in: `../shared/`.

## Stack

- **Language:** C++17
- **GUI:** Qt 6.8 LTS — Qt Widgets (NOT QML)
- **USB transport:** Qt Serial Port module (`QSerialPort`, `QSerialPortInfo`)
- **Compiler (Windows):** MinGW-w64 GCC, 64-bit (the version Qt ships with)
- **Compiler (Linux):** System GCC 11+
- **Compiler (macOS):** Apple Clang 14+
- **Build system:** CMake 3.21+, Ninja generator
- **Primary dev OS:** Windows 11
- **Targets:** Windows 10 (1809+), Windows 11, Linux (glibc 2.31+), macOS 12+

## Identity

| Field | Value |
|---|---|
| Product name | Robin Weather Station |
| Manufacturer | RobinLab |
| Executable | `robin_wsc.exe` (Windows) / `robin_wsc` (Linux/macOS) |
| Version | 1.0.0 |
| USB VID:PID | `0x1209:0xDCB1` (pid.codes, pending merge) |
| Minimum firmware protocol | v1.0 |

## Key constraints

1. **Self-contained portable distribution.** The build produces a folder
   containing `robin_wsc.exe` plus dynamically-linked Qt DLLs bundled via
   `windeployqt`. The MinGW C/C++ runtime is statically linked into the
   executable. The folder is fully relocatable — no installer, no registry,
   no per-user config files. **DO NOT attempt to statically link Qt itself**
   (Qt is used under LGPLv3, which requires dynamic linking unless source
   is published).

2. **No persistent logging on host.** In-memory ring buffer only via the
   `LogBuffer` class. Log lost on app exit. No "Save log to file" feature.

3. **No A7670E modem access from host.** The host talks only to the STM32
   over CDC. All cellular activity stays on the firmware side.

4. **No file I/O** beyond Qt resource files unless a feature explicitly
   requires it (and that feature should be reviewed before implementation).

5. **Permissive protocol versioning.** On firmware/host protocol mismatch,
   show a non-modal warning banner and proceed. Only refuse to connect when
   firmware version is below v1.0.

6. **English-only (en_US).** No translation files. Use `tr()` anyway for
   future-proofing — costs nothing.

7. **No network code.** No telemetry, no auto-update, no analytics.

8. **No code signing.** Distribution is an unsigned zip of a portable folder.

## Code style

- Match the firmware project's warning hygiene: `-Wall -Wextra -Wpedantic`
  clean. Also `-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
  -Wold-style-cast -Wcast-align -Wnull-dereference -Wdouble-promotion`.
- Use Qt's signals/slots with `Qt::QueuedConnection` for cross-thread
  communication.
- No raw `new` / `delete` — use Qt's parent-child ownership or smart pointers.
- One class per file: header `.h` + implementation `.cpp`.
- `.ui` files (Qt Designer XML) for all dialogs and main window layout.
- Shared protocol headers in `../shared/` must compile under BOTH MinGW gcc
  AND arm-none-eabi-gcc. C only, `<stdint.h>` only, no Qt or HAL headers.
- Wire structs use the `PACKED` macro from `protocol_compat.h`, always
  guarded with `STATIC_ASSERT(sizeof(...) == N, "drift")`.

## Layout

```
host/
├── CMakeLists.txt              # Build configuration
├── CLAUDE.md                   # This file
├── README.md                   # Developer-facing overview
├── .gitignore
├── src/                        # Host application sources
│   ├── main.cpp
│   ├── app_info.h              # Single source of truth: version, IDs, strings
│   ├── mainwindow.{h,cpp,ui}
│   ├── device_controller.{h,cpp}
│   ├── device_io.{h,cpp}
│   ├── frame_parser.{h,cpp}
│   ├── log_buffer.{h,cpp}
│   └── dialogs/                # Per-feature dialogs
├── resources/
│   ├── robin_wsc.qrc           # Qt resource manifest
│   ├── robin_wsc.rc            # Windows version-info resource
│   ├── icons/
│   └── udev/
│       └── 99-robin-weather-station.rules   # Linux udev rule (deployment)
├── packaging/
│   ├── README.txt              # End-user readme (shipped with binary)
│   └── COPYING.LGPLv3.txt      # Qt LGPLv3 license text (shipped with binary)
├── tests/
│   └── frame_parser_test.cpp
└── build/                      # CMake out-of-source builds (gitignored)
    ├── debug/
    └── release/
```

## Threading model

- **UI thread (main):** `MainWindow`, `DeviceController`, all dialogs,
  `LogBuffer`.
- **IO thread** (`QThread`): `DeviceIO` holds the `QSerialPort` and frame
  parser; emits parsed frames.
- **Communication:** signals/slots only, queued connections across the
  thread boundary. No shared mutable state crosses the line.

## USB device discovery & open

- Discover via `QSerialPortInfo::availablePorts()`, filter by
  `vendorIdentifier() == 0x1209 && productIdentifier() == 0xDCB1`.
- Each unit has a unique `iSerialNumber` (24 hex chars from STM32 96-bit UID).
  Use `QSerialPortInfo::serialNumber()` to distinguish multiple units.
- Open with `QSerialPort::Baud115200` (baud is fictional over USB-CDC but
  Windows requires it to be set).
- After open: `setDataTerminalReady(false)`, `setRequestToSend(false)`.

## Build

```cmd
cd C:\Users\akrap\weather-station\host

REM Configure
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release

REM Build
cmake --build build/release

REM Package into portable distribution
cmake --install build/release --prefix dist/RobinWSC-1.0.0-win64
```

The `dist/` folder is the portable distribution. Zip it for delivery.

## What NOT to do

- Don't add platform-specific code without `#ifdef` guards.
- Don't write to disk for any reason except explicit user-initiated export.
- Don't add network code, telemetry, or auto-update logic.
- Don't use QML — Widgets only.
- Don't break compatibility of `../shared/` headers with `arm-none-eabi-gcc`.
- Don't add dependencies beyond Qt Core/Widgets/SerialPort.
- Don't statically link Qt (LGPLv3 compliance).
- Don't add a "Save log" or "Load config" file feature without checking with
  the user first — persistent host state is explicitly out of scope.

## Open items still TBD

- pid.codes allocation of `0x1209:0xDCB1` must merge before any release.
- Protocol spec described as comments in `../Src/cdctask.c` .
