# Server-Side Implementation Plan

> Phased plan for building the IoT data server specified in `Server_Architecture.md`.
> Source tree lives in `html/`. Production host: `akp@robin-gpu.cpe.ku.ac.th` (Ubuntu).
> Deploy via SSH (`ssh akp@robin-gpu.cpe.ku.ac.th`) + `git pull` + `systemctl restart` until Phase 5 introduces CI/CD.
>
> Implement phases **strictly in order**. Each phase must pass all listed verifications before the next begins.
> **Architecture reference:** `Server_Architecture.md` (sections cited as §X).
> **Test harness:** `Server_Test_Plan.md` (Python black-box verifiers in `server_test/`).

---

## Target Directory Layout (inside `html/`)

```
html/
├── app/                         ← FastAPI application package
│   ├── __init__.py
│   ├── main.py                  ← FastAPI() factory, router mounts, lifespan hooks
│   ├── config.py                ← Pydantic settings (env-driven; includes FIRMWARE_DIR)
│   ├── deps.py                  ← shared dependencies (db pool, auth)
│   ├── routers/
│   │   ├── weather.py           ← POST /api/v1/weather/upload  (mTLS)
│   │   ├── ota.py               ← GET / , GET /get_firmware    (mTLS)
│   │   └── admin.py             ← /admin/login, /admin/firmware/*, /admin/campaign/*  (JWT)
│   ├── db/
│   │   ├── pool.py              ← asyncpg pool lifecycle
│   │   ├── schema.sql           ← devices, weather_records, ingest_log, ota_campaigns, admin_users
│   │   ├── migrations/          ← numbered .sql files; applied by scripts/migrate.py
│   │   └── queries.py           ← parameterized SQL (no ORM; asyncpg directly)
│   ├── auth/
│   │   ├── mtls.py              ← verify X-SSL-Client-Verify header (Nginx-set)
│   │   └── jwt.py               ← bcrypt, token issue/verify, RBAC dependency
│   ├── ota/
│   │   ├── parser.py            ← binary struct.unpack for Weather_Data_Packed_t
│   │   ├── fixedpt.py           ← S9.7 fixed-point → float converter (matches lib/utils/fixedptc.h)
│   │   ├── crc32.py             ← CRC-32/MPEG-2 (matches shared/crc32.c)
│   │   └── campaign.py          ← active-campaign selection per device
│   └── templates/               ← Jinja2 + HTMX fragments for admin UI
├── firmware/                    ← firmware binaries (NOT in git; created at deploy time)
│   └── v{version}.bin           ← current firmware; only one file kept; NOT web-accessible
├── logs/                        ← structured JSON logs (NOT in git; created at deploy time)
├── pki/                         ← CA certs, CRL, device certs (NOT in git; mode 0700)
│   ├── ca.crl                   ← CRL updated weekly by scripts/refresh_crl.sh
│   └── private_ca_chain.pem
├── etc/                         ← runtime config (NOT in git; created at provisioning time)
│   └── iot.env                  ← DB DSN, JWT secret, FIRMWARE_DIR, other secrets
├── nginx/
│   └── iot_server.conf          ← mTLS termination, proxy, rate limiting, /firmware/ deny
├── systemd/
│   └── iot-server.service       ← gunicorn+uvicorn workers, restart-on-failure
├── scripts/
│   ├── migrate.py               ← idempotent SQL migrations
│   ├── provision_ca.sh          ← root + intermediate CA setup (offline root)
│   ├── issue_device_cert.sh     ← per-device cert + CSR signing
│   └── deploy.sh                ← git pull + pip sync + systemctl restart
├── tests/                       ← server-side pytest (unit tests for parser, crc32, fixedpt)
├── requirements.txt
├── pyproject.toml               ← ruff + black config
└── README.md                    ← ops runbook
```

