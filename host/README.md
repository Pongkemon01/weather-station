# Robin Weather Station Configurator — Host

Cross-platform GUI utility for configuring and monitoring the Robin Weather
Station over USB CDC-ACM.

## Quick start (Windows 11, MinGW-w64 from Qt 6.8 LTS)

```cmd
cd C:\Users\akrap\weather-station\host

REM Configure a Release build
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release

REM Build
cmake --build build/release

REM Run from the build tree (Qt DLLs picked up via PATH set in Qt Creator)
build\release\robin_wsc.exe

REM Or build a portable distribution folder
cmake --install build/release --prefix dist/RobinWSC-1.0.0-win64
```

The contents of `dist/RobinWSC-1.0.0-win64/` form the portable distribution —
zip the folder and ship the zip. Users extract and double-click `robin_wsc.exe`.
No installer, no admin rights, no registry entries.

## Project structure

See `CLAUDE.md` for the full project specification.

```
host/
├── CMakeLists.txt              Build configuration
├── CLAUDE.md                   Project spec (read this first)
├── README.md                   This file
├── src/                        Application sources
├── resources/                  Icons, .qrc, Windows .rc, udev rules
├── packaging/                  README.txt + LGPLv3 license for distribution
├── tests/                      Unit tests (off by default)
└── build/                      CMake out-of-source build dirs (gitignored)
```

The `../shared/` directory at the repo root holds protocol headers shared
with the firmware project.

## Toolchain

- Qt 6.8 LTS (Open Source / LGPLv3) with the Serial Port module
- MinGW-w64 GCC 13.x (64-bit, bundled with Qt)
- CMake 3.21+, Ninja
- Qt Creator (recommended IDE)

Install Qt via the Qt Online Installer from <https://www.qt.io/download-qt-installer>
and tick:
- Qt 6.8.x → MinGW 13.1.0 64-bit
- Qt 6.8.x → Additional Libraries → Qt Serial Port
- Developer and Designer Tools → Qt Creator, CMake, Ninja, MinGW 13.1.0 64-bit

## License

This application is the property of RobinLab-KU.

It uses the Qt framework under the terms of LGPLv3. See
`packaging/COPYING.LGPLv3.txt` for the full Qt license text.
