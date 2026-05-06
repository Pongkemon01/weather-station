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

- [x] S0-1 Confirm host OS (`lsb_release -a`), Python ≥ 3.12 available, systemd present — **Debian 13 (trixie), Python 3.13.5, systemd 257** ✓
- [x] S0-2 Install system packages: `nginx`, `postgresql-17` (target host standard), `python3-venv`, `certbot`, `git`. **TimescaleDB built from source already present** (`~/timescaledb/`, v2.27.0-dev against PG17). Also installed: `ufw`, `build-essential`, `libpq-dev` ✓
- [x] S0-3 **Production only:** create dedicated service user `iotsrv` with no login shell; transfer ownership of `html/` to `iotsrv` (`chown -R iotsrv:iotsrv html/`) — **SKIPPED (dev mode; `akp` owns `html/`)** ✓
- [x] S0-4 Create runtime subdirs under `html/`: `firmware/` (mode `0750`), `logs/`, `pki/` (mode `0700`), `etc/`; all four listed in `html/.gitignore`. **Server `html/` is at `~/html/` (not `~/weather-station/html/`); dirs created with correct permissions** ✓
- [x] S0-5 Enable UFW: allow 22, 443; deny 8000 — **UFW installed and active; rules confirmed** ✓
- [x] S0-6 Initialize TimescaleDB extension in `weather` database; created `akp` PostgreSQL role with LOGIN+CREATEDB; created `weather` database owned by `akp`; `CREATE EXTENSION timescaledb` succeeded ✓
- [x] S0-7 Verification: `psql -U akp -d weather -c "SELECT extversion FROM pg_extension WHERE extname='timescaledb';"` — **returned `2.27.0-dev`** ✓

---

## Phase 1 — Application Skeleton

Stand up a minimal FastAPI app reachable only via Nginx. No device logic yet.

- [x] S1-1 Scaffold `html/app/` per directory layout; add `requirements.txt` pinning FastAPI, uvicorn, gunicorn, asyncpg, pydantic-settings, PyJWT, bcrypt, jinja2, python-multipart
- [x] S1-2 `app/main.py`: create `FastAPI()`, mount empty routers, add `/health` returning `{"status": "ok"}`
- [x] S1-3 `app/config.py`: Pydantic `BaseSettings` loading from `html/etc/iot.env` (DB DSN, JWT secret, `FIRMWARE_DIR` — **must be an absolute path**, resolved via `Path(value).resolve(strict=False)` at startup (Arch §10 Q-S11); the app refuses to start if `FIRMWARE_DIR` is not absolute or not writable; `FIRMWARE_KEEP_N` (default `3`, see Arch §3.4 and Arch §10 Q-S4); `SLOT_LEN_SEC` (default `43200`; must match device firmware upload cadence); `MAX_FIRMWARE_SIZE_BYTES` (default `491520` = 480 KB; matches the STM32L476RG app Flash partition — Arch §3.4 "Firmware size ceiling"))
- [x] S1-4 `systemd/iot-server.service`: `gunicorn app.main:app -k uvicorn.workers.UvicornWorker -w 2 -b 127.0.0.1:8000`; run as `akp` in development, `iotsrv` in production
- [x] S1-5 `scripts/deploy.sh`: `git pull` → `pip install -r requirements.txt` → `sudo systemctl restart iot-server` (system unit, not `--user`; matches S1-4)
- [x] S1-6 Commit and SSH-deploy to host; enable service (`systemctl enable --now iot-server`) — **2026-04-26: deployed via scp+pip install; service active (running); initial deploy via scp; git-pull workflow requires GitHub SSH key on server (deferred to Phase 5 CI/CD)**
- [x] S1-7 Verification: `curl http://127.0.0.1:8000/health` on host returns `200 {"status":"ok"}` ✓ **confirmed 2026-04-26**

---

## Phase 2 — Database Schema & Migrations

Create the schema defined in `Server_Architecture.md` §3.1 and §3.3 and wire up the asyncpg pool.