**Pre-existing placeholder dirs** (`html/admin/`, `html/api/v1/weather/`) will be removed in Phase 1; routes are organised in `app/routers/`, not by URL-mirrored filesystem paths.

---

## Phase 0 — Host Provisioning

Prepare the Ubuntu server before any application code runs.

> **Ownership note:** During development `html/` and all runtime subdirs are owned by `akp`. The `iotsrv` service user and ownership transfer apply to production deployment only (S0-3).

- [ ] S0-1 Confirm host OS (`lsb_release -a`), Python ≥ 3.12 available, systemd present
- [ ] S0-2 Install system packages: `nginx`, `postgresql-17` (target host standard), `python3.12-venv`, `certbot`, `git`. **TimescaleDB is built from source** on this deployment (no `timescaledb-*` apt package); follow the TimescaleDB "build from source" procedure against the installed PostgreSQL 17 headers — see Arch §10 Q-S12
- [ ] S0-3 **Production only:** create dedicated service user `iotsrv` with no login shell; transfer ownership of `html/` to `iotsrv` (`chown -R iotsrv:iotsrv html/`)
- [ ] S0-4 Create runtime subdirs under `html/`: `firmware/` (mode `0750`), `logs/`, `pki/` (mode `0700`), `etc/`; owner is `akp` in development, `iotsrv` in production; confirm all four are listed in `.gitignore`
- [ ] S0-5 Enable UFW: allow 22, 443; deny 8000 (FastAPI listens only on 127.0.0.1)
- [ ] S0-6 Initialize TimescaleDB extension in `weather` database; in development use role `akp`, in production create role `iotsrv` with password from env vault
- [ ] S0-7 Verification: `psql -U akp -d weather -c "SELECT extversion FROM pg_extension WHERE extname='timescaledb';"` returns a version (substitute `iotsrv` in production) ✓

---

## Phase 1 — Application Skeleton

Stand up a minimal FastAPI app reachable only via Nginx. No device logic yet.

- [ ] S1-1 Scaffold `html/app/` per directory layout; add `requirements.txt` pinning FastAPI, uvicorn, gunicorn, asyncpg, pydantic-settings, PyJWT, bcrypt, jinja2, python-multipart
- [ ] S1-2 `app/main.py`: create `FastAPI()`, mount empty routers, add `/health` returning `{"status": "ok"}`
- [ ] S1-3 `app/config.py`: Pydantic `BaseSettings` loading from `html/etc/iot.env` (DB DSN, JWT secret, `FIRMWARE_DIR` — **must be an absolute path**, resolved via `Path(value).resolve(strict=False)` at startup (Arch §10 Q-S11); the app refuses to start if `FIRMWARE_DIR` is not absolute or not writable; `FIRMWARE_KEEP_N` (default `3`, see Arch §3.4 and Arch §10 Q-S4); `SLOT_LEN_SEC` (default `43200`; must match device firmware upload cadence); `MAX_FIRMWARE_SIZE_BYTES` (default `491520` = 480 KB; matches the STM32L476RG app Flash partition — Arch §3.4 "Firmware size ceiling"))
- [ ] S1-4 `systemd/iot-server.service`: `gunicorn app.main:app -k uvicorn.workers.UvicornWorker -w 2 -b 127.0.0.1:8000`; run as `akp` in development, `iotsrv` in production
- [ ] S1-5 `scripts/deploy.sh`: `git pull` → `pip install -r requirements.txt` → `sudo systemctl restart iot-server` (system unit, not `--user`; matches S1-4)
- [ ] S1-6 Commit and SSH-deploy to host; enable service (`systemctl enable --now iot-server`)
- [ ] S1-7 Verification: `curl http://127.0.0.1:8000/health` on host returns `200 {"status":"ok"}` ✓

---

## Phase 2 — Database Schema & Migrations

Create the schema defined in `Server_Architecture.md` §3.1 and §3.3 and wire up the asyncpg pool.

