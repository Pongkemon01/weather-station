# Implementation Status

Single source of truth for phase completion. One line per phase.
Detail per phase lives in the relevant architecture / implementation-plan doc.

---

## Firmware (OTA)

Architecture: `OTA_Firmware_Architecture.md`

| Phase | Description | Code | Hardware test |
|-------|-------------|------|---------------|
| P0    | Prerequisite hardening (mutex, snapshot DB, watchdog, FRAM layout) | ✓ 2026-04-16 | — |
| P1    | Bootloader (dual-copy OCB, boot_flash, boot_fram, cache mgmt)      | ✓ 2026-04-16 | ⏳ P1-9..P1-12 pending |
| P2    | Download infra (CRC-32/MPEG-2, SSL downloader, image writer, SHA-256) | ✓ 2026-04-17 | ⏳ P2-5, P2-6 pending |
| P2.1  | Upload migration to `AT+HTTP*` (CCH eliminated; 512 B blob, 28 rec) | ✓ 2026-04-18 | ⏳ P2.1-10..12 pending |
| P3    | OTA manager task (8-state machine, version parse, NTP fix R-11)    | ✓ 2026-04-19 | ⏳ P3-7, P3-8 pending |
| P3.1  | Rollout gate (`W.<sec>`) + device identity (`?id=rrrsss`)           | ✓ 2026-04-21 | ⏳ P3.1-8..10 pending |
| P3.2  | Image size guard (`FLASH_APP_SIZE_MAX = 480 KB`)                    | ✓ 2026-05-11 (host 6/6) | ⏳ P3.2-6, P3.2-7 pending |
| P4    | Integration & field testing (end-to-end, power-loss, RDP1)          | — | ⏳ P4-1..P4-7 pending |

Build budget (current): app 22.7 % RAM / 9.3 % Flash; bootloader 2.8 % RAM / 0.7 % Flash.

---

## Server

Plan: `Server_Implementation_Plan.md` · Test plan: `Server_Test_Plan.md` · Host: `akp@robin-gpu.cpe.ku.ac.th`

| Phase | Description | Status |
|-------|-------------|--------|
| S0  | Host provisioning (Debian 13, PG 17 + TimescaleDB 2.27.0-dev, UFW)      | ✓ 2026-04-26 |
| S1  | FastAPI scaffold, gunicorn, systemd                                     | ✓ 2026-04-26 |
| S2  | DB schema (devices, weather_records hypertable, ota_campaigns, download_completions, admin_users, ingest_log), migrations, asyncpg pool | ✓ 2026-04-26 |
| S3  | mTLS ingestion `/api/v1/weather/upload`                                 | ✓ 2026-04-26 |
| S4  | Nginx mTLS termination, PKI (private CA + fleet client cert), CRL, Let's Encrypt admin vhost | ✓ 2026-05-06 |
| S5  | OTA device endpoints (`/`, `/get_firmware`) — slot algo, 429, CRC trailer | ✓ 2026-05-06 |
| S6  | Admin JWT auth + RBAC (admin / operator / viewer)                       | ✓ 2026-05-06 |
| S7  | Admin OTA campaign management REST API (upload, start, pause, cancel)   | ✓ 2026-05-07 |
| S8  | Admin UI (HTMX + Jinja2 — login, dashboard, campaigns)                  | ✓ 2026-05-07 |
| S9  | Observability (Prometheus + Loki + Promtail + Grafana, custom metrics, alerts) | ✓ 2026-05-07 |
| S10 | CI/CD (GH Actions), blue/green deploy, daily PG backups, Ed25519 signing | ✓ 2026-05-07 |
| S11 | Sensor data browse UI (filters: region, station, date range, BUS)        | ✓ 2026-05-07 |

---

## User Management

Plan: `User_Management_Implementation_Plan.md` · Test plan: `User_Management_Test_Plan.md`

| Phase | Description | Status |
|-------|-------------|--------|
| UM1 | DB query layer (7 functions in `html/app/db/queries.py`)                   | ✓ 2026-05-08 |
| UM2 | JSON API endpoints (4 new + list extension in `html/app/routers/admin.py`) | ✓ 2026-05-08 |
| UM3 | HTML UI routes (6 routes in `html/app/routers/ui.py`)                      | ✓ 2026-05-08 |
| UM4 | Templates (`users.html`, `profile.html`, partials, nav)                    | ✓ 2026-05-08 |
| UM5 | Modal routes (`new-modal`, `edit-modal` + `user_form_modal.html` partial)  | ✓ 2026-05-08 |

