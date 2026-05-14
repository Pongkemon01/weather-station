# Robin Weather Station Configurator — Implementation Plan

## Context

The host application skeleton at `C:\Users\akrap\weather-station\host\` already has a fully working backend layer (USB CDC discovery + open, frame send/recv state machine, in-memory log ring, app constants) but only a placeholder `mainwindow.ui` containing a smoke-test "Rescan" button and list. The user has provided 5 tab screenshots that fix the visual contract: **Status**, **Current Measurement**, **General Settings**, **Sensor Settings**, **About**. Internal per-tab logic will be specified later — this plan is the structural skeleton-to-shipping path.

The wire protocol in `shared/protocol.h` already covers everything the screenshots demand: `REQ_STATUS / REQ_WEATHER / REQ_META / SET_META / REQ_RTC / SET_RTC / DB_FLUSH / SYS_RESET`. No firmware changes are needed.

User-confirmed contract:
- **Connect:** auto-open if exactly one device matches `0x1209:0xDCB1`; show picker dialog only when 2+ match; banner when zero.
- **Log surface:** hidden Debug-Log dialog reachable from a Help menu / shortcut. No permanent log panel.
- **Sampling interval:** `QComboBox` with fixed presets `1, 5, 10, 15, 30, 60` minutes; `uint8_t` value = literal minute count; default 10.
- **Polling:** Status tab `REQ_STATUS` every **2 s**; Current Measurement tab `REQ_WEATHER` every **1 s**. Pause when tab not visible.

---

## Existing assets to reuse (do NOT rewrite)

| Asset | Location | Role |
|---|---|---|
| `app_info.h` constants | `src/app_info.h` | VID/PID, version, baud, log capacity — all populated |
| `LogBuffer` ring + signals | `src/log_buffer.{h,cpp}` | `append/snapshot/clear`, thread-safe; bind directly to log dialog |
| `FrameParser` state machine | `src/frame_parser.{h,cpp}` | `payloadLen()` already covers Weather=18, Meta=216, Status=12, ACK/NAK=1 |
| `DeviceIO` worker | `src/device_io.{h,cpp}` | Serial open + frame send/recv on its own `QThread` — no changes needed |
| `DeviceController::findDevices()` | `src/device_controller.cpp:52` | VID/PID enumeration |
| Existing signal contract | `device_controller.h` | `connected/disconnectedSignal/errorOccurred/frameReceived/protocolMismatch/fatalIncompatibility` — keep as-is |
| Frame parser tests | `tests/frame_parser_test.cpp` | 25 cases; keep, extend pattern for new units |

---

## Phases

### Phase 0 — Shell rewrite & resources
**Goal:** replace skeleton `mainwindow.ui` stub with the real 5-tab shell + menu bar + icon, no per-tab logic yet.

- Rewrite `src/mainwindow.ui`: top-level `QTabWidget` with 5 empty `QWidget` pages named `Status / Current Measurement / General Settings / Sensor Settings / About`. Add `QMenuBar` with `Help` menu containing `Debug Log…` (Ctrl+L) and `About` actions.
- Add icon: place `screenshots/Icon.png` (or resized copy) under `resources/icons/robin_wsc.png`; register in `resources/robin_wsc.qrc`. Wire as window/app icon in `MainWindow` constructor and `main.cpp` `QApplication::setWindowIcon`.
- Add a slim non-modal "banner" `QLabel` (hidden by default, top of central widget) for protocol-mismatch / no-device / many-devices messages. Single helper `MainWindow::showBanner(QString, BannerKind)`.
- **Verify:** `cmake --build build/release` succeeds; `robin_wsc.exe` runs, shows 5 named tab headers + window icon + Help menu (entries do nothing yet).

### Phase 1 — Connection lifecycle
**Goal:** boot the user-confirmed auto-connect flow and surface state.

- New `src/dialogs/device_picker_dialog.{h,cpp,ui}` — modal, lists `QSerialPortInfo` rows by `serialNumber()`, double-click or OK selects.
- In `MainWindow::initialise()` (called after `show()`): call `DeviceController::findDevices()`; 1 → auto-`connectTo`; 0 → banner "No Robin Weather Station detected"; ≥2 → open picker, then `connectTo` chosen.
- Wire `Re-connect` button on Status tab (added in Phase 3) to repeat the same flow with `disconnect()` first.
- Hook existing `DeviceController::protocolMismatch` to non-modal banner; `fatalIncompatibility` to modal `QMessageBox::critical` then disable all tab content (keep only Status tab interactive).
- Status-bar text mirror of `connected/disconnectedSignal`.
- **Verify:** plug device → Status header turns green "Connected"; unplug → "Disconnected"; spoof second device via stub → picker appears.

### Phase 2 — DeviceController request/response API
**Goal:** turn the existing scaffold into a typed request layer the tabs can call.

- Extend `DeviceController` with one slot per logical command, each returning `void` and emitting a typed completion signal:
  - `requestStatus()` → `statusReceived(System_Ready_Status_t)` / `requestTimedOut(quint8 op)`
  - `requestWeather()` → `weatherReceived(Weather_Data_Packed_t)`
  - `requestMeta()` → `metaReceived(Meta_Data_t)`
  - `setMeta(Meta_Data_t)` → `metaWriteAck(bool ok, quint8 nakCode)`
  - `requestRtc()` → `rtcReceived(RTC_DateTime_t)` (already declared as slot — implement)
  - `setRtc(RTC_DateTime_t)` → `rtcWriteAck(bool ok)`
  - `clearDatabase()` → `dbFlushAck(bool ok)`
- Cache the most recent `Meta_Data_t` inside `DeviceController` (`m_metaCache`) — both settings tabs read/write through this cache so they share one round-trip on Discard. `metaCache()` getter; `invalidateMetaCache()` on disconnect.
- Add per-request timeout: a single `QTimer` keyed on outstanding opcode (max one in-flight; queue if multiple). 1500 ms default. Timeout emits `requestTimedOut(opcode)` and the originating slot's failure signal.
- Use the existing `frameReceived(quint8, QByteArray)` signal as input; route by opcode using `static_cast` + `STATIC_ASSERT(sizeof(...)==N)` (the structs already carry these asserts in `protocol.h`).
- **Verify:** add `tests/device_controller_test.cpp` driving `DeviceController` against a fake `QIODevice` that produces canned frames; assert each request emits its typed signal and timeouts fire.

### Phase 3 — Status tab
**Goal:** match `screenshots/Status_Tab.png`.

- New `src/tabs/status_tab.{h,cpp,ui}` (`QWidget` subclass).
- Layout: header row "Device status: <text>" + `Re-connect` button right-aligned; grid of 11 status rows (each = small colored `QLabel` swatch + name) in a 6×2 layout matching the screenshot order; horizontal separator; "Computer Date-Time" + "Device RTC Date-Time" rows; `Update RTC to the Computer Time` button right-aligned; separator; red `Clear Database` button bottom-left.
- `QTimer` 2 s polling `requestStatus()` while tab visible; pause on `tabBar::currentChanged` away.
- `Update RTC` builds `RTC_DateTime_t` from `QDateTime::currentDateTime()` and calls `setRtc()`. On ack, immediately `requestRtc()` to refresh display.
- `Clear Database` opens `QMessageBox::warning` with `Yes/No` confirmation; on confirm, `clearDatabase()`. On ack, briefly highlight the button.
- `Re-connect` calls into Phase-1 flow.
- **Verify:** all 11 indicators colour from a live `System_Ready_Status_t`; RTC clock visibly ticks (driven by Computer Date-Time `QTimer` 1 s); Update RTC pushes computer time to device and re-reads it.

### Phase 4 — Current Measurement tab
**Goal:** match `screenshots/Measurement_Tab.png`.

- New `src/tabs/measurement_tab.{h,cpp,ui}`.
- `QTableWidget` 3 columns × 7 rows (Sensor / Value / Unit), header non-editable, alternating row tint, no selection. Rows pre-seeded with sensor names + units (Temperature/Celsius, Humidity/%Rh, Pressure/kPa, Light/uMol/m2s, Rain Guage/mm/Hr, Dew Point/Celsius, BUS Value/-).
- `QTimer` 1 s `requestWeather()` while visible.
- Convert `Weather_Data_Packed_t` Q9.7 fixed-point fields using `lib/utils/fixedptc.h` semantics — host-side helper `qint16_to_float(value) = value / 128.0`. `light_par` is `uint16_t` linear; `bus_value` is also Q9.7 displayed integer (per screenshot showing `0`).
- **Verify:** values update once per second; reading a captured sample frame produces the expected decimal values; Q9.7 conversion unit-tested.

### Phase 5 — General Settings tab
**Goal:** match `screenshots/General_Settings_Tab.png`; uses shared `m_metaCache`.

- New `src/tabs/general_settings_tab.{h,cpp,ui}`.
- Widgets: `QSpinBox` Region ID (0..65535), `QSpinBox` Station ID (0..65535), `QComboBox` Sampling Interval with items `[1,5,10,15,30,60]` minutes (display "N minutes", `userData = uint8_t`), `QLineEdit` Server DNS (max 63 chars, `setMaxLength(63)`), `QLineEdit` Sensor Upload Path (max 63), `QLineEdit` Firmware Path (max 63). Bottom right: `Discard` and `Apply` buttons.
- On tab show OR explicit Discard: call `requestMeta()`; populate widgets from received `Meta_Data_t` (these 6 fields only — leave calibration alone).
- `Apply`: copy widgets into a working `Meta_Data_t` cloned from `m_metaCache` (preserving calibration set by Phase 6), call `setMeta()`. Show non-modal toast on ack/nak.
- Dirty tracking: change-signal on each widget toggles Apply enabled state; closing/leaving tab while dirty → `QMessageBox::question` "Discard unsaved changes?".
- **Verify:** load → edit fields → Apply → reconnect → values persist; Discard restores from device.

### Phase 6 — Sensor Settings tab
**Goal:** match `screenshots/Sensor_Settings_Tab.png`; shares `m_metaCache` with Phase 5.

- New `src/tabs/sensor_settings_tab.{h,cpp,ui}`.
- Widgets (all QDoubleSpinBox 2 decimals except Light): Temp Adjust (-50.00..50.00 °C), Humidity Adjust (-50.00..50.00 %Rh), Pressure Adjust (-20.00..20.00 kPa), **Light Adjust QSpinBox** (-32768..32767, integer — protocol type is `int16_t`), Rain-Gauge Adjust (-20.00..20.00 mm/Hr). Bottom right: `Discard` / `Apply`.
- Same load-from-`requestMeta()`, save-via-`setMeta()` pattern as Phase 5; `Apply` clones `m_metaCache` and overwrites only the 5 calibration fields so it doesn't stomp Phase-5 settings.
- Same dirty tracking + leave-prompt as Phase 5.
- **Verify:** edit calibration on this tab while General Settings tab is also dirty → each Apply preserves the other tab's edits (manual test); round-trip via reconnect persists values.

### Phase 7 — About tab
**Goal:** match `screenshots/About_Tab.png`. Trivial.

- New `src/tabs/about_tab.{h,cpp,ui}`.
- Centered: icon (left) + "Weather Station Control Center" header, "Copyright (c) 2026 by RobinLab", "This program should be used with the weather-station device connected through USB", and version line `"Version " + QString(ROBIN_WSC_VERSION_STRING)` from `app_info.h`.
- **Verify:** version string matches CMake-injected value.

### Phase 8 — Debug Log dialog
**Goal:** surface the existing `LogBuffer` from the Help menu.

- New `src/dialogs/log_viewer_dialog.{h,cpp,ui}`: non-modal, `QPlainTextEdit` (read-only, monospace), `Clear` button, `Copy All` button, "Auto-scroll" checkbox.
- On open: prime with `LogBuffer::snapshot()`; subscribe to `entryAdded(Entry)` via `Qt::QueuedConnection` for live tail. Disconnect on close.
- Wire Help → Debug Log… (Ctrl+L) action from Phase 0.
- Add log calls in `DeviceController` (connect / disconnect / opcode sent / opcode received / timeout / nak code) and in connection lifecycle (Phase 1).
- **Verify:** Ctrl+L opens dialog; performing any action causes a new line; Copy All puts text on clipboard.

### Phase 9 — Build, package, polish
**Goal:** clean delivery.

- Update root `CMakeLists.txt` to add new sources (5 tab classes, 2 dialog classes) and the qrc icon.
- Extend `tests/CMakeLists.txt` for `device_controller_test.cpp` and any small `q97_conversion_test.cpp`.
- Run `cmake --build build/release` then `cmake --install build/release --prefix dist/RobinWSC-1.0.0-win64`; sanity-check the relocatable folder runs on a clean profile.
- Confirm `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wnull-dereference -Wdouble-promotion` clean (the project already enforces this).

---

## Files added / modified summary

| Path | Action |
|---|---|
| `src/mainwindow.{h,cpp,ui}` | Heavy rewrite (Phase 0) |
| `src/main.cpp` | Set window icon (Phase 0) |
| `src/device_controller.{h,cpp}` | Extend API + meta cache + timeout (Phase 2) |
| `src/tabs/status_tab.{h,cpp,ui}` | NEW (Phase 3) |
| `src/tabs/measurement_tab.{h,cpp,ui}` | NEW (Phase 4) |
| `src/tabs/general_settings_tab.{h,cpp,ui}` | NEW (Phase 5) |
| `src/tabs/sensor_settings_tab.{h,cpp,ui}` | NEW (Phase 6) |
| `src/tabs/about_tab.{h,cpp,ui}` | NEW (Phase 7) |
| `src/dialogs/device_picker_dialog.{h,cpp,ui}` | NEW (Phase 1) |
| `src/dialogs/log_viewer_dialog.{h,cpp,ui}` | NEW (Phase 8) |
| `resources/robin_wsc.qrc` | Add icon entry (Phase 0) |
| `resources/icons/robin_wsc.png` | NEW from `screenshots/Icon.png` (Phase 0) |
| `tests/device_controller_test.cpp` | NEW (Phase 2) |
| `tests/q97_conversion_test.cpp` | NEW (Phase 4) |
| `CMakeLists.txt` (root) | Add new sources (Phase 9) |
| `tests/CMakeLists.txt` | Register new tests (Phases 2, 4) |

Untouched: `frame_parser.*`, `device_io.*`, `log_buffer.*`, `app_info.h`, `tests/frame_parser_test.cpp`.

---

## End-to-end verification

1. `wsl bash //scripts/run_native_tests.sh` for firmware-side mocked tests still green (we didn't touch firmware).
2. From `host/`: `cmake --build build/release` clean.
3. `ctest --test-dir build/release` — frame parser, device controller, Q9.7 conversion all pass.
4. Plug a real Robin Weather Station via USB, launch `robin_wsc.exe`:
   - Status tab shows green for present subsystems, RTC ticks, Update RTC writes computer time, Clear Database confirms then succeeds.
   - Current Measurement values update every 1 s with sane numbers.
   - General Settings: load values, change Region ID, Apply, power-cycle device, reload — value persists.
   - Sensor Settings: change Temp Adjust, Apply; verify Current Measurement Temperature shifts by that offset.
   - About tab shows version string matching CMake.
   - Help → Debug Log shows live trace of every request/response.
5. Unplug device while running → banner appears, tabs gracefully stop polling, no crash.
6. Plug a second device → next Re-connect opens picker dialog.