- [ ] S2-1 `app/db/schema.sql`: `devices` (`UNIQUE (region_id, station_id)`; no `cn` column — fleet shares one cert, no per-device CN exists), `weather_records` (hypertable + compression policy), `ingest_log`, `ota_campaigns` (with `firmware_size`, `firmware_sha256`, `rollout_start`, `rollout_window_days INT DEFAULT 10`, `slot_len_sec INT NOT NULL DEFAULT 43200`, `target_cohort_ids TEXT[]`, `status` enum values `draft`/`scheduled`/`in_progress`/`paused`/`completed`/`cancelled` — terminal `cancelled` replaces the earlier `rolled_back` name per Arch §10 Q-S8 — see §3.3 and §3.4; **no** `target_cohort_size` column), `download_completions` (`campaign_id INT REFERENCES ota_campaigns(id)`, `device_id VARCHAR(6)`, `chunk_index INT NOT NULL`, `recorded_at TIMESTAMPTZ DEFAULT now()`, `PRIMARY KEY (campaign_id, device_id, chunk_index)` — written by the `/get_firmware` handler on every successfully served chunk; source of truth for `success_rate` and live rollout progress — Arch §10 Q-S13), `admin_users`
- [ ] S2-2 `scripts/migrate.py`: applies numbered files from `app/db/migrations/` inside a transaction; records state in `schema_migrations` table
- [ ] S2-3 `app/db/pool.py`: `asyncpg.create_pool(min=2, max=20)` in FastAPI lifespan startup; close on shutdown
- [ ] S2-4 Seed admin user: one bcrypt-hashed row inserted via migration (credentials from env at provisioning time only)
- [ ] S2-5 Verification: `python -m scripts.migrate --dry-run` prints the DDL; run for real; `\dt` in psql shows all five tables ✓
- [ ] S2-6 Verification: `SELECT * FROM timescaledb_information.hypertables WHERE hypertable_name='weather_records';` returns one row ✓

---

## Phase 3 — mTLS Ingestion Path (`/api/v1/weather/upload`)

Implements the binary upload flow from §3.1 and §6.

- [ ] S3-1 `app/auth/mtls.py`: verify `X-SSL-Client-Verify == SUCCESS` (Nginx-set); return 403 on missing/failed; dependency-injectable. No CN extraction — no per-device CN exists (Arch §2.1); device identity comes from payload `(region_id, station_id)` only
- [ ] S3-2 `app/ota/fixedpt.py`: convert S9.7 fixed-point (int16) → float using `value / 128.0`; mirror `lib/utils/fixedptc.h` sign-extension
- [ ] S3-3 `app/ota/parser.py`: `parse_upload(payload: bytes) -> (region, station, list[dict])` using `struct.Struct("<HHB")` for header + 18-byte chunks; reject if length ≠ `5 + 18*count`
- [ ] S3-4 `app/routers/weather.py`: `POST /api/v1/weather/upload` — parse header for (region, station) → `upsert_device_by_region_station()` → idempotency check using `{region:03d}{station:03d}:{first_sample_ts}` key → `INSERT ... ON CONFLICT DO NOTHING` for weather + ingest_log in a single transaction. The ingest, OTA metadata poll, and OTA chunk download all share the `/api/v1/weather/` prefix (Arch §10 Q-S1 Option B), so they live behind one Nginx location block and one rate-limit zone
- [ ] S3-5 Timestamp conversion: Y2K epoch (seconds since 2000-01-01 UTC) → `TIMESTAMPTZ`; use `datetime(2000,1,1,tzinfo=UTC) + timedelta(seconds=ts)`
- [ ] S3-6 Update `devices.last_seen` on every successful ingest (same transaction); auto-create the row via upsert when a new `(region_id, station_id)` first reports
- [ ] S3-7 Verification (unit, `html/tests/`): parse a fixture byte-string produced from a known `Weather_Data_Packed_t` and assert all fields round-trip within ±1 LSB ✓
- [ ] S3-8 Verification (integration, `server_test/`): see test harness plan T1-series ✓

