# IoT Data Server — Architecture

Fleet: 300–1000 STM32L476RG + A7670E LTE devices · 2× daily sensor uploads + pull-model OTA.
Production host: `akp@robin-gpu.cpe.ku.ac.th` (Ubuntu, PG 17 + TimescaleDB 2.27.0-dev from source).
Phase status: `IMPLEMENTATION_STATUS.md`. Implementation plan: `Server_Implementation_Plan.md`. Test plan: `Server_Test_Plan.md`.

---

## 1. Stack

| Component | Choice | Why |
|-----------|--------|-----|
| Language        | Python 3.13           | Binary protocol parsing, async I/O |
| Web framework   | FastAPI               | Async, OpenAPI |
| Reverse proxy   | Nginx                 | mTLS termination at scale |
| DB              | PostgreSQL 17 + TimescaleDB | Hypertable + compression for `weather_records` |
| Admin UI        | FastAPI + HTMX + Jinja2 | Server-rendered, no SPA build |
| Metrics / logs  | Prometheus + Loki + Promtail + Grafana | `/metrics` bound to 127.0.0.1 |

---

## 2. Authentication

Two distinct models. Devices use mTLS; admins use username/password → JWT. Never mix.

### 2.1 Devices (mTLS, private CA)

| Aspect | Setting |
|--------|---------|
| Trust anchor | Private intermediate CA (`{html_dir}/pki/private_ca_chain.pem`) |
| Cert scope | One shared **fleet** client cert (CN `iot-fleet`). No per-device CN. |
| Device identity | Payload `(region_id, station_id)` for ingest; `?id=rrrsss` query param for OTA |
| Modem cert format | **DER binary** (no PEM headers). Convert via `openssl x509/rsa -outform DER`. |
| TLS | 1.3 preferred; 1.2 fallback |
| CRL | `{html_dir}/pki/ca.crl`, refreshed weekly by `scripts/refresh_crl.sh`. **Must contain intermediate + root CRLs** (nginx `ssl_crl` validates every chain level). |
| Cert rotation | Fleet client cert: 365 days. Server TLS cert: 365 days. Intermediate CA: annual audit. Root CA: offline, 5-year rotation. |

Embedded cert arrays (`lib/A7670/*_der.c`) loaded onto the modem via `AT+CCERTDOWN` on each cold boot:

| File | Array | Contents |
|------|-------|----------|
| `server_der.c`     | `server_der[]`     | Intermediate CA DER (trust anchor for server TLS) |
| `client_der.c`     | `client_der[]`     | Fleet client cert DER |
| `client_key_der.c` | `client_key_der[]` | Fleet private key DER |

**Revocation = fleet-wide.** Revoking the fleet cert blocks every device. To rotate: re-issue cert, regenerate DER + C arrays, rebuild, reflash all units.

### 2.2 Admins (JWT)

| Aspect | Setting |
|--------|---------|
| Trust anchor | Let's Encrypt (server cert only — `ssl_verify_client off`) |
| Login | `POST /admin/login` → bcrypt verify → HS256 JWT (24 h) in `access_token` cookie (HttpOnly, SameSite=strict) |
| Auth header | UI: cookie. API: `Authorization: Bearer`. |
| RBAC | viewer (0) < operator (1) < admin (2). Enforced via `require_role` dependency. |
| CSRF | HMAC double-submit on every form mutation (`app/auth/csrf.py`). |
| Password reset | Out-of-band (admin updates DB row directly). |

---

## 3. Data Protocols

### 3.1 Ingest — binary packed struct

```
Header (5 B):  uint16 region_id, uint16 station_id, uint8 chunk_count
Body  (18 B × chunk_count):  Weather_Data_Packed_t
  - uint32 ts                (Y2K epoch — seconds since 2000-01-01 UTC)
  - int16  temperature       (S9.7 fixed-point — see lib/utils/fixedptc.h)
  - int16  humidity          (S9.7)
  - int16  pressure          (S9.7)
  - uint16 light_par         (µmol/s·m², 0–2500, no scaling)
  - int16  rainfall          (S9.7)
  - int16  dew_point         (S9.7)
  - int16  bus_value         (S9.7)
```

S9.7: sign bit + 9-bit whole + 7-bit fraction → float = `int_value / 128.0`.

**Idempotency key:** `"{region:03d}{station:03d}:{first_sample_ts.isoformat()}"` stored in `ingest_log`. Same payload twice → `{"status": "duplicate"}` returned, no second insert.