---

## Host Application (Robin WSC)

Plan: `host/host_implementation_plan.md` · Constraints: `host/CLAUDE.md` · Stack: Qt 6.8 Widgets + QSerialPort, C++17, MinGW-w64

Backend (completed before new UI plan):

| Phase | Description | Status |
|-------|-------------|--------|
| H0 | Project scaffold (CMake, Qt, warning flags, app_info.h, resource file, smoke-test MainWindow) | ✓ 2026-05-13 |
| H1 | Wire protocol layer — `FrameParser` (6-state D→H state machine, opcode-length table, resync) + 13 Qt Test cases | ✓ 2026-05-14 |
| H2 | Device communication — `DeviceIO` (IO thread, `QSerialPort`, H→D frame build, parser feed) + `DeviceController` (UI-thread owner, `QThread` lifecycle, all cross-thread signals `Qt::QueuedConnection`) | ✓ 2026-05-14 |

UI plan phases:

| Phase | Description | Status |
|-------|-------------|--------|
| Ph0 | Shell rewrite — 5-tab `QTabWidget` (Status / Current Measurement / General Settings / Sensor Settings / About), Help menu (Debug Log… Ctrl+L, About), window icon, banner `QLabel` | ✓ 2026-05-14 |
| Ph1 | Connection lifecycle — device picker dialog, auto-connect (0/1/2+ device logic), protocol-mismatch banner, status-bar mirror | — |
| Ph2 | DeviceController request/response API — typed slots per opcode, `m_metaCache`, 1500 ms per-request timeout | — |
| Ph3 | Status tab — 11 subsystem indicators, RTC display, Update RTC, Clear Database, Re-connect, 2 s poll | — |
| Ph4 | Current Measurement tab — 7-row sensor table, 1 s `REQ_WEATHER` poll, Q9.7 conversion | — |
| Ph5 | General Settings tab — Region/Station ID, Sampling Interval, server URLs; load/Apply/Discard via `m_metaCache` | — |
| Ph6 | Sensor Settings tab — 5 calibration adjusts; shares `m_metaCache` with Ph5 | — |
| Ph7 | About tab — icon, product name, copyright, version string | — |
| Ph8 | Debug Log dialog — `LogBuffer` viewer, live tail, Copy All, Ctrl+L wire-up | — |
| Ph9 | Build & package — new sources in CMakeLists, ctest clean, windeployqt portable zip | — |

Test coverage: `tests/frame_parser_test.cpp` — 13 Qt Test cases, 15/15 pass.

---

## Verification Suites

| Suite | Scope | Result |
|-------|-------|--------|
| T0  | Harness bootstrap (parity: CRC-32/MPEG-2, fixed-point S9.7, packed struct) | ✓ |
| T1  | Ingest path                | ✓ 9/9 |
| T2  | OTA download (incl. slot determinism, 429, resumable, completions tracking) | ✓ 18/18 |
| T3  | Admin campaign lifecycle   | ✓ 25/25 |
| T4  | mTLS + Nginx controls      | ✓ 6/6 (T4-3, T4-6 N/A) — 2026-05-07 |
| T5  | Soak + load + failure modes (`pytest -m slow`, 300 s soak) | ✓ 8/8 — 2026-05-07 |
| T6  | Sensor data browse UI       | ✓ 10/10 — 2026-05-07 |
| T7 / TUM0–11 | User management black-box  | ✓ 75/75 — 2026-05-08 |
| S6 / S8 unit | Admin auth (7) + admin UI E2E (14) | ✓ — 2026-05-07 |
| P3.2 host    | OTA size guard — `ovp_parse` + `FLASH_APP_SIZE_MAX` (native gcc / WSL) | ✓ 6/6 — 2026-05-11 |

---

## Outstanding

All on-target firmware integration tests (P1-9..P1-12, P2-5/6, P2.1-10..12, P3-7/8, P3.1-8..10, P3.2-6/7) and field/integration tests (P4-1..P4-7). Server side: complete.

Open risk: **R-12** — cert format. Verify `*_der.c` arrays actually contain DER (not PEM) bytes before next field deployment.

---

## Deploy

`bash html/scripts/deploy.sh` (scp + extract + `systemctl restart`; server has no git repo). SSH key `~/.ssh/akrapong.key`.