- [x] S2-1 `app/db/schema.sql`: `devices` (`UNIQUE (region_id, station_id)`; no `cn` column — fleet shares one cert, no per-device CN exists), `weather_records` (hypertable + compression policy), `ingest_log`, `ota_campaigns` (with `firmware_size`, `firmware_sha256`, `rollout_start`, `rollout_window_days INT DEFAULT 10`, `slot_len_sec INT NOT NULL DEFAULT 43200`, `target_cohort_ids TEXT[]`, `status` enum values `draft`/`scheduled`/`in_progress`/`paused`/`completed`/`cancelled` — terminal `cancelled` replaces the earlier `rolled_back` name per Arch §10 Q-S8 — see §3.3 and §3.4; **no** `target_cohort_size` column), `download_completions` (`campaign_id INT REFERENCES ota_campaigns(id)`, `device_id VARCHAR(6)`, `chunk_index INT NOT NULL`, `recorded_at TIMESTAMPTZ DEFAULT now()`, `PRIMARY KEY (campaign_id, device_id, chunk_index)` — written by the `/get_firmware` handler on every successfully served chunk; source of truth for `success_rate` and live rollout progress — Arch §10 Q-S13), `admin_users`
- [x] S2-2 `scripts/migrate.py`: applies numbered files from `app/db/migrations/` inside a transaction; records state in `schema_migrations` table
- [x] S2-3 `app/db/pool.py`: `asyncpg.create_pool(min=2, max=20)` in FastAPI lifespan startup; close on shutdown
- [x] S2-4 Seed admin user: one bcrypt-hashed row inserted via migration (credentials from env at provisioning time only)
- [x] S2-5 Verification: `python -m scripts.migrate --dry-run` prints the DDL; run for real; `\dt` in psql shows all 6 tables + `schema_migrations` ✓ **confirmed 2026-04-26** — Note: `DB_DSN` uses Unix socket (`postgresql:///weather?host=/var/run/postgresql`) for peer auth; TCP via `localhost` requires `scram-sha-256` password
- [x] S2-6 Verification: `SELECT * FROM timescaledb_information.hypertables WHERE hypertable_name='weather_records';` returns one row ✓ **confirmed 2026-04-26** (`num_dimensions=1`); pool init confirmed by "Application startup complete" in gunicorn logs

---

## Phase 3 — mTLS Ingestion Path (`/api/v1/weather/upload`)

Implements the binary upload flow from §3.1 and §6.

- [x] S3-1 `app/auth/mtls.py`: verify `X-SSL-Client-Verify == SUCCESS` (Nginx-set); return 403 on missing/failed; dependency-injectable. No CN extraction — no per-device CN exists (Arch §2.1); device identity comes from payload `(region_id, station_id)` only
- [x] S3-2 `app/ota/fixedpt.py`: convert S9.7 fixed-point (int16) → float using `value / 128.0`; mirror `lib/utils/fixedptc.h` sign-extension
- [x] S3-3 `app/ota/parser.py`: `parse_upload(payload: bytes) -> (region, station, list[dict])` using `struct.Struct("<HHB")` for header + 18-byte chunks; reject if length ≠ `5 + 18*count`
- [x] S3-4 `app/routers/weather.py`: `POST /api/v1/weather/upload` — parse header for (region, station) → `upsert_device_by_region_station()` → idempotency check using `{region:03d}{station:03d}:{first_sample_ts}` key → `INSERT ... ON CONFLICT DO NOTHING` for weather + ingest_log in a single transaction. The ingest, OTA metadata poll, and OTA chunk download all share the `/api/v1/weather/` prefix (Arch §10 Q-S1 Option B), so they live behind one Nginx location block and one rate-limit zone
- [x] S3-5 Timestamp conversion: Y2K epoch (seconds since 2000-01-01 UTC) → `TIMESTAMPTZ`; use `datetime(2000,1,1,tzinfo=UTC) + timedelta(seconds=ts)`
- [x] S3-6 Update `devices.last_seen` on every successful ingest (same transaction); auto-create the row via upsert when a new `(region_id, station_id)` first reports
- [x] S3-7 Verification (unit, `html/tests/`): parse a fixture byte-string produced from a known `Weather_Data_Packed_t` and assert all fields round-trip within ±1 LSB — **`html/tests/test_parser.py` covers single/max-batch/boundary/error cases; 9 parameterized tests** ✓
- [x] S3-8 Verification (integration, `server_test/`): T0 harness scaffold + T1-series tests written; run `pytest server_test/lib/` (parity, no network) and `pytest server_test/tests/test_ingest.py` (live server, requires INTERNAL_URL + TEST_DB_DSN in `.env`) ✓

---

## Phase 4 — Nginx & mTLS Termination

Bring traffic through Nginx; enforce client cert validation.