---

## Phase 4 — Nginx & mTLS Termination

Bring traffic through Nginx; enforce client cert validation.

- [ ] S4-1 `scripts/provision_ca.sh`: generate offline root (kept on air-gapped USB), online intermediate signed by root, both as PEM
- [ ] S4-2 `scripts/issue_device_cert.sh <CN>`: generate device key, CSR, sign with intermediate, emit PEM bundle + private key
- [ ] S4-3 `nginx/iot_server.conf`: **two `server{}` blocks** (Arch §5, Q-S9). Device vhost (`api.iot.example.com`): TLS 1.2+1.3, private-CA server cert, `ssl_client_certificate {html_dir}/pki/private_ca_chain.pem`, `ssl_crl {html_dir}/pki/ca.crl`, `ssl_verify_client on`, a **single** `location /api/v1/weather/` block (covering ingest + OTA per Q-S1 Option B) that enforces `if ($ssl_client_verify != SUCCESS) return 403` and forwards `X-SSL-Client-Verify`, plus `location /firmware/ { deny all; return 404; }`. Admin vhost (`admin.iot.example.com`): Let's Encrypt server cert, `ssl_verify_client off`, `location /admin/ { proxy_pass ... }` only — **no** client-cert headers forwarded
- [ ] S4-4 Rate limit: `limit_req_zone $arg_id zone=device_api:10m rate=10r/s; limit_req zone=device_api burst=20` — **per-device** throttle (Arch §10 Q-S3). Keyed on the `?id=` query parameter that Phase 3.1 firmware guarantees on every device request; do **not** key on `$ssl_client_s_dn` (fleet shares one cert per Arch §2.1)
- [ ] S4-5 Weekly CRL refresh: systemd timer runs `scripts/refresh_crl.sh` → regenerates CRL from revoked-cert table → `nginx -s reload`
- [ ] S4-6 Verification: `openssl s_client -cert valid.pem -key valid.key -servername api.iot.example.com -connect HOST:443` completes handshake; device path request reaches FastAPI and `X-SSL-Client-Verify: SUCCESS` appears in access log ✓
- [ ] S4-7 Verification: request to `https://api.iot.example.com/api/v1/weather/...` without `-cert` flag returns `403` (mandatory mTLS on the device vhost); request to `https://admin.iot.example.com/admin/` without `-cert` flag reaches FastAPI and returns `200` or `401` — admin vhost is a separate listener with `ssl_verify_client off` (Arch §5, Q-S9) ✓
- [ ] S4-8 Verification: confirm the admin vhost forwards no `X-SSL-Client-*` headers upstream and serves a Let's Encrypt chain (`openssl s_client -servername admin.iot.example.com ...` shows the public CA) ✓

---

## Phase 5 — OTA Endpoints (`/`, `/get_firmware`)

Implements §3.2 and §3.3 device-facing OTA. Firmware ingestion (admin side) is Phase 7.

