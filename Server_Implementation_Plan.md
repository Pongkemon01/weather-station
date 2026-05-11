# Server Implementation Plan

> Phase summary for the IoT data server. All server phases (S0–S11) are complete.
> Architecture: `Server_Architecture.md` · Test plan: `Server_Test_Plan.md` · Status: `IMPLEMENTATION_STATUS.md`.
> Host: `akp@robin-gpu.cpe.ku.ac.th` · Deploy: `bash html/scripts/deploy.sh` (scp; server has no git repo).

---

## Directory Layout (inside `html/`)

```
html/
├── app/                       FastAPI package
│   ├── main.py                ; app factory, router mounts, lifespan, logging_config bootstrap
│   ├── config.py              ; Pydantic settings — DB_DSN, JWT_SECRET, FIRMWARE_DIR (must be absolute),
│   │                          ;   FIRMWARE_KEEP_N=3, SLOT_LEN_SEC=43200, MAX_FIRMWARE_SIZE_BYTES=491520
│   ├── deps.py
│   ├── metrics.py             ; custom Prometheus counters/histograms/gauges
│   ├── logging_config.py      ; RotatingFileHandler → html/logs/app.log (JSON)
│   ├── templating.py          ; Jinja2 singleton
│   ├── routers/
│   │   ├── weather.py         ; POST /api/v1/weather/upload          (mTLS)
│   │   ├── ota.py             ; GET / , GET /get_firmware            (mTLS)
│   │   ├── admin.py           ; JSON admin API (auth, campaigns, users)
│   │   └── ui.py              ; HTML routes (login, dashboard, campaigns, sensor-data, users, profile)
│   ├── db/
│   │   ├── pool.py            ; asyncpg.create_pool(min=2, max=20) in lifespan
│   │   ├── schema.sql
│   │   ├── migrations/        ; numbered .sql files; tracked in schema_migrations
│   │   └── queries.py         ; parameterised SQL (no ORM)
│   ├── auth/
│   │   ├── mtls.py            ; verifies X-SSL-Client-Verify == SUCCESS
│   │   ├── jwt.py             ; bcrypt + HS256 + require_role
│   │   └── csrf.py            ; HMAC-signed double-submit
│   ├── ota/
│   │   ├── parser.py          ; struct.unpack for packed binary ingest
│   │   ├── fixedpt.py         ; S9.7 → float (mirrors lib/utils/fixedptc.h)
│   │   ├── crc32.py           ; CRC-32/MPEG-2 (mirrors shared/crc32.c)
│   │   └── campaign.py        ; get_active_campaign_for_device, compute_wait
│   └── templates/             ; Jinja2 base + partials
├── firmware/                  ; binaries — mode 0750; NOT in git
├── logs/                      ; JSON app log — NOT in git
├── pki/                       ; CA chain + CRL — mode 0700 — NOT in git
├── backups/                   ; daily pg_dump output — NOT in git
├── etc/                       ; iot.env — NOT in git
├── nginx/                     ; iot_server.conf, iot_upstream*.conf
├── systemd/                   ; iot-server-{blue,green}.service, refresh-crl, backup-db, restore-test units
├── monitoring/                ; prometheus/, loki/, promtail/, grafana/{dashboards,provisioning}
├── scripts/                   ; deploy, migrate, provision_ca, issue_device_cert, refresh_crl,
│                              ;   backup_db, restore_test, blue_green_deploy, generate_signing_key
├── tests/                     ; html-side unit tests (pytest)
├── requirements.txt
└── pyproject.toml
```

---

## Phase Summary

All phases complete. Verification suites in `server_test/` documented in `Server_Test_Plan.md`.