- [x] S4-1 `scripts/provision_ca.sh`: generate offline root (kept on air-gapped USB), online intermediate signed by root, both as PEM; initialises CA database (`index.txt`, `serial`, `crlnumber`, `ca.conf`) for `openssl ca` signing ✓
- [x] S4-2 `scripts/issue_device_cert.sh <CN>`: generate device key, CSR, sign with intermediate via `openssl ca -extensions v3_device`, emit `<CN>.crt`, `<CN>.key`, and `<CN>-chain.pem` (device cert + intermediate) ✓
- [x] S4-3 `nginx/iot_server.conf`: **two `server{}` blocks** (Arch §5, Q-S9). Each block uses `listen 443 ssl;` + `http2 on;` as separate directives (nginx ≥1.25.1; the `http2` parameter on `listen` is deprecated). Device vhost (`robin-gpu.cpe.ku.ac.th`): TLS 1.2+1.3, private-CA server cert, `ssl_client_certificate {html_dir}/pki/private_ca_chain.pem`, `ssl_crl {html_dir}/pki/ca.crl`, `ssl_verify_client on`, a **single** `location /api/v1/weather/` block (covering ingest + OTA per Q-S1 Option B) that enforces `if ($ssl_client_verify != SUCCESS) return 403` and forwards `X-SSL-Client-Verify`, plus `location /firmware/ { deny all; return 404; }`. Admin vhost (`adm.robinlab.cc`): Let's Encrypt server cert, `ssl_verify_client off`, `location /admin/ { proxy_pass ... }` only — **no** client-cert headers forwarded ✓
- [x] S4-4 Rate limit: place `limit_req_zone $arg_id zone=device_api:10m rate=10r/s;` at the **top of `nginx/iot_server.conf`** outside any `server{}` block — `conf.d/` files are included inside the `http{}` block of nginx.conf so this is a valid http-context directive; placing it inside a `server{}` block causes a "zero size shared memory zone" error at reload. Use `limit_req zone=device_api burst=20` inside `location /api/v1/weather/` — **per-device** throttle (Arch §10 Q-S3). Keyed on the `?id=` query parameter that Phase 3.1 firmware guarantees on every device request; do **not** key on `$ssl_client_s_dn` (fleet shares one cert per Arch §2.1) ✓
- [x] S4-5 Weekly CRL refresh: `scripts/refresh_crl.sh` reads revoked entries from `pki/intermediate/index.txt` via `openssl ca -gencrl`, atomically replaces `pki/ca.crl`, then `nginx -s reload`; `systemd/refresh-crl.service` (runs as root) + `systemd/refresh-crl.timer` (weekly, `Persistent=true`, `RandomizedDelaySec=3600`) ✓
- [x] S4-6 Verification: `openssl s_client -cert client.crt -key client.key -CAfile rootCA.crt -tls1_2 -servername robin-gpu.cpe.ku.ac.th -connect  HOST:443` completes handshake; device path request reaches FastAPI and `X-SSL-Client-Verify: SUCCESS` appears in access log ✓
- [x] S4-7 Verification: request to `https://robin-gpu.cpe.ku.ac.th/api/v1/weather/...` without `-cert` flag returns `403` (mandatory mTLS on the device vhost); request to `https://adm.robinlab.cc/admin/` without `-cert` flag reaches FastAPI and returns `200` or `401` — admin vhost is a separate listener with `ssl_verify_client off` (Arch §5, Q-S9) ✓
- [x] S4-8 Verification: confirm the admin vhost forwards no `X-SSL-Client-*` headers upstream and serves a Let's Encrypt chain (`openssl s_client -servername adm.robinlab.cc ...` shows the public CA) ✓

---

## Phase 4 Deploy — PKI Provisioning & Nginx Deployment on Server

Run these steps once, in order, to bring Phase 4 live on `robin-gpu.cpe.ku.ac.th`.
Prerequisite: S4-1 through S4-5 complete (scripts and config committed).

### Step 1 — PKI generation (secure workstation, not the server)

- [x] S4D-1 On a workstation that is **not** the production server, run `provision_ca.sh` to generate root + intermediate CAs and initial CRL — **2026-05-06: run via WSL Ubuntu-24.04; PKI at `/home/akp/iot_pki/` (not `/tmp/` — WSL `/tmp` is wiped on distro shutdown)**:
  ```bash
  wsl -d Ubuntu-24.04 -- bash /mnt/c/Users/akrap/weather-station/html/scripts/provision_ca.sh /home/akp/iot_pki
  ```
  All 7 outputs verified:
  - `/home/akp/iot_pki/offline/root.key` (4096-bit RSA, 3.2 KB, mode 400) ✓
  - `/home/akp/iot_pki/offline/root.crt` ✓
  - `/home/akp/iot_pki/intermediate/intermediate.key` (2048-bit RSA, 1.7 KB, mode 400) ✓
  - `/home/akp/iot_pki/intermediate/intermediate.crt` ✓
  - `/home/akp/iot_pki/intermediate/ca.conf` ✓
  - `/home/akp/iot_pki/private_ca_chain.pem` (intermediate + root, for Nginx) ✓
  - `/home/akp/iot_pki/ca.crl` (initial empty CRL) ✓