**Schema (full):**

```sql
CREATE TABLE devices (
  id          SERIAL       PRIMARY KEY,
  region_id   SMALLINT     NOT NULL,
  station_id  SMALLINT     NOT NULL,
  last_seen   TIMESTAMPTZ,
  created_at  TIMESTAMPTZ  DEFAULT now(),
  UNIQUE (region_id, station_id)
);

CREATE TABLE weather_records (
  time        TIMESTAMPTZ  NOT NULL,
  device_id   INT          NOT NULL REFERENCES devices(id),
  temperature REAL, humidity REAL, pressure REAL,
  light_par   SMALLINT,
  rainfall    REAL, dew_point REAL, bus_value REAL
);
SELECT create_hypertable('weather_records', 'time');
ALTER TABLE weather_records SET (
  timescaledb.compress,
  timescaledb.compress_segmentby = 'device_id'
);
SELECT add_compression_policy('weather_records', INTERVAL '7 days');

CREATE TABLE ingest_log (
  idempotency_key VARCHAR(320) PRIMARY KEY,
  recorded_at     TIMESTAMPTZ DEFAULT now()
);
```

### 3.2 OTA — three-phase pull model

Device builds URLs from `UPDATE_PATH + "/"`. All device traffic shares `/api/v1/weather/` (Q-S1 Option B): one Nginx location, one rate-limit zone.

| Phase | URL | Behavior |
|-------|-----|----------|
| 1. Version + size | `GET <update_path>/?id=rrrsss` | HTML body containing `V.#####:L.$$$$$$$:H.<sha256hex>:W.<seconds>`. `W.0` = download now; `W>0` = skip cycle. Missing `:W.` = `W.0` (back-compat). |
| 2. Chunk download | `GET <update_path>/get_firmware?offset=X&length=Y&id=rrrsss` | Returns `Y` bytes + 4-byte little-endian CRC-32/MPEG-2 trailer. Length clamped to `[1, 512]`; `offset + length > size` → 416. Out-of-slot device → `429 Retry-After: <sec>`. |
| 3. Install | (Device-side) | OCB written; reboot; bootloader programs Flash; `ota_confirm_success()` within 60 s or rollback. |

### 3.3 Rollout slot algorithm

Problem: 1000 devices × 512 KB unthrottled ≈ 4.5 Gbit burst, exceeds VPS cap. A7670E firmware does not follow HTTP 302 → no object-storage offload. Solution: deterministic 20-slot schedule over 10 days, flatten to ~90 KB/s peak.

```python
def compute_wait(device_id: str, campaign) -> int:
    """Returns seconds the device must wait. 0 = download now."""
    slot_len  = campaign.slot_len_sec           # frozen at creation; default 43200 = 12 h
    num_slots = campaign.rollout_window_days * 2  # default 20
    elapsed   = (now_utc - campaign.rollout_start).total_seconds()
    now_slot  = min(num_slots - 1, max(0, int(elapsed // slot_len)))
    dev_slot  = zlib.crc32(device_id.encode("ascii")) % num_slots
    return 0 if dev_slot <= now_slot else (dev_slot - now_slot) * slot_len
```

**Properties:**

- Monotone in time — once eligible, always eligible (failed devices retry next cycle).
- Even distribution — ~50 ± 10 devices/slot at N=1000.
- Stateless per request — no device-progress table needed for slot computation.
- Drift-tolerant — server time end-to-end; device RTC drift irrelevant.
- Slot gate re-checked on every `/get_firmware` request → 429 (defends against clients that ignore `W.`).

**`zlib.crc32` is CRC-32 IEEE (reflected, poly `0xEDB88320`)** — **different** from the CRC-32/MPEG-2 used for chunk trailers. The device does not hash its own id; it reads `W` from the token.

**Schema:**