| Phase | Description | Status (date) |
|-------|-------------|---------------|
| S0  | Host provisioning — Debian 13, Python 3.13, PG 17 + TimescaleDB 2.27.0-dev from source, UFW (22 + 443 only), `weather` DB owned by `akp`, peer auth via Unix socket | ✓ 2026-04-26 |
| S1  | FastAPI scaffold, gunicorn + uvicorn worker, systemd unit, `/health`, `iot.env` loader | ✓ 2026-04-26 |
| S2  | Schema (`devices`, `weather_records` hypertable + 7-day compression policy, `ingest_log`, `ota_campaigns`, `download_completions`, `admin_users`, `schema_migrations`), asyncpg pool | ✓ 2026-04-26 |
| S3  | `POST /api/v1/weather/upload` — packed-struct parse, S9.7 → float, Y2K epoch → TIMESTAMPTZ, idempotency on `{rrr}{sss}:{first_ts.isoformat()}`, device upsert | ✓ 2026-04-26 |
| S4  | Two-listener Nginx (private CA device vhost, Let's Encrypt admin vhost), CRL refresh timer, fleet client cert issuance, DER conversion + C array generation for modem | ✓ 2026-05-06 |
| S5  | `GET /api/v1/weather/?id=` + `GET /api/v1/weather/get_firmware`, `compute_wait()` slot algo, 429 on out-of-slot, CRC-32/MPEG-2 trailer, `aiofiles` chunked read, `download_completions` insert on every served chunk | ✓ 2026-05-06 |
| S6  | JWT auth — `login`, `logout`, `me`, `users`. `require_role` factory with hierarchy viewer(0) < operator(1) < admin(2) | ✓ 2026-05-06 |
| S7  | Admin campaign API — upload (binary only, server assigns version, 413 over 480 KB, atomic `os.replace`, retention sweep), start (recompute SHA-256 from disk → 409 on mismatch, freeze `slot_len_sec`), pause/resume/cancel (rollout_start preserved across pause), success_rate on terminal transition | ✓ 2026-05-07 |
| S8  | Admin UI — login + CSRF, dashboard (device list HTMX-paginated), campaigns (upload form, detail with 15 s polling, lifecycle buttons). Cookie-based JWT (HttpOnly, SameSite=strict) | ✓ 2026-05-07 |
| S9  | Prometheus instrumentator on `/metrics` (loopback only, IP-guard middleware), custom metrics (`ingest_chunks_total`, `ingest_duplicates_total`, `ingest_lag_seconds`, `ota_chunks_served_total`, `cert_verify_failures_total{reason}`, `ota_campaign_success_rate`), JSON logging, Loki + Promtail, Grafana dashboard (8 panels), alert rules | ✓ 2026-05-07 |
| S10 | GH Actions CI (ruff + pytest) + deploy workflow; blue/green systemd units + swap script; daily pg_dump + weekly restore-test timers; Ed25519 firmware signing (server emits `v{n}.sig` when key set; bootloader verification deferred) | ✓ 2026-05-07 |
| S11 | Sensor data browse UI — `GET /admin/sensor-data` + `/admin/sensor-data/table` (HTMX partial). Filters: region_id, station_id, date range, BUS min/max. Default `date_from = today − 7 days`. `::date` cast in `_weather_filter()` for asyncpg type inference | ✓ 2026-05-07 |

User-management phases (UM1–UM5): see `User_Management_Implementation_Plan.md`.

---

## PKI Provisioning Runbook (one-time, condensed)

PKI artifacts live on an admin workstation, **not** the server. Generated via WSL Ubuntu (root path: `/home/akp/iot_pki/`).

1. `provision_ca.sh /home/akp/iot_pki` — root CA (4096-bit, mode 400), intermediate CA (2048-bit), `private_ca_chain.pem`, initial empty CRL.
2. **Move `root.key` to air-gapped USB**; `shred -u` on-disk copy.
3. `issue_device_cert.sh robin-gpu.cpe.ku.ac.th /home/akp/server_cert /home/akp/iot_pki` — server TLS cert for device vhost (uses `v3_device` extension; openssl warns but nginx serves correctly).
4. `issue_device_cert.sh iot-fleet /home/akp/fleet_cert /home/akp/iot_pki` — fleet client cert (365 days). Key **never copied to server**; stored on USB alongside root.key.
5. **Convert PEM → DER + C arrays** (`scripts/pem_to_c_array.sh`):

   | Source PEM | DER output | C array file | Symbol |
   |------------|-----------|--------------|--------|
   | `intermediate.crt` | `intermediate.der` (1132 B) | `lib/A7670/server_der.c` | `server_der[]` |
   | `iot-fleet.crt`    | `iot-fleet.der` (870 B)     | `lib/A7670/client_der.c` | `client_der[]` |
   | `iot-fleet.key`    | `iot-fleet-key.der` (1216 B)| `lib/A7670/client_key_der.c` | `client_key_der[]` |

6. Rebuild + flash firmware: `platformio run -t upload`. Modem calls `AT+CCERTDOWN` on cold boot; confirm with `AT+CCERTLIST`.
7. Copy `intermediate/`, `private_ca_chain.pem`, `ca.crl` (concatenated intermediate + root CRLs) to server `html/pki/`. Server TLS cert → `/etc/ssl/certs/`, key → `/etc/ssl/private/`.
8. Let's Encrypt admin vhost: `certbot certonly --nginx -d adm.robinlab.cc --agree-tos --non-interactive`.
9. Install `nginx/iot_server.conf` to `/etc/nginx/conf.d/`; `nginx -t && systemctl reload nginx`.
10. Install `refresh-crl.service` + `refresh-crl.timer` (weekly, `Persistent=true`, `RandomizedDelaySec=3600`).

**Renewal (annual).** Re-issue fleet cert with new output dir, regenerate DER + C arrays, rebuild, flash all units. Old cert can be revoked once all units update.
**Revocation = fleet-wide.** `openssl ca -revoke iot-fleet.crt -config /home/akp/iot_pki/intermediate/ca.conf` → `refresh_crl.sh` on server → reload nginx → all units blocked simultaneously. Issue new fleet cert and reflash.

---

## References

- `Server_Architecture.md` — full architecture spec
- `Server_Test_Plan.md` — verification harness (T0–T6)
- `IMPLEMENTATION_STATUS.md` — phase status
- `User_Management_Implementation_Plan.md` + `User_Management_Test_Plan.md` — UM phases & tests
- `lib/utils/weather_data.h`, `lib/utils/fixedptc.h` — binary schema source of truth
- `shared/crc32.c` — CRC-32/MPEG-2 firmware implementation
- `lib/A7670/a7670_ssl_downloader.h`, `Src/ota_manager_task.c` — device OTA path