- [ ] S4D-2 Move `/home/akp/iot_pki/offline/root.key` to an **air-gapped USB drive** immediately. Verify the USB copy then delete the on-disk copy:
  ```bash
  wsl -d Ubuntu-24.04 -- shred -u /home/akp/iot_pki/offline/root.key
  ```

- [x] S4D-3 Issue the **server TLS certificate** for the device vhost (`robin-gpu.cpe.ku.ac.th`) signed by the intermediate CA — **2026-05-06: issued at `/home/akp/server_cert/`; valid to 2027-05-06** ✓ **NOTE: cert uses `v3_device` extension (EKU=clientAuth). openssl warns "unsuitable certificate purpose" but nginx presents it successfully. Verify with real A7670E modem; if modem enforces EKU, re-issue with `v3_server` (serverAuth) extension.**

- [x] S4D-4 Issue at least one **device test certificate** for end-to-end verification (S4-6) — **2026-05-06: `weather-test` cert issued at `/home/akp/device_test/`; valid to 2027-05-06** ✓

### Step 1b — Fleet certificate issuance (one-time; same cert baked into every device)

All weather stations share one client key and certificate (Arch §2.1). Device identity comes
exclusively from the payload `(region_id, station_id)`, not the cert CN. The CN `iot-fleet`
is only used for CA database tracking.

Each firmware binary embeds three cert arrays (uploaded to the A7670E modem FS via
`ssl_cert_inject()` on every cold boot — `lib/A7670/a7670.c`):

> **Format:** The A7670E modem requires **DER binary format** (no PEM headers). Each PEM file must
> be converted to DER before being embedded as a C byte array. Use `openssl x509 -outform DER`
> for certificates and `openssl rsa -outform DER` for private keys.

| File | Array | Content (DER binary) |
|------|-------|----------------------|
| `lib/A7670/server_der.c` | `server_der[]` | `intermediate.crt` → DER (1132 bytes) — modem trust anchor for server TLS cert signed by intermediate CA |
| `lib/A7670/client_der.c` | `client_der[]` | `iot-fleet.crt` → DER (870 bytes) — fleet mTLS client leaf cert |
| `lib/A7670/client_key_der.c` | `client_key_der[]` | `iot-fleet.key` → DER (1216 bytes) — fleet private key |

- [x] S4D-F1 Issue the **fleet client certificate** (run once; keep outputs off the server): **2026-05-06: issued at `/home/akp/fleet_cert/`; valid 365 days (expires 2027-05-06)** ✓
  ```bash
  wsl -d Ubuntu-24.04 -- bash /mnt/c/Users/akrap/weather-station/html/scripts/issue_device_cert.sh \
      iot-fleet /home/akp/fleet_cert /home/akp/iot_pki
  ```
  Outputs at `/home/akp/fleet_cert/`:
  - `iot-fleet.key` (mode 400) — **never copy to server; store alongside root.key on USB**
  - `iot-fleet.crt` — fleet certificate (valid 365 days; renew before expiry)
  - `iot-fleet-chain.pem` — fleet cert + intermediate (the mTLS client chain)