- [ ] S5-1 `app/ota/crc32.py`: table-based CRC-32/MPEG-2 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection, no final XOR); verify against fixtures generated by `shared/crc32.c`
- [ ] S5-2 `app/ota/campaign.py`: `get_active_campaign_for_device(device_id: str) -> Campaign | None` — `device_id` is the 6-char `"{region:03d}{station:03d}"` passed via the `?id=` query param (Arch §3.2); picks highest `version` among `status='in_progress'` campaigns the device is eligible for. Cohort match uses `target_cohort_ids IS NULL OR cardinality(target_cohort_ids) = 0 OR device_id = ANY(target_cohort_ids)` (Arch §10 Q-S10)
- [ ] S5-3 `app/ota/campaign.py` `compute_wait(device_id, campaign) -> int` — Arch §3.3 slot algorithm; `slot_len = campaign.slot_len_sec` (frozen at creation), `num_slots = rollout_window_days * 2`, `dev_slot = zlib.crc32(device_id.encode('ascii')) % num_slots`, `now_slot = min(num_slots-1, max(0, int((now - rollout_start).total_seconds() // slot_len)))`; returns `0` if `dev_slot <= now_slot` else `(dev_slot - now_slot) * slot_len`. Monotone-in-time ⇒ failed devices retry automatically next upload cycle
- [ ] S5-4 `app/routers/ota.py` `GET /api/v1/weather/?id=<rrrsss>` (mTLS; Q-S1 Option B): reads `id` query param (6-char decimal, regex `^[0-9]{6}$`; reject with 400 on malformed); resolves campaign via S5-2; calls `compute_wait()`; returns `HTMLResponse` whose body contains `V.<version>:L.<size>:H.<sha256hex>:W.<seconds>`. Return `<html><body>No update available</body></html>` when no match (token absent → device treats as `W.0`, no update)
- [ ] S5-5 `GET /api/v1/weather/get_firmware?offset=X&length=Y&id=<rrrsss>` (mTLS; Q-S1 Option B): reads `id` query param; re-runs `compute_wait()`; if `W > 0` return `429 Too Many Requests` with `Retry-After: <seconds>` header (Arch §3.2); else opens file at the **absolute** `firmware_file_path` (Arch §10 Q-S11), `seek(offset)`, `read(length)`, appends 4-byte little-endian CRC-32/MPEG-2, returns `application/octet-stream`; after serving the chunk body, `INSERT INTO download_completions (campaign_id, device_id, chunk_index) VALUES (...) ON CONFLICT DO NOTHING` where `chunk_index = offset // 512` (Arch §10 Q-S13)
- [ ] S5-6 Clamp `length` to [1, 512]; reject `offset + length > file_size` with 416; reject missing/malformed `id` with 400; reject when no active campaign for device with 404; enforce slot gate with 429 (S5-5)
- [ ] S5-7 Stream reads use `aiofiles` or blocking read inside `run_in_executor` to avoid stalling the event loop
- [ ] S5-8 Verification: see `server_test/` T2-series (metadata regex match incl. optional `W` field, resumable chunked download, CRC + SHA-256 reconstruction, 429 on out-of-slot GET) ✓

---

## Phase 6 — Admin Authentication & RBAC

JWT-based login for human operators. Must be in place before Phase 7 (firmware upload).

- [ ] S6-1 `app/auth/jwt.py`: login endpoint verifies bcrypt hash, issues HS256 JWT with `sub`, `role`, `exp` (24 h)
- [ ] S6-2 Dependency `require_role("admin"|"operator"|"viewer")` — 401 on missing/invalid token, 403 on insufficient role
- [ ] S6-3 `/admin/login` (form POST), `/admin/logout`, `/admin/me` (returns current user) — all under plain TLS, no mTLS
- [ ] S6-4 Password reset: out-of-band only for v1 (admin updates DB row); in-app reset deferred to Phase 9
- [ ] S6-5 Verification: valid creds → 200 + JWT; wrong password → 401; role-restricted endpoint with `viewer` token → 403 ✓

---

## Phase 7 — Admin OTA Campaign Management