```sql
CREATE TABLE ota_campaigns (
  id SERIAL PRIMARY KEY,
  version INT NOT NULL UNIQUE,
  description TEXT,
  firmware_sha256 VARCHAR(64) NOT NULL,
  firmware_size   INT          NOT NULL,
  firmware_file_path TEXT NOT NULL,           -- {FIRMWARE_DIR}/v{version}.bin
  rollout_start TIMESTAMPTZ,
  rollout_window_days INT DEFAULT 10,
  slot_len_sec INT NOT NULL DEFAULT 43200,    -- frozen at campaign creation
  target_cohort_ids TEXT[],                   -- NULL or empty = whole fleet (Q-S10)
  status VARCHAR(32) DEFAULT 'draft',
                                              -- draft | scheduled | in_progress | paused
                                              -- | completed | cancelled (Q-S8)
  success_rate NUMERIC,                       -- written on terminal transition (Q-S5)
  created_at TIMESTAMPTZ DEFAULT now(),
  updated_at TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE download_completions (
  campaign_id INT          NOT NULL REFERENCES ota_campaigns(id),
  device_id   VARCHAR(6)   NOT NULL,          -- "{region:03d}{station:03d}"
  chunk_index INT          NOT NULL,          -- offset // 512
  recorded_at TIMESTAMPTZ  DEFAULT now(),
  PRIMARY KEY (campaign_id, device_id, chunk_index)
);

CREATE TABLE admin_users (
  id SERIAL PRIMARY KEY,
  username VARCHAR(64) NOT NULL UNIQUE,
  password_hash VARCHAR(128) NOT NULL,        -- bcrypt cost ≥ 12
  role VARCHAR(16) NOT NULL,                  -- viewer | operator | admin
  created_at TIMESTAMPTZ DEFAULT now()
);
```

**Active-campaign selection (per device):** highest `version` among rows where `status='in_progress'` and (`target_cohort_ids IS NULL OR cardinality(target_cohort_ids)=0 OR device_id = ANY(target_cohort_ids)`). Empty arrays normalised to `NULL` on insert.

**No per-device firmware version tracking on the server.** Device compares `V` vs `FW_VERSION` itself.

**Campaign workflow:** draft → start (sets `rollout_start`, freezes `slot_len_sec`) → in_progress → pause/resume (rollout_start preserved) or cancel/complete (terminal: `success_rate` computed). Field rollback = upload old binary as a new higher-version campaign.

**Live progress** during the rollout window: query `download_completions` for `(campaign_id, device_id)` with `count(chunk_index) >= (firmware_size + 511) // 512` (ceiling). Surfaced via `GET /admin/campaign/{id}` aggregates. **Terminal `success_rate`** is written once at transition.

**Egress envelope (1000 dev, 512 KB, 10 days, device-side 30 min jitter):** ~50 ± 10 dev/slot, ~26 MB/slot, peak ~90 KB/s, average ~14 KB/s, total ~512 MB.

### 3.4 Firmware storage

```
{FIRMWARE_DIR}/v{version}.bin     ← raw binary; filename is canonical key
```

- `FIRMWARE_DIR` must be **absolute** (resolved at startup; app refuses to start otherwise — Q-S11). Mode `0750`; owned by `iotsrv` in prod, `akp` in dev. Excluded from git.
- Atomic write: write `.tmp`, then `os.replace()` only after DB commit succeeds.
- Nginx denies `/firmware/` → 404. Files reach devices only via `/get_firmware`.

**Version assignment:** `new_version = MAX(ota_campaigns.version) + 1` (starts at 1). Admin uploads **only the binary**; size + SHA-256 computed by the server.

**Size ceiling:** `MAX_FIRMWARE_SIZE_BYTES = 480 * 1024` (matches STM32L476RG app Flash partition — `OTA_Firmware_Architecture.md §2`). Upload >480 KB → `413 Request Entity Too Large` **before** writing to disk.

**Integrity on start:** `POST /admin/campaign/{id}/start` recomputes SHA-256 from disk; mismatch with stored value → `409`.

**Retention (Q-S4):** `FIRMWARE_KEEP_N = 3` (configurable). Sweep runs after each upload and after any transition into a terminal state.

- Never delete `.bin` for draft / in_progress / paused campaigns.
- Among terminal (completed / cancelled) campaigns, keep the `N` most recent by `version DESC`; delete the older ones.

**Optional Ed25519 signing (S10-4):** when `SIGNING_PRIVATE_KEY_PATH` is set, the server writes `v{n}.sig` alongside the binary and returns `firmware_ed25519_sig` in the upload response. Bootloader verification is deferred.

---

## 4. Performance Envelope

| Metric | Estimate | Note |
|--------|----------|------|
| Ingest req/day        | 2000          | 1000 dev × 2 uploads — trivial |
| Ingest payload         | ~200 B / req  | Binary packed struct |
| TimescaleDB write rate | ~50 rows/s    | Fits single vCPU |
| OTA peak egress        | ~90 KB/s      | With 20-slot rollout + device jitter |
| OTA total egress       | ~512 MB / 10 d | 1000 dev × 512 KB |
| asyncpg pool           | min 2 / max 20 | Handles 100 concurrent uploads |