- [x] S4D-F2 Convert fleet PEM outputs + new CA chain to DER, then to C arrays: **2026-05-06: intermediate.der (1132 B), iot-fleet.der (870 B), iot-fleet-key.der (1216 B) generated; C arrays written to lib/A7670/server_der.c, client_der.c, client_key_der.c** ✓
  The A7670E modem requires **DER binary format**. Step 1: convert PEM → DER. Step 2: embed DER as C byte arrays.
  ```bash
  # Step 1: PEM → DER conversion (run in WSL)
  # CA chain (certs only — concatenate intermediate + root, then convert)
  wsl -d Ubuntu-24.04 -- openssl x509 -in /home/akp/iot_pki/intermediate/intermediate.crt \
      -outform DER -out /home/akp/iot_pki/intermediate.der
  # Note: if server cert chain has multiple certs, DER cannot concatenate them directly.
  # Use only the intermediate cert as the trust anchor if root is already in the modem.
  # Alternatively embed the full chain as two separate DER arrays — check AT+CSSLCFG docs.

  # Fleet client cert chain → DER (leaf cert only; intermediate loaded separately if needed)
  wsl -d Ubuntu-24.04 -- openssl x509 -in /home/akp/fleet_cert/iot-fleet.crt \
      -outform DER -out /home/akp/fleet_cert/iot-fleet.der

  # Fleet private key → DER
  wsl -d Ubuntu-24.04 -- openssl rsa -in /home/akp/fleet_cert/iot-fleet.key \
      -outform DER -out /home/akp/fleet_cert/iot-fleet-key.der

  # Step 2: DER binary → C byte arrays
  # CA chain (server trust anchor)
  wsl -d Ubuntu-24.04 -- bash /mnt/c/Users/akrap/weather-station/scripts/pem_to_c_array.sh \
      /home/akp/iot_pki/intermediate.der \
      server_der \
      /mnt/c/Users/akrap/weather-station/lib/A7670/server_der.c

  # Fleet client cert
  wsl -d Ubuntu-24.04 -- bash /mnt/c/Users/akrap/weather-station/scripts/pem_to_c_array.sh \
      /home/akp/fleet_cert/iot-fleet.der \
      client_der \
      /mnt/c/Users/akrap/weather-station/lib/A7670/client_der.c

  # Fleet private key
  wsl -d Ubuntu-24.04 -- bash /mnt/c/Users/akrap/weather-station/scripts/pem_to_c_array.sh \
      /home/akp/fleet_cert/iot-fleet-key.der \
      client_key_der \
      /mnt/c/Users/akrap/weather-station/lib/A7670/client_key_der.c
  ```

- [ ] S4D-F3 Rebuild firmware — the same binary is flashed to every unit:
  ```bash
  platformio run          # verify sizes stay within limits
  platformio run -t upload
  ```
  On cold boot `Modem_Module_Init()` calls `AT+CCERTDOWN` for each array (skips if already
  stored). Confirm storage with `AT+CCERTLIST` in the serial console — expect `"server.der"`,
  `"client.der"`, `"client_key.der"`.

**Certificate renewal (annual):** `iot-fleet.crt` expires after 365 days. Before expiry:
re-run S4D-F1 with a new output dir, regenerate C arrays (S4D-F2), rebuild, and reflash all
units. The old cert can be revoked after all units are updated.

**Revocation / key compromise:** revoking the fleet cert blocks **all** devices simultaneously.
Run `openssl ca -revoke /home/akp/fleet_cert/iot-fleet.crt -config /home/akp/iot_pki/intermediate/ca.conf`,
then `refresh_crl.sh` on the server. Issue a new fleet cert, regenerate C arrays, rebuild, and
reflash all units. The server starts rejecting the old cert as soon as Nginx reloads with the
updated CRL.

### Step 2 — Copy PKI artifacts to server

- [x] S4D-5 Copy the pki/ tree **excluding the offline root key** to the server — **2026-05-06: rsync'd `intermediate/`, `private_ca_chain.pem`, `ca.crl` (combined intermediate+root), `root.crl` to `html/pki/` on server** ✓ **NOTE: `ca.crl` must contain both intermediate and root CRLs — nginx `ssl_crl` requires CRLs for all chain levels. `refresh_crl.sh` updated to combine both.**

- [x] S4D-6 Copy server TLS cert and key to standard Nginx paths on the server — **2026-05-06: cert at `/etc/ssl/certs/robin-gpu.cpe.ku.ac.th.crt`; key at `/etc/ssl/private/robin-gpu.cpe.ku.ac.th.key` (mode 640, owned akp)** ✓ **Disabled conflicting `sites-enabled/robin-gpu` symlink (duplicate `upstream fastapi_backend`).**

### Step 3 — Let's Encrypt cert for admin vhost

- [x] S4D-7 On the server, obtain a Let's Encrypt certificate for `adm.robinlab.cc` (certbot was installed in S0-2). DNS A record for `adm.robinlab.cc` must point to the server IP before running:
  ```bash
  sudo certbot certonly --nginx -d adm.robinlab.cc --agree-tos --non-interactive
  ```
  Verify cert path matches `nginx/iot_server.conf`: `/etc/letsencrypt/live/adm.robinlab.cc/fullchain.pem` ✓