- [ ] S7-1 `POST /admin/firmware/upload` (admin only): accepts **only the binary file** (no version field); **rejects with `413 Request Entity Too Large` if `len(file_bytes) > settings.MAX_FIRMWARE_SIZE_BYTES`** (default 480 KB, the STM32L476RG app Flash partition — Arch §3.4 "Firmware size ceiling"); auto-assigns `new_version = MAX(ota_campaigns.version) + 1` (1 on first upload); auto-computes SHA-256 and byte size; writes `{FIRMWARE_DIR}/v{new_version}.bin` atomically (`tmp` + `os.replace()`); inserts draft campaign row with `slot_len_sec = settings.SLOT_LEN_SEC`; then runs `_sweep_firmware_retention(settings.FIRMWARE_KEEP_N)` — deletes `.bin` files for **terminal** (`completed`/`cancelled`) campaigns older than the `FIRMWARE_KEEP_N` most recent terminal campaigns; **never** touches draft/in_progress/paused campaigns (Arch §3.4, §10 Q-S4). Returns `version`, `firmware_sha256`, `firmware_size`. Add `get_max_firmware_version()` and `list_campaigns_by_status()` DB helpers to `app/db/queries.py`.
- [ ] S7-2 `POST /admin/campaign/{id}/start`: admin-only; accepts optional `rollout_window_days` (default 10, min 1, max 30), optional `slot_len_sec` (override; default from `settings.SLOT_LEN_SEC`), and optional `target_cohort_ids TEXT[]` (6-char `rrrsss` list; **empty list is normalised to NULL before insert** so "whole fleet" has a single canonical representation — Arch §10 Q-S10); sets `status='in_progress'`, `rollout_start=now()`; transitional states validated (`draft → in_progress` only)
- [ ] S7-3 `POST /admin/campaign/{id}/pause`, `POST /admin/campaign/{id}/resume`, `POST /admin/campaign/{id}/cancel` — operator role or higher. (Formerly `/rollback` — renamed to `/cancel` per Arch §10 Q-S8; the terminal status column is now `cancelled`.) Pause/resume do **not** reset `rollout_start`; resuming continues from the original slot clock so already-eligible devices remain eligible. `cancel` is terminal and runs `_sweep_firmware_retention()` so any binary that falls outside the keep-N window is reclaimed
- [ ] S7-3.6 On terminal status transition (`completed` or `cancelled`): call `_compute_success_rate(campaign)` — query count of distinct `device_id` values in `download_completions` where `campaign_id = {id}` and the device appears in all `(firmware_size + 511) // 512` `chunk_index` rows (ceiling division per Q-S6) — and write the result to `ota_campaigns.success_rate`. This is the **status-change path** chosen for Q-S5 (Arch §10). No background scheduler required; `success_rate` stays `NULL` during the active rollout window and is finalised at campaign close
- [ ] S7-4 `GET /admin/campaign/{id}`: returns campaign row + aggregate download progress (count of distinct `device_id` values in `download_completions` where `campaign_id = {id}` and `chunk_index` count equals `(firmware_size + 511) // 512` — ceiling division per Q-S6; uses the `download_completions` table written by S5-5, Arch §10 Q-S13); include derived `current_slot`, `num_slots = rollout_window_days * 2`, and `eligible_device_count` for the admin dashboard. No per-device firmware version column — devices do not report FW_VERSION to the server
- [ ] S7-5 Enforce invariants: duplicate `version` rejected; firmware file size must match DB row; SHA-256 recomputed on start to catch tampering; both `rollout_window_days` and `slot_len_sec` are immutable after `start` (changing either mid-rollout would reshuffle `dev_slot` assignments and break monotone retry)
- [ ] S7-6 Verification: see `server_test/` T3-series (campaign lifecycle + firmware artefact integrity + slot schedule determinism + retention sweep preserves in-progress binaries) ✓

---

## Phase 8 — Admin UI (HTMX + Jinja2)

Minimum viable operator surface.

- [ ] S8-1 Login page (`/admin/login.html`); CSRF token on form POST
- [ ] S8-2 Dashboard: device list with `(region_id, station_id)` and `last_seen`; server-rendered table with HTMX `hx-get` pagination
- [ ] S8-3 Campaign form: file-only upload (no version field — version shown as "will be assigned automatically: v{max+1}"); progress via HTMX `hx-post` + SSE or polling; display assigned version and SHA-256 after upload completes; campaign list with start/pause buttons
- [ ] S8-4 Verification: end-to-end manual test from a browser — log in, upload firmware, start rollout, observe download-completion progress ✓