---

## 5. Nginx (two listeners, two trust stores — Q-S9)

Devices and browsers terminate on separate `server{}` blocks, selected by SNI. Device vhost = private CA + mTLS required. Admin vhost = Let's Encrypt + no client cert. Eliminates any risk of provisioning the wrong trust anchor to the modem.

`limit_req_zone` keys on `$arg_id` (Q-S3) — `$ssl_client_s_dn` would collapse to a single fleet-wide key.

```nginx
# /etc/nginx/conf.d/iot_server.conf — included inside http{} so http-level directives are valid here
limit_req_zone $arg_id zone=device_api:10m rate=10r/s;

upstream fastapi_backend { server 127.0.0.1:8000; keepalive 32; }

# Device listener
server {
    listen 443 ssl; http2 on;
    server_name robin-gpu.cpe.ku.ac.th;

    ssl_certificate     /etc/ssl/certs/robin-gpu.cpe.ku.ac.th.crt;
    ssl_certificate_key /etc/ssl/private/robin-gpu.cpe.ku.ac.th.key;
    ssl_client_certificate {html_dir}/pki/private_ca_chain.pem;
    ssl_crl               {html_dir}/pki/ca.crl;
    ssl_verify_client     on;
    ssl_verify_depth      2;
    ssl_protocols         TLSv1.3 TLSv1.2;

    location /api/v1/weather/ {
        if ($ssl_client_verify != SUCCESS) { return 403; }
        proxy_set_header X-SSL-Client-Verify $ssl_client_verify;
        proxy_pass http://fastapi_backend;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        limit_req zone=device_api burst=20;
    }

    location /firmware/ { deny all; return 404; }
    location /          { return 404; }
}

# Admin listener
server {
    listen 443 ssl; http2 on;
    server_name adm.robinlab.cc;

    ssl_certificate     /etc/letsencrypt/live/adm.robinlab.cc/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/adm.robinlab.cc/privkey.pem;
    ssl_verify_client   off;
    ssl_protocols       TLSv1.3 TLSv1.2;

    location /admin/ {
        proxy_pass http://fastapi_backend;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
    }
    location / { return 404; }
}
```

The admin vhost forwards **no** `X-SSL-Client-*` headers (verified by T4-8).

---

## 6. FastAPI Endpoint Map

### Device (mTLS via Nginx; FastAPI verifies `X-SSL-Client-Verify == SUCCESS`)

| Endpoint | Purpose |
|----------|---------|
| `POST /api/v1/weather/upload`                          | Ingest binary packed struct |
| `GET  /api/v1/weather/?id=<rrrsss>`                    | OTA version + wait token |
| `GET  /api/v1/weather/get_firmware?offset=&length=&id=`| OTA chunk + CRC trailer; inserts `download_completions` row |

### Admin (JWT; cookie-based for UI, Bearer for API)