### Step 4 — Deploy Nginx config

- [x] S4D-8 Patch the `{html_dir}` placeholder in `nginx/iot_server.conf` and install to Nginx — **2026-05-06: installed to `/etc/nginx/conf.d/iot_server.conf`; `ssl_client_certificate` and `ssl_crl` paths confirmed absolute** ✓

- [x] S4D-9 Test config and reload Nginx — **2026-05-06: `nginx -t` OK; `systemctl reload nginx` OK** ✓

### Step 5 — Enable CRL refresh timer

- [x] S4D-10 Install and enable the CRL refresh systemd units — **2026-05-06: both units installed at `/etc/systemd/system/`, mode 644; timer enabled; next run Mon 2026-05-11** ✓

### Step 6 — Verifications (from Phase 4)

- [x] S4D-11 Run verification S4-6: mTLS handshake with test device cert completes, `X-SSL-Client-Verify: SUCCESS` visible in Nginx access log — **2026-05-06: handshake succeeds; request forwarded to FastAPI (404 from app, not 403 from nginx — confirms cert accepted and X-SSL-Client-Verify: SUCCESS forwarded)** ✓
- [x] S4D-12 Run verification S4-7: no client cert → **400** on device vhost (not 403 — `ssl_verify_client on` blocks at SSL layer before location block; both 400 and 403 are correct rejections); no client cert on admin vhost → **404** (Phase 6 admin routes not yet implemented, admin vhost reaches FastAPI correctly with `ssl_verify_client off`) ✓
- [x] S4D-13 Run verification S4-8: admin vhost issuer = `Let's Encrypt E7` ✓; no `X-SSL-Client-*` headers forwarded ✓

---

## Phase 5 — OTA Endpoints (`/`, `/get_firmware`)

Implements §3.2 and §3.3 device-facing OTA. Firmware ingestion (admin side) is Phase 7.

- [x] S5-1 `app/ota/crc32.py`: table-based CRC-32/MPEG-2 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection, no final XOR); verify against fixtures generated by `shared/crc32.c` — **2026-05-06: implemented; check value `b"123456789"` → `0x0376E6E7` matches C impl; unit tests in `html/tests/test_crc32.py`** ✓
- [x] S5-2 `app/ota/campaign.py`: `get_active_campaign_for_device(device_id: str) -> Campaign | None` — `device_id` is the 6-char `"{region:03d}{station:03d}"` passed via the `?id=` query param (Arch §3.2); picks highest `version` among `status='in_progress'` campaigns the device is eligible for. Cohort match uses `target_cohort_ids IS NULL OR cardinality(target_cohort_ids) = 0 OR device_id = ANY(target_cohort_ids)` (Arch §10 Q-S10) — **2026-05-06: implemented** ✓
- [x] S5-3 `app/ota/campaign.py` `compute_wait(device_id, campaign) -> int` — Arch §3.3 slot algorithm; `slot_len = campaign.slot_len_sec` (frozen at creation), `num_slots = rollout_window_days * 2`, `dev_slot = zlib.crc32(device_id.encode('ascii')) % num_slots`, `now_slot = min(num_slots-1, max(0, int((now - rollout_start).total_seconds() // slot_len)))`; returns `0` if `dev_slot <= now_slot` else `(dev_slot - now_slot) * slot_len`. Monotone-in-time ⇒ failed devices retry automatically next upload cycle — **2026-05-06: implemented; slot determinism verified in JavaScript against zlib.crc32** ✓
- [x] S5-4 `app/routers/ota.py` `GET /api/v1/weather/?id=<rrrsss>` (mTLS; Q-S1 Option B): reads `id` query param (6-char decimal, regex `^[0-9]{6}$`; reject with 400 on malformed); resolves campaign via S5-2; calls `compute_wait()`; returns `HTMLResponse` whose body contains `V.<version>:L.<size>:H.<sha256hex>:W.<seconds>`. Return `<html><body>No update available</body></html>` when no match (token absent → device treats as `W.0`, no update) — **2026-05-06: implemented; W field omitted when wait==0** ✓
- [x] S5-5 `GET /api/v1/weather/get_firmware?offset=X&length=Y&id=<rrrsss>` (mTLS; Q-S1 Option B): reads `id` query param; re-runs `compute_wait()`; if `W > 0` return `429 Too Many Requests` with `Retry-After: <seconds>` header (Arch §3.2); else opens file at the **absolute** `firmware_file_path` (Arch §10 Q-S11), `seek(offset)`, `read(length)`, appends 4-byte little-endian CRC-32/MPEG-2, returns `application/octet-stream`; after serving the chunk body, `INSERT INTO download_completions (campaign_id, device_id, chunk_index) VALUES (...) ON CONFLICT DO NOTHING` where `chunk_index = offset // 512` (Arch §10 Q-S13) — **2026-05-06: implemented** ✓
- [x] S5-6 Clamp `length` to [1, 512]; reject `offset + length > file_size` with 416; reject missing/malformed `id` with 400; reject when no active campaign for device with 404; enforce slot gate with 429 (S5-5) — **2026-05-06: all error paths implemented** ✓
- [x] S5-7 Stream reads use `aiofiles` or blocking read inside `run_in_executor` to avoid stalling the event loop — **2026-05-06: `aiofiles.open()` with async seek/read in `_read_chunk()`** ✓
- [x] S5-8 Verification: see `server_test/` T2-series (metadata regex match incl. optional `W` field, resumable chunked download, CRC + SHA-256 reconstruction, 429 on out-of-slot GET) — **2026-05-06: 18/18 T2 tests pass on robin-gpu.cpe.ku.ac.th (Python 3.13, pytest 9.0.3); fixed `struct.error` in `lib/fixedpt.py` (pack unsigned before unpack signed)** ✓