---

## Phase 9 — Observability

From §1 (Prometheus + Loki + Grafana). Deployable independent of earlier phases once app is stable.

- [ ] S9-1 Integrate `prometheus-fastapi-instrumentator`; expose `/metrics` bound to 127.0.0.1 only
- [ ] S9-2 Custom metrics: `ingest_chunks_total`, `ingest_duplicates_total`, `ota_chunks_served_total`, `cert_verify_failures_total{reason}`
- [ ] S9-3 Structured JSON logging to `html/logs/app.log`; Loki Promtail scrapes
- [ ] S9-4 Grafana dashboards: device heartbeat panel, OTA rollout progress per campaign, ingest lag p95
- [ ] S9-5 Alert rules: ingest lag > 5 min, cert-verify error rate > 1/min, OTA campaign success < 80 % after rollout completes at cohort ≥ 10 %. Alert reads `ota_campaigns.success_rate` which is populated on terminal-status transition (S7-3.6, status-change path per Q-S5). Live rollout progress during the 10-day window is monitored via the `GET /admin/campaign/{id}` derived aggregate — not via this column
- [ ] S9-6 Verification: synthetic duplicate upload → `ingest_duplicates_total` increments visible in Grafana ✓

---

## Phase 10 — CI/CD & Hardening (Optional, Post-v1)

- [ ] S10-1 GitHub Actions: lint (ruff) + unit tests (pytest) on PR; deploy workflow on `main` push
- [ ] S10-2 Blue/green: two systemd services on different ports; Nginx upstream swap + health-check gate
- [ ] S10-3 Automated PostgreSQL backups to off-site bucket; weekly restore-test
- [ ] S10-4 Ed25519 firmware signing (§3.2 Phase 5 note); bootloader verification; rotate signing key annually

---

## References

- **Architecture spec:** `Server_Architecture.md`
- **Binary schema:** `lib/utils/weather_data.h`, `lib/utils/fixedptc.h`
- **CRC-32 parity:** `shared/crc32.c`
- **OTA protocol:** `lib/A7670/a7670_ssl_downloader.h`, `Src/ota_manager_task.c`
- **Firmware-side status:** `IMPLEMENTATION_STATUS.md`
- **Test harness:** `Server_Test_Plan.md`

---

## Resolved Issues (Cross-Document Review — 2026-04-21)

Issues flagged during the IoT / Server consistency review. All decisions are now final; the body of this plan and `Server_Architecture.md` reflect the accepted resolutions. Original framings preserved for rationale.

1. **Success-rate source of truth — RESOLVED (Option A: counter table).** The `/get_firmware` handler inserts `(campaign_id, device_id, chunk_index)` into the `download_completions` table (`ON CONFLICT DO NOTHING`) on every successfully served chunk (S5-5). `success_rate` is computed from this table at terminal status transition (S7-3.6). Live progress in `GET /admin/campaign/{id}` (S7-4) also queries this table. Nginx access-log parsing is not used. Table schema added to S2-1 and Arch §3.3 (Q-S13).

2. **Device-side oversize image rejection — RESOLVED (Phase 3.2).** Device firmware rejects any `L.` value exceeding `FLASH_APP_SIZE_MAX` (480 × 1024 bytes) in the `POLLING_VERSION` state; the bootloader independently guards `image_size` before SHA-256 verification. Both checks use the `FLASH_APP_SIZE_MAX` constant to be added to `shared/fram_addresses.h`. Full implementation checklist: `IMPLEMENTATION_STATUS.md` Phase 3.2.

3. **Chunk-total arithmetic cohesion — RESOLVED (cohesion enforced).** Ceiling division `(firmware_size + 511) // 512` is the single formula used at all three sites: S5-6 (range/416 guard), S7-4 (live progress readout), and S7-3.6 (terminal `success_rate` computation). Enforced as a code-review checkpoint in S7-5 before Phase 7 sign-off.