| Endpoint | Min role | Purpose |
|----------|----------|---------|
| `POST /admin/login`, `POST /admin/logout`, `GET /admin/me` | — / viewer | Auth |
| `GET /admin/users`                                      | admin   | List users (extended JSON) |
| `POST /admin/users`, `PUT /admin/users/{id}`, `PUT /admin/users/{id}/password`, `DELETE /admin/users/{id}` | (see User_Management_Implementation_Plan.md §UM2) | CRUD (with self-protection guards) |
| `POST /admin/firmware/upload`                           | admin   | Binary upload — auto version + SHA-256 + size; 413 if > 480 KB; retention sweep |
| `POST /admin/campaign/{id}/start                       | admin   | Set `in_progress`; freeze `slot_len_sec`; recompute SHA-256 from disk (409 mismatch) |
| `POST /admin/campaign/{id}/pause | resume | cancel`    | operator+ | Lifecycle. `cancel` is terminal; computes `success_rate` and runs retention sweep |
| `GET /admin/campaign/{id}`                              | viewer+ | Full row + `completed_device_count`, `eligible_device_count`, `current_slot`, `num_slots` |
| UI routes under `/admin/*`                              | viewer+ | Login, dashboard, campaigns, sensor-data, users, profile |
| `GET /metrics`                                          | — (loopback only) | Prometheus scrape; IP guard rejects non-127.0.0.1 |

---

## 7. Device-Side Requirements

| Requirement | Firmware component |
|-------------|-------------------|
| TLS 1.2+ mTLS with fleet client cert | A7670E `AT+CSSLCFG` + `AT+CCERTDOWN` (DER format) |
| NTP sync before TLS                  | `at_channel_send_cntp(12000)` blocks until `+CNTP: 0` |
| Binary ingest                        | `maintask.c` packs → `ssluploadtask.c` POSTs |
| OTA metadata polling                 | `OtaManagerTask` GET `<update_path>/?id=rrrsss` |
| OTA rollout gate                     | On `W>0` return to IDLE (no download) |
| OTA chunk download                   | `a7670_ssl_downloader.c`; per-chunk CRC; 429 → back off & retry |
| Device identity bounds               | `region_id`, `station_id` ∈ `0–999`; URL builder applies `% 1000` (Q-S7) |
| Size guard (defence-in-depth)        | Reject `L > FLASH_APP_SIZE_MAX (480 KB)` in `POLLING_VERSION` |
| Whole-image SHA-256 verify           | `OtaManagerTask` accumulates during download; bootloader re-verifies pre-program |
| 60 s confirm + IWDG rollback         | `ota_confirm_success()`; bootloader retains old app on timeout / SP sanity fail |

See `OTA_Firmware_Architecture.md` for full device-side detail.

---

## 8. Deploy & Ops

**Deploy:** `bash html/scripts/deploy.sh` — scp + extract + `systemctl restart`. Server has **no** git repo. SSH key `~/.ssh/akrapong.key`.

**Blue/green (S10-2):** two units (`iot-server-blue` on :8000, `iot-server-green` on :8001). `scripts/blue_green_deploy.sh` health-checks the inactive slot, swaps `nginx/iot_upstream.conf`, reloads nginx, tracks active slot in `etc/active_slot`.

**Backups (S10-3):** daily `pg_dump → gzip → html/backups/` (14-day retention) via `backup-db.timer`; weekly `restore-test.timer` validates by restoring to a temp DB.

**Alerts (S9-5):**

- Ingest lag > 5 min → page ops
- Cert-verify error rate > 1/min → check CRL freshness
- OTA campaign `success_rate < 0.8` (post-rollout) on cohort ≥ 10 %

---

## 9. Decision Summary

All cross-document open questions are resolved. Body of this doc reflects accepted decisions.

| ID | Decision |
|----|----------|
| Q-S1  | All device traffic mounted under `/api/v1/weather/`. One Nginx location, one rate-limit zone. |
| Q-S2  | Firmware sends `?id=` on every OTA request and parses `:W.<seconds>` (missing → `W.0`). |
| Q-S3  | `limit_req_zone` keys on `$arg_id` for per-device throttling (fleet shares one cert). |
| Q-S4  | `FIRMWARE_KEEP_N = 3` retention sweep; non-terminal campaigns never deleted. |
| Q-S5  | `success_rate` written once on terminal status transition; live progress derived from `download_completions`. |
| Q-S6  | Ceiling division `(firmware_size + 511) // 512` everywhere chunk totals matter. |
| Q-S7  | `region_id`, `station_id` ∈ `0–999`; firmware crops `% 1000` at URL-build time. |
| Q-S8  | Terminal abort = `cancelled` (was `rolled_back`). Field rollback = re-upload as new higher version. |
| Q-S9  | Two `server{}` blocks: device vhost (private CA + mTLS) and admin vhost (Let's Encrypt). |
| Q-S10 | `target_cohort_ids` NULL or empty array both mean "whole fleet"; admin write path normalises empty → NULL. |
| Q-S11 | `FIRMWARE_DIR` must be absolute (`Path.resolve()` at startup; refuse to start otherwise). |
| Q-S12 | PG 17 + TimescaleDB from source on production host. No `timescaledb` apt package. |
| Q-S13 | `download_completions` is the sole source for live progress AND terminal `success_rate`. No log-parsing. |

---

## 10. References

- Device firmware: `OTA_Firmware_Architecture.md`
- Binary schema: `lib/utils/weather_data.h`, `lib/utils/fixedptc.h`
- CRC-32/MPEG-2: `shared/crc32.c`
- OTA protocol: `shared/ota_control_block.h`, `lib/A7670/a7670_ssl_downloader.h`
- NTP: `ntp_manual.md`
- HTTPS AT commands: `https_manual.md` (Chapter 16)