---

## Phase 6 — Admin Authentication & RBAC

JWT-based login for human operators. Must be in place before Phase 7 (firmware upload).

- [x] S6-1 `app/auth/jwt.py`: login endpoint verifies bcrypt hash, issues HS256 JWT with `sub`, `role`, `exp` (24 h) — **2026-05-06: implemented; `create_token`, `verify_token`, `check_password`, `get_current_user`, `require_role` factory** ✓
- [x] S6-2 Dependency `require_role("admin"|"operator"|"viewer")` — 401 on missing/invalid token, 403 on insufficient role — **2026-05-06: role hierarchy viewer(0) < operator(1) < admin(2); HTTPBearer auto_error=False → 401 on missing; level check → 403** ✓
- [x] S6-3 `/admin/login` (form POST), `/admin/logout`, `/admin/me` (returns current user), `/admin/users` (admin-only) — **2026-05-06: login returns `{access_token, token_type}`; logout stateless 200; me requires viewer; users requires admin** ✓
- [x] S6-4 Password reset: out-of-band only for v1 (admin updates DB row directly); in-app reset deferred to Phase 9 — no code needed ✓
- [x] S6-5 Verification: valid creds → 200 + JWT; wrong password → 401; role-restricted endpoint with `viewer` token → 403 — **2026-05-06: 7/7 unit tests (`html/tests/test_auth.py`) + 7/7 integration tests (`server_test/tests/test_admin_auth.py`) pass on robin-gpu.cpe.ku.ac.th** ✓

---

## Phase 7 — Admin OTA Campaign Management

