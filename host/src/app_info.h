// app_info.h — single source of truth for product identity and constants.
//
// All product strings, USB IDs, and version numbers used by the application
// are defined here. Anything user-visible that names the product or the
// organization should come from this header.

#pragma once

#include <QtGlobal>

namespace AppInfo {

// ----- Product identity -----------------------------------------------------
inline constexpr const char* kProductName        = "Robin Weather Station";
inline constexpr const char* kAppDisplayName     = "Robin Weather Station Configurator";
inline constexpr const char* kAppExecutableName  = "robin_wsc";
inline constexpr const char* kOrganizationName   = "RobinLab-KU";
inline constexpr const char* kOrganizationDomain = "robinlab.ku.ac.th";

// Version is also injected via -DAPP_VERSION="..." from CMake; keep this
// constant aligned with project(VERSION ...) in CMakeLists.txt.
#ifndef APP_VERSION
#define APP_VERSION "1.0.0"
#endif
inline constexpr const char* kAppVersion = APP_VERSION;

// ----- USB identification ---------------------------------------------------
// VID 0x1209 = pid.codes; PID 0xDCB1 = RobinLab-KU Weather Station
// (pid.codes allocation pending merge confirmation).
inline constexpr quint16 kUsbVid = 0x1209;
inline constexpr quint16 kUsbPid = 0xDCB1;

// ----- Protocol versioning --------------------------------------------------
// Encoded as 0xMMmm (major.minor). Policy is permissive: warn on mismatch,
// proceed anyway. Below kProtocolMinSupported, the host refuses to connect.
inline constexpr quint16 kProtocolMinSupported = 0x0100;  // v1.0
inline constexpr quint16 kProtocolPreferred    = 0x0100;  // v1.0

// ----- Serial port defaults -------------------------------------------------
// Baud rate is fictional over USB-CDC but Windows requires it to be set.
inline constexpr qint32 kSerialBaudRate = 115200;

// ----- Logging --------------------------------------------------------------
// In-memory ring buffer capacity. Older entries are dropped on overflow.
// Log is NOT persisted to disk.
inline constexpr int kLogBufferCapacity = 2000;

}  // namespace AppInfo