- [x] S7-1 `POST /admin/firmware/upload` (admin only): accepts **only the binary file** (no version field); **rejects with `413 Request Entity Too Large` if `len(file_bytes) > settings.MAX_FIRMWARE_SIZE_BYTES`** (default 480 KB, the STM32L476RG app Flash partition — Arch §3.4 "Firmware size ceiling"); auto-assigns `new_version = MAX(ota_campaigns.version) + 1` (1 on first upload); auto-computes SHA-256 and byte size; writes `{FIRMWARE_DIR}/v{new_version}.bin` atomically (`tmp` + `os.replace()` — rename only after DB commit to avoid orphaned files on transaction failure); inserts draft campaign row with `slot_len_sec = settings.SLOT_LEN_SEC`; then runs `_sweep_firmware_retention(settings.FIRMWARE_KEEP_N)` — deletes `.bin` files for **terminal** (`completed`/`cancelled`) campaigns older than the `FIRMWARE_KEEP_N` most recent terminal campaigns; **never** touches draft/in_progress/paused campaigns (Arch §3.4, §10 Q-S4). Returns `id`, `version`, `firmware_sha256`, `firmware_size`. Phase 7 DB helpers added to `app/db/queries.py`: `get_max_firmware_version`, `insert_campaign`, `get_campaign`, `list_campaigns_by_status`, `list_terminal_campaigns_ordered`, `set_campaign_in_progress/paused/resumed/cancelled`, `compute_campaign_success_rate`, `count_completed_devices`, `count_eligible_devices` — **2026-05-06: implemented** ✓
- [x] S7-2 `POST /admin/campaign/{id}/start`: admin-only; `StartCampaignRequest` body with `rollout_window_days` (default 10, ge=1 le=30, validated by Pydantic → 422 on violation), optional `slot_len_sec` (override; default from `settings.SLOT_LEN_SEC`), and optional `target_cohort_ids TEXT[]` (**empty list normalised to NULL before insert** — Arch §10 Q-S10); sets `status='in_progress'`, `rollout_start=now()`; transition validated (`draft → in_progress` only; any other status → 409) — **2026-05-06: implemented** ✓
- [x] S7-3 `POST /admin/campaign/{id}/pause`, `POST /admin/campaign/{id}/resume`, `POST /admin/campaign/{id}/cancel` — operator role or higher. Pause/resume do **not** reset `rollout_start`; resuming continues from original slot clock. `cancel` is terminal: computes `success_rate` (S7-3.6) then runs `_sweep_firmware_retention()`. Invalid transitions return 409. `cancel` valid from `draft`, `in_progress`, or `paused` — **2026-05-06: implemented** ✓
- [x] S7-3.6 On terminal status transition (`cancelled`; `completed` handled same way when added): `compute_campaign_success_rate()` counts distinct `device_id` values in `download_completions` where `campaign_id = {id}` and chunk count ≥ `(firmware_size + 511) // 512` (ceiling division per Q-S6), divided by total distinct devices that started; writes result to `ota_campaigns.success_rate` (0.0 when no devices downloaded) — **2026-05-06: implemented** ✓
- [x] S7-4 `GET /admin/campaign/{id}` (viewer+): returns full campaign row plus `completed_device_count` (devices with all chunks), `eligible_device_count` (devices matching cohort filter via `devices` table; NULL cohort = full fleet), `current_slot` (derived from `rollout_start` + `now()`), `num_slots = rollout_window_days * 2` — **2026-05-06: implemented** ✓
- [x] S7-5 Invariants enforced: duplicate `version` rejected at DB level (UNIQUE constraint); on start, firmware file SHA-256 and size re-verified from disk (409 on mismatch or missing file); `rollout_window_days`/`slot_len_sec` frozen at start (status guard prevents re-starting an in_progress campaign → 409); ceiling division `(firmware_size + 511) // 512` used consistently at S5-6 range guard, S7-3.6 success-rate, and S7-4 progress readout — **2026-05-06: implemented** ✓
- [ ] S7-6 Verification: `pytest server_test/tests/test_admin_campaign.py` — 22 T3-series tests (auth/RBAC, upload response+DB+disk, version increment, oversize 413, start lifecycle, window-days validation, pause/resume rollout_start immutability, cancel success_rate, cohort filter, re-start 409, SHA-256 tamper 409, campaign detail aggregates) — **pending server deploy + test run**

---

## Phase 8 — Admin UI (HTMX + Jinja2)

Minimum viable operator surface.

- [x] S8-1 Login page (`/admin/login.html`); CSRF token on form POST — **2026-05-07: `app/routers/ui.py` GET+POST; `app/auth/csrf.py` HMAC-signed double-submit; cookie-based JWT auth (`access_token`, HttpOnly, SameSite=strict); `templates/login.html`** ✓
- [x] S8-2 Dashboard: device list with `(region_id, station_id)` and `last_seen`; server-rendered table with HTMX `hx-get` pagination — **2026-05-07: `GET /admin/dashboard` → `templates/dashboard.html`; `GET /admin/devices/table?page=N` → `partials/device_table.html`; 20 rows/page; `list_devices`/`count_devices` added to `db/queries.py`** ✓
- [x] S8-3 Campaign form: file-only upload (no version field — version shown as "will be assigned automatically: v{max+1}"); progress via HTMX `hx-post` + polling; display assigned version and SHA-256 after upload completes; campaign list with start/pause/cancel buttons — **2026-05-07: `GET /admin/campaigns` + `GET /admin/campaigns/list` + `POST /admin/firmware/upload-ui` + start/pause/resume/cancel-ui endpoints; campaign detail page with 15s polling progress; `list_all_campaigns` added to `db/queries.py`; `app/templating.py` Jinja2 singleton; `ui.router` mounted in `main.py`** ✓
- [ ] S8-4 Verification: end-to-end manual test from a browser — log in, upload firmware, start rollout, observe download-completion progress — **deployed 2026-05-07 via scp (server has no git repo; deploy.sh updated to scp+extract); service active on robin-gpu; browser test pending**

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
