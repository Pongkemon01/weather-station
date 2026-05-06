# IoT Data Server: Final Architecture Specification

**Fleet size:** 300–1000 IoT devices (STM32L476RG + A7670E LTE modem)
**Primary use case:** Environmental sensor data ingestion (2× daily uploads) + Over-The-Air (OTA) firmware distribution
**Deployment:** Low-cost VPS (1–2 vCPU, ~2 GB RAM)

---

## 1. Software Stack

| Component | Choice | Rationale |
| ----------- | -------- | ----------- |
| **Language** | Python 3.12+ | Binary protocol parsing, database interaction, OTA logic |
| **Web framework** | FastAPI | Async I/O for concurrent uploads; native OpenAPI docs |
| **Reverse proxy** | Nginx | mTLS termination with private CA; outperforms FastAPI on TLS handshakes at scale |
| **Database** | PostgreSQL + TimescaleDB | Relational metadata + optimized time-series compression for sensor logs |
| **Admin UI** | FastAPI + HTMX/Jinja2 | Auth via JWT; server-rendered fragments; no separate SPA build step |
| **Monitoring** | Grafana | Real-time device dashboards |
| **Observability** | Prometheus + Loki | Metrics (ingest lag, cert verify failures, OTA success rate); logs aggregation |

---

## 2. Security Configuration

Two distinct authentication models are used, one per actor type:

| Actor | Path | Auth mechanism | Who triggers it |
|-------|------|----------------|-----------------|
| **IoT device** | `/api/v1/weather/*`, `/`, `/get_firmware` | Mutual TLS (client certificate) | Automatic — modem performs TLS handshake with pre-provisioned cert |
| **Human admin** | `/admin/*` | Username + password → JWT | Human — operator types credentials into a browser |

These two models must not be mixed: device paths enforce a valid client certificate and reject requests without one; admin paths must **not** require a client certificate because browsers carry none.

### 2.1 mTLS for Device Path

IoT devices connect to the server **automatically** with no human in the loop. Authentication is performed at the TLS layer using a pre-provisioned X.509 client certificate stored in the A7670E modem at device commissioning time. No password is involved.

- **Mechanism:** Nginx loads the private CA chain and validates each incoming client certificate. Connections without a valid cert are rejected at the TLS layer before any HTTP processing.
- **A7670E role:** Modem presents the device certificate during TLS handshake using **DER binary format** (no PEM headers). Certs and keys must be converted from PEM to DER before being embedded as C byte arrays and uploaded via `AT+CCERTDOWN`.
- **Nginx verification:** Validates client cert against Private CA and CRL; forwards to FastAPI only if `$ssl_client_verify == SUCCESS`.
- **Certificate scope:** The entire fleet shares a single client certificate. The cert authenticates *"a device in this fleet"* only — no per-device CN information exists. Device identity comes exclusively from the application payload (`region_id`/`station_id` in §3.1) or the `?id=rrrsss` query parameter (§3.2). `X-Client-DN` is never used as identity.
- **FastAPI:** Accepts any request with `$ssl_client_verify == SUCCESS`; treats the payload `region_id`/`station_id` as the authoritative device identity for ingest, OTA cohort assignment, and idempotency.
- **Rate limiting:** Nginx limits ingest requests per client cert as a coarse fleet-wide throttle; tune the rate above 1 req/sec to accommodate 1000-device bursts, or switch the limit key to `$arg_id`.

**TLS version:** Prefer **TLS 1.3** on A7670 firmware ≥ 1.15; fall back to 1.2 for older modems. TLS 1.2 is acceptable but 1.3 reduces handshake overhead.

### 2.2 Username/Password for Admin Path

Human operators access the admin UI through a standard web browser. No client certificate is required or expected — the browser carries none. Authentication is credential-based.

- **Login flow:** Operator submits username + password via `POST /admin/login`; FastAPI verifies the bcrypt hash; issues a signed JWT (HS256, 24 h expiry).
- **Per-request auth:** Every subsequent admin request must include the JWT in the `Authorization: Bearer` header; FastAPI validates signature and expiry.
- **User/password storage:** bcrypt-hashed rows in the `admin_users` table; password reset is out-of-band (admin updates DB row) for v1.
- **RBAC:** Role-based access control (admin, operator, viewer) controls firmware upload, device management, log access.
- **No client certificate:** Nginx is configured with `ssl_verify_client optional` at the server level; client cert enforcement is applied **only** in the device location block (see §5). Admin browsers connect over plain server-authenticated TLS.

### 2.3 Certificate Lifecycle (Critical at 1000 scale)

| Stage | Responsibility | Timeline |
|-------|-----------------|----------|
| **Device cert provisioning** | Off-line key generation; store private key in A7670 secure enclave or protected FRAM sector | At device commissioning |
| **Server root CA** | Keep offline; sign only the intermediate CA; store in HSM or air-gapped vault | Indefinite; rotate every 5 years |
| **Server intermediate CA** | Online signer for device certs; rotated annually | Annual audit & rotation |
| **CRL / OCSP** | FastAPI endpoint `/ca/crl.pem` (updated weekly); Nginx `ssl_crl` validates against it on each connection | Weekly CRL refresh + log revocations |

---

## 3. Data Protocols

### 3.1 Binary Ingestion (Packed Struct)

Minimizes bandwidth for frequent sensor uploads.

**Format:**
```
Header (5 bytes):
  - uint16_t region_id    (region identifier)
  - uint16_t station_id   (station identifier)
  - uint8_t  chunk_count  (number of 18-byte chunks following)

Data (18 bytes × chunk_count):
  - Packed C struct matching firmware's weather_data_packed_t layout
  - See shared/weather_data.h for definitive schema
```

**Parsing:** Python `struct.unpack("<HHB...")` + loop over chunks.

**Conversion:** Unpacked data must be converted to standard formats before added to the database as:
    - time_stamp represents epoch since 1-Jan-2000. It should be converted to TIMESTAMP in Postgresql format.
    - light_par is already in the correct format.
    - temperature, humidity, pressure, rainfall, dew_point, and bus_value are in fixed-point format defined in lib/utils/fixedptc.h. They have the format as SWWWWWWWWFFFFFFF where S is sign bit, F represent fraction part, and W represent whole part. These values should be converted to FLOAT in Postgresql format.

**Idempotency:** Server deduplicates on `(region_id, station_id, first_sample_timestamp)`. Prevents duplicate records if A7670 retransmits after a flaky TCP reset.

**Database schema:**

```sql
-- One row per device; (region_id, station_id) is the sole identity.
-- No per-device CN exists — the entire fleet shares one client certificate (§2.1).
CREATE TABLE devices (
  id          SERIAL       PRIMARY KEY,
  region_id   SMALLINT     NOT NULL,
  station_id  SMALLINT     NOT NULL,
  last_seen   TIMESTAMPTZ,
  created_at  TIMESTAMPTZ  DEFAULT now(),
  UNIQUE (region_id, station_id)
);

-- Per-sample sensor readings; partitioned by time via TimescaleDB
CREATE TABLE weather_records (
  time        TIMESTAMPTZ  NOT NULL,  -- firmware time_stamp converted from Y2K epoch
  device_id   INT          NOT NULL REFERENCES devices(id),
  temperature REAL,                   -- °C
  humidity    REAL,                   -- %RH
  pressure    REAL,                   -- kPa
  light_par   SMALLINT,               -- µmol/s·m²  (0–2500)
  rainfall    REAL,                   -- mm/hr (cumulative)
  dew_point   REAL,                   -- °C
  bus_value   REAL                    -- Blast Unit of Severity
);

SELECT create_hypertable('weather_records', 'time');

-- Compress chunks older than 7 days; segment by device for efficient per-device queries
ALTER TABLE weather_records SET (
  timescaledb.compress,
  timescaledb.compress_segmentby = 'device_id'
);
SELECT add_compression_policy('weather_records', INTERVAL '7 days');

-- Idempotency guard: prevents duplicate rows when A7670 retransmits after a TCP reset
CREATE TABLE ingest_log (
  idempotency_key VARCHAR(320) PRIMARY KEY,  -- "{region_id:03d}{station_id:03d}:{first_sample_ts.isoformat()}"
  recorded_at     TIMESTAMPTZ DEFAULT now()
);
```

### 3.2 OTA Update Logic (Pull Model, Staged Rollout)

**Note on UPDATE_PATH:** The device firmware stores a configurable base URL (`UPDATE_PATH`, 64 bytes in Config Sector) accessible via CDC interface. The server mounts all device endpoints — ingest, OTA metadata poll, and OTA chunk download — under this same base path (Q-S1 Option B). Example:

- `UPDATE_PATH = "https://robin-gpu.cpe.ku.ac.th/api/v1/weather"` (no trailing slash; firmware appends one).
- Ingest endpoint: `https://robin-gpu.cpe.ku.ac.th/api/v1/weather/upload` (mTLS; §3.1).
- Metadata endpoint: `https://robin-gpu.cpe.ku.ac.th/api/v1/weather/?id=<rrrsss>` (mTLS; identity required).
- Chunk download: `https://robin-gpu.cpe.ku.ac.th/api/v1/weather/get_firmware?offset=X&length=512&id=<rrrsss>` (mTLS; identity required).

All three paths share one Nginx `location` block and one rate-limit zone, which simplifies mTLS enforcement and throttling.

**Device identity (`id` query parameter).** Because the fleet shares a single client certificate (§2.1), both OTA endpoints require an `id` query parameter carrying the 6-character decimal identity `%03u%03u` of `(region_id, station_id)`. Firmware builds this string from `Meta_Data_t`.

**Three-phase design (aligned with STM32 firmware):**

#### Phase 1: Device queries version & metadata

```
GET <UPDATE_PATH>/?id=<rrrsss>
Response (plain text token inside an HTML body):
  V.#####:L.$$$$$$$:H.<sha256hex>:W.<seconds>

  Parsed fields (delimited by ':'):
  - V.##### = server firmware version as uint32 (decimal, no padding)
  - L.$$$$$$$ = firmware image size in bytes (decimal)
  - H.<sha256hex> = SHA-256 of firmware image (64 lowercase hex chars)
  - W.<seconds> = rollout wait time for this device (uint32, decimal).
                   W.0 → device is cleared to download this cycle.
                   W.>0 → device must skip download; retry on next scheduled poll.
                   Server-side slotting rule: see §3.3.
```

**Response format:** HTML body containing the `V.#####:L.$$$$$$$:H.<sha256hex>:W.<seconds>` token anywhere in the response; device scans for the regex pattern and extracts values.

**Backward compatibility:** a response that omits the `:W.<seconds>` suffix is treated as `W.0` (download permitted). Lets pre-rollout firmware interoperate with a rollout-aware server, and vice-versa.

**Version check:** Device compares `V` against `FW_VERSION` constant. If `V` ≤ `FW_VERSION` → OTA skipped silently (treat as "no update"); no retries on parseable but non-matching body.

#### Phase 2: Device downloads chunks (resumable)

```
GET <UPDATE_PATH>/get_firmware?offset=<byte_offset>&length=<byte_count>&id=<rrrsss>
Response (binary):
  [<byte_count> bytes of firmware image] + [4-byte CRC32 (MPEG-2, little-endian)]
```

**Chunking:** Device downloads 512-byte chunks (`<byte_count>` = 512 per request). Offset query param supports resume after power loss (device scans FRAM download bitmap for missing chunks).

**Slot re-check:** The server re-evaluates the §3.3 slot gate on every chunk request using the `id` parameter; an out-of-slot device receives `429 Too Many Requests` instead of a chunk body. This is belt-and-suspenders against a misbehaving client that ignores `W.<seconds>` from Phase 1; the device's existing modem-error retry/backoff behaviour handles it correctly.

**CRC32 validation:** Device validates 4-byte CRC-32/MPEG-2 trailer appended by server; per-chunk integrity check protects against MCU↔modem UART bit-flips (A7670 known issue). If CRC mismatch, device retries chunk up to 3×; after 3 failures, aborts and waits for next scheduled check.

**Whole-image integrity:** Device accumulates SHA-256 during download; after all chunks received, compares SHA-256 against value from Phase 1. If mismatch, OTA aborted and device remains on current firmware.

#### Phase 3: Device verifies & installs

1. Bootloader reads `OtaControlBlock_t` from FRAM config sector.
2. If `ota_pending == 1` and image SHA-256 valid: program STM32 internal Flash page-by-page, read-back verify each page.
3. If all pages verified: clear `ota_pending`, jump to new application at 0x08008000.
4. New application must call `ota_confirm_success()` within 60 seconds; if timeout or hang, IWDG fires and bootloader rolls back to previous firmware.

**Note:** Firmware signing (Ed25519) is Phase 5 enhancement, not v1 release. Current design relies on SHA-256 transport integrity + TLS server authentication.

### 3.3 OTA Rollout Controls (Server-Side)

**Goal.** Flatten the concurrent-download egress spike. At 1000 devices × 512 KB, unthrottled rollout produces ~111 Mbps concurrent egress (§4) — exceeds a low-cost VPS cap and cannot be offloaded to object storage because the A7670E firmware does not follow HTTP 302 redirects and `update_path` is tightly bound to both endpoints.

**Design.** The server gates each device's download window with a deterministic 20-slot schedule spanning 10 days. Each version-check response carries a `W.<seconds>` wait time; the device skips the download when `W > 0`. Combined with the device-side jitter specified in `OTA_Firmware_Architecture.md §10.6`, this smooths the rollout to ~90 KB/s peak egress with zero added device state.

**Database schema:**

```sql
CREATE TABLE ota_campaigns (
  id SERIAL PRIMARY KEY,
  version INT NOT NULL UNIQUE,
  description TEXT,
  firmware_sha256 VARCHAR(64) NOT NULL,    -- SHA-256 hex digest, auto-computed on upload
  firmware_size   INT          NOT NULL,    -- byte count, auto-computed on upload
  firmware_file_path TEXT NOT NULL,         -- {FIRMWARE_DIR}/v{version}.bin (see §3.4)
  rollout_start TIMESTAMPTZ,                -- anchor for the slot schedule; set by "start"
  rollout_window_days INT DEFAULT 10,       -- tunable; num_slots = 2 * rollout_window_days
  slot_len_sec INT NOT NULL DEFAULT 43200,  -- seconds per slot; frozen at campaign creation from
                                            -- settings.SLOT_LEN_SEC (default 43200 = 12 h).
                                            -- Must match the device firmware upload cadence.
  target_cohort_ids TEXT[],                 -- optional allow-list of device ids (rrrsss strings);
                                            -- NULL = entire fleet. Used for manual canary overrides.
  status VARCHAR(32) DEFAULT 'draft',       -- draft, scheduled, in_progress, paused, completed, cancelled
                                            -- (Q-S8) 'cancelled' replaces the earlier 'rolled_back' name:
                                            -- cancelling halts further eligibility only; it does NOT
                                            -- install a prior image on devices that already updated.
                                            -- Field rollback = upload the prior binary as a new, higher-
                                            -- version campaign and run it through the normal lifecycle.
  success_rate NUMERIC,                     -- post-rollout: % of eligible devices that completed all chunks
  created_at TIMESTAMPTZ DEFAULT now(),
  updated_at TIMESTAMPTZ DEFAULT now()
);

-- Per-device per-chunk download tracking (Q-S13).
-- Written by the /get_firmware handler on every successfully served chunk.
-- Primary key prevents duplicates; ON CONFLICT DO NOTHING makes writes idempotent.
-- Source of truth for live rollout progress (GET /admin/campaign/{id}) and
-- terminal success_rate computation (set when campaign transitions to completed/cancelled).
CREATE TABLE download_completions (
  campaign_id INT          NOT NULL REFERENCES ota_campaigns(id),
  device_id   VARCHAR(6)   NOT NULL,        -- "{region:03d}{station:03d}" from ?id= param
  chunk_index INT          NOT NULL,        -- offset // 512; last chunk may be shorter than 512 B
  recorded_at TIMESTAMPTZ  DEFAULT now(),
  PRIMARY KEY (campaign_id, device_id, chunk_index)
);
```

**Slot computation (authoritative algorithm):**

```python
# SLOT_LEN_SEC is configurable via settings.SLOT_LEN_SEC (default 43200 = 12 h).
# It is frozen into ota_campaigns.slot_len_sec at campaign creation time so that
# mid-rollout config changes do not reshuffle device assignments.

def compute_wait(device_id: str, campaign) -> int:
    """
    device_id: 6-character decimal string "{region:03d}{station:03d}" from the
               ?id= query param (§3.2).
    Returns the number of seconds the device should wait before downloading.
    W == 0 means "download now"; W > 0 means "skip this cycle".
    """
    slot_len  = campaign.slot_len_sec               # frozen at campaign creation
    num_slots = campaign.rollout_window_days * 2    # default 20
    now       = datetime.now(timezone.utc)
    elapsed   = (now - campaign.rollout_start).total_seconds()
    now_slot  = min(num_slots - 1, max(0, int(elapsed // slot_len)))
    dev_slot  = zlib.crc32(device_id.encode("ascii")) % num_slots
    return 0 if dev_slot <= now_slot else (dev_slot - now_slot) * slot_len
```

**Key properties:**

- **Monotone in time.** Once `dev_slot ≤ now_slot`, every subsequent cycle also returns `W.0`. A device that fails mid-download automatically retries on the next poll cycle and resumes from its FRAM download bitmap — no server-side retry tracking.
- **Even distribution.** `crc32(id) mod 20` gives ≈50 ± 10 devices per slot at N=1000 (good enough for egress flattening).
- **Stateless per request.** `compute_wait` is a pure function of `(device_id, campaign.rollout_start, campaign.rollout_window_days, campaign.slot_len_sec, now)`. No per-device progress tables.
- **Works with stale clocks.** The comparison uses server time end-to-end; device RTC drift does not affect eligibility.

**Active-campaign selection:**

Server looks up the highest-version campaign where:

- `status = 'in_progress'` **and**
- `target_cohort_ids IS NULL OR cardinality(target_cohort_ids) = 0` (entire fleet — Q-S10) **or** the device id is in the list.

Empty arrays are normalised to `NULL` on insert (see §7 "Rollout Controls" checklist) so the two representations do not diverge over time.

The server does not track per-device firmware versions. Version filtering is handled entirely on the device side (§3.2 version check): if `V ≤ FW_VERSION`, the device silently skips download.

If the slot gate returns `W > 0`, the server still serves the token — with the wait time — so the device knows an update exists and will retry. If no campaign matches at all, the server returns "No update available" and the device stays on its current firmware.

**Enforcement on the download endpoint.** `GET /get_firmware` re-evaluates `compute_wait` on every request. A non-zero result produces `429 Too Many Requests`. This prevents a misbehaving client from defeating the rollout by skipping the Phase 1 check.

**Rollout workflow (admin):**

1. Upload firmware binary; server auto-assigns version, auto-computes SHA-256 and size, creates draft campaign.
2. (Optional) Set `target_cohort_ids`, `rollout_window_days`, or `slot_len_sec` before starting.
3. `POST /admin/campaign/{id}/start` sets `status='in_progress'`, `rollout_start=now()`, and freezes `slot_len_sec` from `settings.SLOT_LEN_SEC` if not already set.
4. Rollout proceeds automatically over `rollout_window_days`. Operators monitor download-completion metrics (chunks-served count per device from the `download_completions` table — see Q-S13).
5. `POST /admin/campaign/{id}/pause` or `/cancel` halts further eligibility by flipping `status`; in-flight downloads finish harmlessly (next cycle sees `status != 'in_progress'` → "No update available"). `cancel` is terminal and does **not** re-flash any device with the prior image (Q-S8); true field rollback is performed by uploading the prior binary as a new, higher-version campaign.

**Egress envelope (1000 devices, 512 KB image, 10-day window):**

| Metric | Value |
|--------|-------|
| Devices per 12 h slot (expected) | 50 ± ~10 |
| Data per slot | ≈26 MB |
| Peak concurrent downloads (with device-side jitter) | 2–3 |
| Average egress during slot | ≈14 KB/s |
| Peak egress | ≈90 KB/s |
| Total rollout egress | ≈512 MB over 10 days |

### 3.4 Firmware Image Storage

Firmware binaries are stored under `html/firmware/` in the application directory; metadata (version, size, SHA-256, path) is stored in PostgreSQL. All metadata fields are computed automatically by the server on upload — the admin supplies only the binary file. The directory is **not** accessible from a web browser (Nginx denies `/firmware/` — see §5).

**Filesystem layout:**

```
html/firmware/
└── v{version}.bin        ← raw firmware binary; filename is the canonical key
```

- Directory is created at deployment, mode `0750`; owned by `akp` in development, `iotsrv` in production.
- `html/firmware/` is excluded from version control (`.gitignore`).
- Files are written atomically: write to a `.tmp` path then `rename()` to the final name to avoid serving a partial image.
- The path is configurable via `FIRMWARE_DIR` in `config.py`. Per Q-S11 the value **must be an absolute path** — the app resolves it at startup and refuses to start on a relative or unwritable value. `html/etc/iot.env` is responsible for setting a concrete absolute path (typically `${html_dir}/firmware`); there is no usable relative default.

**Version assignment:** The server automatically assigns `version = MAX(version) + 1` from the `ota_campaigns` table (starting at 1 when no campaigns exist). The admin does **not** enter a version number.

**Firmware size ceiling.** The STM32L476RG application Flash partition is **480 KB** (see `OTA_Firmware_Architecture.md §6`). Uploads with `len(file_bytes) > MAX_FIRMWARE_SIZE_BYTES` (default `480 * 1024`, configurable via `settings.MAX_FIRMWARE_SIZE_BYTES`) are rejected with `413 Request Entity Too Large` **before** the file is written to disk. This prevents a mis-built binary from ever reaching a device, where the firmware would also reject it (§3.2 Phase 1 size check) but only after burning an OTA cycle. The limit is a server-side admin hygiene check; the device-side rejection remains the authoritative gate.

**Old firmware cleanup (Q-S4 resolution):** Binaries are garbage-collected conservatively so that an in-progress rollout can never lose its backing file. A `FIRMWARE_KEEP_N = 3` retention window is applied on the set of campaigns whose status is **terminal** (`completed` or `cancelled`):

- Campaigns in `draft`, `in_progress`, or `paused` are **never** eligible for deletion — their binary must remain on disk.
- Among terminal campaigns, the `FIRMWARE_KEEP_N` most recent (by `version` DESC) are retained; any older ones have their `.bin` deleted.
- The cleanup is invoked **after** each successful upload and **after** any status transition into a terminal state.

`FIRMWARE_KEEP_N = 3` is sized for the worst realistic case: the latest completed campaign (current production image), the one before it (common target for "roll back by re-upload"), plus one head-room slot so operators are never forced to delete a recent binary to free a slot. Increase the setting if the operator workflow benefits from a deeper archive; decrease is not recommended.

**Metadata computed on upload:**

| Field | Source | Stored in |
|-------|--------|-----------|
| `version` | `MAX(ota_campaigns.version) + 1` | `ota_campaigns.version` |
| `firmware_size` | `len(file_bytes)` | `ota_campaigns.firmware_size` |
| `firmware_sha256` | `hashlib.sha256(file_bytes).hexdigest()` | `ota_campaigns.firmware_sha256` |
| `firmware_file_path` | `{FIRMWARE_DIR}/v{version}.bin` | `ota_campaigns.firmware_file_path` |

The `L.` field in the OTA metadata response (§3.2 Phase 1) is served directly from `ota_campaigns.firmware_size`; the `H.` field from `ota_campaigns.firmware_sha256`. No file I/O is needed to answer a metadata poll.

**Admin upload flow:**

1. Admin opens `/admin/firmware/upload` in browser.
2. Selects `.bin` file (no version number input).
3. Form POST to `POST /admin/firmware/upload`.
4. Server: compute `new_version = MAX(version) + 1` → read bytes → compute SHA-256 + size → write `v{new_version}.bin` atomically → insert draft `ota_campaigns` row → run the retention sweep (delete `.bin` files whose campaigns are terminal and older than the `FIRMWARE_KEEP_N = 3` most-recent terminal campaigns; non-terminal campaigns are never touched).
5. Response returns `version`, `firmware_sha256`, `firmware_size` for admin review before starting a campaign.

**Integrity check on campaign start:** `POST /admin/campaign/{id}/start` recomputes SHA-256 from disk and rejects with `409` if it does not match `firmware_sha256` in the DB, catching silent filesystem corruption or accidental overwrite.

---

## 4. Performance & Scalability

| Metric | Estimate | Notes |
|--------|----------|-------|
| **Ingest throughput** | 1000 dev × 2 uploads/day = 2000 req/day | Trivial; batching means low QPS |
| **Ingest payload** | ~200 bytes/device per upload | Gzip compression + small JSON overhead |
| **TimescaleDB write rate** | ~50 rows/sec (aggregated across fleet) | Well within single vCPU capacity |
| **OTA bandwidth (unthrottled)** | 1000 × 512 KB in ≈15 min = ~4.5 Gbit burst | Would exceed any low-cost VPS egress cap; avoided by the §3.3 slot schedule |
| **OTA bandwidth (20-slot rollout + 30 min device jitter)** | ≈90 KB/s peak, ≈14 KB/s average per 12 h slot; ~512 MB total over 10 days | Comfortable on 1–2 vCPU / 1 Gbit VPS |
| **Connection pooling** | PostgreSQL + asyncpg pool size 20 | Handles 100 concurrent uploads comfortably |

**Recommendations:**

- OTA egress is controlled by the §3.3 slot schedule; object-storage offload is not viable for the current A7670E firmware (no HTTP 302 support). Re-evaluate object storage if fleet size or image size grows ≥ 10×.
- Monitor ingest lag; alert if > 5 minutes (indicates database bottleneck or network saturation).

---

## 5. Nginx Configuration Checklist

**Two listeners, two trust stores (Q-S9).** Devices and admin browsers terminate TLS on **separate `server{}` blocks** — one anchored to the private CA (devices, mTLS required), the other to a public CA (admin, browser-trusted). The device listener is bound to `robin-gpu.cpe.ku.ac.th` and the admin listener to `adm.robinlab.cc`; both listen on 443 and are selected by SNI. Separating them eliminates any risk of provisioning the wrong trust anchor into the A7670E modem (see §2.3) and keeps the admin path free of client-cert prompts.

**Rate-limit key (Q-S3).** All device traffic (ingest + OTA) carries `?id=<rrrsss>` once Phase 3.1 firmware ships. The device listener therefore keys `limit_req` on `$arg_id` — a true per-device throttle — instead of `$ssl_client_s_dn`, which resolves to a single fleet-wide value because the whole fleet shares one client certificate (§2.1).

```nginx
# /etc/nginx/conf.d/iot_server.conf
# conf.d/ files are included inside the http{} block of nginx.conf, so
# limit_req_zone is valid here (at the top level of this file, outside any server{} block).
limit_req_zone $arg_id zone=device_api:10m rate=10r/s;

upstream fastapi_backend {
    server 127.0.0.1:8000;
    keepalive 32;
}

#
# Device listener — mTLS with the private CA. All device traffic lives under
# /api/v1/weather/ (Q-S1 Option B): ingest, OTA metadata poll, OTA chunk download.
#
server {
    listen 443 ssl;
    http2 on;
    server_name robin-gpu.cpe.ku.ac.th;

    # Server certificate — signed by the private intermediate CA and trusted by
    # the A7670E modem via the single CA cert injected at commissioning (see §2.3).
    ssl_certificate /etc/ssl/certs/robin-gpu.cpe.ku.ac.th.crt;
    ssl_certificate_key /etc/ssl/private/robin-gpu.cpe.ku.ac.th.key;

    # Private CA chain for device client cert validation.
    # Replace {html_dir} with the absolute path to the html/ deployment directory.
    ssl_client_certificate {html_dir}/pki/private_ca_chain.pem;
    ssl_crl               {html_dir}/pki/ca.crl;   # updated weekly by scripts/refresh_crl.sh
    ssl_verify_client     on;                      # mandatory on this vhost — there is no admin path here
    ssl_verify_depth      2;

    ssl_protocols        TLSv1.3 TLSv1.2;
    ssl_ciphers          HIGH:!aNULL:!MD5;
    ssl_prefer_server_ciphers on;

    # Single mTLS-enforced block covering ingest + OTA (Q-S1 Option B):
    #   POST /api/v1/weather/upload                  — sensor ingest
    #   GET  /api/v1/weather/?id=<rrrsss>            — OTA metadata poll
    #   GET  /api/v1/weather/get_firmware?...&id=... — OTA chunk download
    location /api/v1/weather/ {
        if ($ssl_client_verify != SUCCESS) { return 403; }
        proxy_set_header X-SSL-Client-Verify $ssl_client_verify;
        proxy_pass http://fastapi_backend;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        limit_req zone=device_api burst=20;   # per-$arg_id, see Q-S3
    }

    # Firmware binaries are served only via the /get_firmware handler.
    location /firmware/ { deny all; return 404; }

    # Everything else on the device vhost is closed.
    location / { return 404; }
}

#
# Admin listener — public CA (Let's Encrypt), no client cert. Browsers only.
#
server {
    listen 443 ssl;
    http2 on;
    server_name adm.robinlab.cc;

    ssl_certificate     /etc/letsencrypt/live/adm.robinlab.cc/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/adm.robinlab.cc/privkey.pem;

    ssl_verify_client off;                   # admin has no client cert
    ssl_protocols     TLSv1.3 TLSv1.2;
    ssl_ciphers       HIGH:!aNULL:!MD5;
    ssl_prefer_server_ciphers on;

    location /admin/ {
        proxy_pass http://fastapi_backend;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
    }

    location / { return 404; }
}
```

---

## 6. FastAPI Endpoints

### Device endpoints (mTLS enforced by Nginx; FastAPI verifies `X-SSL-Client-Verify: SUCCESS`; device identity from payload/query param only)

```python
import struct
from datetime import datetime, timedelta, timezone

# Y2K epoch: firmware timestamps are seconds since 2000-01-01 00:00:00 UTC
_Y2K_EPOCH = datetime(2000, 1, 1, tzinfo=timezone.utc)
# Fixed-point scale: FIXEDPT_BITS=16, FIXEDPT_WBITS=9 → FIXEDPT_FBITS=7 → scale=2^7
_FIXEDPT_SCALE = 128
# Weather_Data_Packed_t: uint32 ts, int16×3 (temp/hum/pres), uint16 light, int16×3 (rain/dew/bus)
_CHUNK_STRUCT = struct.Struct("<IhhhHhhh")  # 18 bytes


def _parse_chunks(payload: bytes, count: int) -> list[dict]:
    records = []
    for i in range(count):
        ts, temp, hum, pres, light, rain, dew, bus = _CHUNK_STRUCT.unpack_from(payload, 5 + i * 18)
        records.append({
            "timestamp":   _Y2K_EPOCH + timedelta(seconds=ts),
            "temperature": temp / _FIXEDPT_SCALE,
            "humidity":    hum  / _FIXEDPT_SCALE,
            "pressure":    pres / _FIXEDPT_SCALE,
            "light_par":   light,
            "rainfall":    rain / _FIXEDPT_SCALE,
            "dew_point":   dew  / _FIXEDPT_SCALE,
            "bus_value":   bus  / _FIXEDPT_SCALE,
        })
    return records


import zlib

def compute_wait(device_id: str, campaign) -> int:
    """Return seconds the device should wait before downloading. 0 = go now."""
    slot_len  = campaign.slot_len_sec               # frozen at campaign creation; see §3.3
    num_slots = campaign.rollout_window_days * 2
    elapsed   = (datetime.now(timezone.utc) - campaign.rollout_start).total_seconds()
    now_slot  = min(num_slots - 1, max(0, int(elapsed // slot_len)))
    dev_slot  = zlib.crc32(device_id.encode("ascii")) % num_slots
    return 0 if dev_slot <= now_slot else (dev_slot - now_slot) * slot_len


@app.post("/api/v1/weather/upload")
async def ingest_sensor_data(
    request: Request,
    payload: bytes = Body(..., media_type="application/octet-stream")
):
    # Identity comes from the payload — the fleet shares a single client cert (§2.1).
    region_id, station_id, chunk_count = struct.unpack("<HHB", payload[:5])
    device = await db.upsert_device_by_region_station(region_id, station_id)

    chunks = _parse_chunks(payload, chunk_count)
    first_ts = chunks[0]["timestamp"].isoformat()
    idempotency_key = f"{region_id:03d}{station_id:03d}:{first_ts}"
    if await db.ingest_log_exists(idempotency_key):
        return {"status": "duplicate", "recorded_at": first_ts}

    await db.insert_weather_records(device.id, chunks)
    await db.ingest_log_insert(idempotency_key)
    return {"status": "ok"}


@app.get("/api/v1/weather/")  # UPDATE_PATH root — firmware-defined base URL (Q-S1 Option B)
async def get_ota_metadata(
    id: str = Query(..., pattern=r"^\d{6}$",
                    description="Device identity: 3-digit region + 3-digit station"),
):
    campaign = await db.get_active_campaign_for_device(id)
    if not campaign:
        return HTMLResponse("<html><body>No update available</body></html>")

    # Plain-text token in HTML body: V.#####:L.$$$$$$$:H.<sha256hex>:W.<seconds>
    wait_seconds = compute_wait(id, campaign)
    token = (f"V.{campaign.version}"
             f":L.{campaign.firmware_size}"
             f":H.{campaign.firmware_sha256}"
             f":W.{wait_seconds}")
    return HTMLResponse(f"<html><body>{token}</body></html>")


@app.get("/api/v1/weather/get_firmware")  # UPDATE_PATH/get_firmware — firmware chunked download (Q-S1)
async def download_ota_chunk(
    id: str = Query(..., pattern=r"^\d{6}$"),
    offset: int = Query(0, description="Byte offset in firmware image"),
    length: int = Query(512, description="Number of bytes to read"),
):
    campaign = await db.get_active_campaign_for_device(id)
    if not campaign:
        raise HTTPException(404, "No active OTA campaign")

    # Re-check the slot gate on every chunk — defence against clients that ignore W.
    if compute_wait(id, campaign) != 0:
        raise HTTPException(429, "Not your slot yet")

    try:
        chunk_data = read_chunk(campaign.firmware_file_path, offset, length)
    except Exception as e:
        raise HTTPException(500, f"Failed to read firmware chunk: {str(e)}")

    crc32_val = crc32_mpeg2(chunk_data)
    return Response(chunk_data + crc32_val.to_bytes(4, "little"),
                    media_type="application/octet-stream")
```

### Admin endpoints (authenticated by username/password → JWT; accessed by humans via browser)

```python
import hashlib
import os
from fastapi.responses import HTMLResponse
from fastapi import Query

# Helper: compute CRC-32/MPEG-2 (matching firmware implementation)
def crc32_mpeg2(data: bytes) -> int:
    """
    Compute CRC-32/MPEG-2 polynomial 0x04C11DB7, initial 0xFFFFFFFF,
    no reflection, no final XOR. Matches firmware's shared/crc32.c.
    Note: zlib/binascii.crc32 is CRC-32 (reflected, poly 0xEDB88320) and
    will NOT produce matching values — must implement directly.
    Production: replace this bit-serial loop with a 256-entry table lookup.
    """
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)
            crc &= 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF

@app.post("/admin/firmware/upload")
async def upload_firmware(
    current_user: User = Depends(get_current_user),
    file: UploadFile,
    # No version parameter — server assigns version = MAX(version) + 1 automatically
):
    if current_user.role != "admin":
        raise HTTPException(403, "Unauthorized")

    firmware_bytes = await file.read()
    if len(firmware_bytes) > settings.max_firmware_size_bytes:
        # Device Flash partition is 480 KB (OTA_Firmware_Architecture.md §6).
        raise HTTPException(413, f"Firmware exceeds {settings.max_firmware_size_bytes} bytes")
    firmware_sha256_hex = hashlib.sha256(firmware_bytes).hexdigest()
    firmware_size = len(firmware_bytes)

    # Auto-assign version; MAX returns None when table is empty → start at 1
    new_version = (await db.get_max_firmware_version() or 0) + 1

    # Write atomically: tmp → rename to avoid serving a partial image
    firmware_path = os.path.join(settings.firmware_dir, f"v{new_version}.bin")
    tmp_path = firmware_path + ".tmp"
    try:
        with open(tmp_path, "wb") as f:
            f.write(firmware_bytes)
        os.replace(tmp_path, firmware_path)
    except Exception as e:
        raise HTTPException(500, f"Failed to save firmware: {str(e)}")

    # Insert new draft campaign; all metadata auto-computed
    campaign = await db.insert_campaign(OtaCampaign(
        version=new_version,
        firmware_sha256=firmware_sha256_hex,
        firmware_size=firmware_size,
        firmware_file_path=firmware_path,
        status="draft",
    ))

    # Retention sweep (Q-S4): never touch draft/in_progress/paused campaigns —
    # they still have work to do. Among terminal (completed/cancelled) campaigns,
    # keep the FIRMWARE_KEEP_N most recent by version DESC and delete the rest.
    await _sweep_firmware_retention(settings.firmware_keep_n)

    return {
        "campaign_id": campaign.id,
        "version": new_version,
        "firmware_sha256": firmware_sha256_hex,
        "firmware_size": firmware_size,
    }


async def _sweep_firmware_retention(keep_n: int) -> None:
    """
    Garbage-collect firmware binaries for terminal campaigns older than the
    keep_n most recent. Called after upload and after status transitions into
    a terminal state (completed, cancelled). Non-terminal campaigns are skipped.
    """
    terminal = await db.list_campaigns_by_status(
        ("completed", "cancelled"), order="version DESC")
    for campaign in terminal[keep_n:]:
        if not campaign.firmware_file_path:
            continue
        try:
            os.remove(campaign.firmware_file_path)
        except FileNotFoundError:
            pass  # already absent; not an error

@app.post("/admin/campaign/{campaign_id}/start")
async def start_rollout(
    campaign_id: int,
    rollout_window_days: int = Query(10, ge=1, le=30),
    slot_len_sec: int = Query(None, description="Override slot length; defaults to settings.SLOT_LEN_SEC"),
    target_cohort_ids: list[str] = Query(None),
    current_user: User = Depends(get_current_user)
):
    if current_user.role != "admin":
        raise HTTPException(403, "Unauthorized")

    campaign = await db.get_campaign(campaign_id)
    if not campaign or campaign.status != "draft":
        raise HTTPException(404 if not campaign else 409, "Campaign not found or not in draft state")

    # Recompute SHA-256 from disk to catch silent corruption before rollout
    if hashlib.sha256(open(campaign.firmware_file_path, "rb").read()).hexdigest() != campaign.firmware_sha256:
        raise HTTPException(409, "Firmware SHA-256 mismatch — file may be corrupt")

    campaign.status = "in_progress"
    campaign.rollout_start = datetime.now(timezone.utc)
    campaign.rollout_window_days = rollout_window_days
    campaign.slot_len_sec = slot_len_sec or settings.slot_len_sec
    if target_cohort_ids:
        campaign.target_cohort_ids = target_cohort_ids
    await db.update_campaign(campaign)

    return {"status": "in_progress", "rollout_window_days": rollout_window_days, "slot_len_sec": campaign.slot_len_sec}
```

---

## 7. Implementation Checklist

### Security (Blocking)

- [ ] Private CA setup (root key offline, intermediate CA online)
- [ ] Device cert provisioning pipeline (bulk generate + distribute)
- [ ] CRL endpoint + Nginx configuration: `ssl_verify_client optional` server-wide; `if ($ssl_client_verify != SUCCESS) return 403` in device/OTA locations only; admin path carries no client cert
- [ ] Ed25519 firmware signing + bootloader verification (see OTA_Firmware_Architecture.md §9)
- [ ] Rate limiting per device cert (`limit_req_zone` in http context)

### Data Ingestion

- [ ] Binary protocol parser in Python (match firmware's packed struct)
- [ ] TimescaleDB schema for sensor records (see §3.1 database schema)
- [ ] Idempotency key deduplication
- [ ] UTC time normalization (device NTP syncs to server time)

### OTA

- [ ] `ota_campaigns` table schema (see §3.3 database schema)
- [ ] Campaign state machine (`draft → in_progress → paused/completed/cancelled`)
- [ ] `GET /api/v1/weather/?id=<rrrsss>` endpoint returning plain-text `V.#####:L.$$$$$$$:H.<sha256hex>:W.<seconds>`
- [ ] `GET /api/v1/weather/get_firmware?offset=X&length=Y&id=<rrrsss>` endpoint appending 4-byte CRC32/MPEG-2 trailer
- [ ] `compute_wait()` per §3.3; `429` from `/get_firmware` when `compute_wait() != 0`
- [ ] Auto-assign version, auto-compute SHA-256 and size on firmware upload (§3.4)
- [ ] Reject uploads with `len(file_bytes) > MAX_FIRMWARE_SIZE_BYTES` (default 480 KB) with HTTP 413 (§3.4 "Firmware size ceiling")
- [ ] Success rate tracking via `download_completions` table — `/get_firmware` handler inserts `(campaign_id, device_id, chunk_index)` on every served chunk; `success_rate` computed at terminal status transition over devices that have all `(firmware_size + 511) // 512` chunk rows (Q-S13)
- [ ] Rollback alert on low success rate (< 80% after rollout completes)

### Observability

- [ ] Prometheus metrics: ingest latency, cert-verify errors, OTA success rate
- [ ] Loki logs aggregation for FastAPI + Nginx
- [ ] Grafana dashboards: device heartbeat, OTA rollout status

### Admin UI

- [ ] HTMX-based firmware upload form + progress bar
- [ ] Campaign management UI (create, start, pause, view status)
- [ ] Device management (list, cert status, version, last-seen)
- [ ] JWT login + RBAC for admin/operator/viewer roles

---

## 8. Device-Side Requirements (Firmware Integration)

From `CLAUDE.md` / OTA modules:

| Requirement | Firmware component | Status |
|-------------|-------------------|--------|
| TLS 1.2+ with mTLS | A7670E AT+CSSLCFG + cert provisioning | Required; cert provisioned at commissioning |
| NTP time sync before TLS | `at_channel_send_cntp()` in a7670_at_channel.c | Done (commit d517c19) |
| Binary ingest (packed struct) | maintask.c → ssluploadtask.c | Match protocol definition in `weather_data.h` |
| OTA metadata polling (plain text) | `OtaManagerTask` in ota_manager_task.c | GET <UPDATE_PATH>/?id=<rrrsss> (UPDATE_PATH = `/api/v1/weather`) → parses `V.#####:L.$$$$$$$:H.<sha256hex>:W.<seconds>` |
| OTA rollout gate | `OtaManagerTask` in ota_manager_task.c | On `W>0`: return to IDLE, no download. On `W==0`: proceed to download. |
| Chunk download with offset/length | `a7670_ssl_downloader.c` (URL built in `ota_manager_task.c`) | GET <UPDATE_PATH>/get_firmware?offset=X&length=512&id=<rrrsss>; validates per-chunk CRC32; treats `429` like other HTTP errors (retry/back-off) |
| Device identity bounds | `DB_SetMeta()` / `ota_manager_task.c` | `region_id`, `station_id` constrained to 0–999; URL builder applies `% 1000` before formatting `%03u%03u` (Q-S7). |
| SHA-256 whole-image verification | `ota_manager_task.c` + bootloader | SHA-256 accumulated during download; bootloader re-verifies before Flash programming |
| Firmware signing (Ed25519) | Bootloader `boot_flash.c` | **Phase 5 (future)** — not implemented in v1. Current v1 relies on SHA-256 + TLS cert auth. |
| Bootloader rollback on timeout | Bootloader + IWDG | New app must call `ota_confirm_success()` within 60s; IWDG fires → previous FW retained |

---

## 9. Deployment & Ops

**Host:** `robin-gpu.cpe.ku.ac.th` (Ubuntu); 30 GB SSD sufficient for 1000 devices + 90 days retention.

**CI/CD:**

- Deploy server: `git pull` + `pip install` + `systemctl restart` (see `scripts/deploy.sh`)

**Monitoring alerts:**

- Ingest lag > 5 min → page ops
- Cert-verify errors spike → check CRL freshness
- OTA campaign success < 80% after 24 hours → pause & investigate

---

## 10. Resolved Design Questions

These were the open cross-document issues at spec v1.0. All are now resolved; the body of this spec reflects the accepted decisions. Each entry retains the original framing so the rationale survives for future readers.

### Q-S1 — URL path layout inconsistency → **RESOLVED (Option B)**

All device traffic (ingest, OTA metadata poll, OTA chunk download) lives under `/api/v1/weather/`. One mTLS-enforced Nginx location block, one rate-limit zone; firmware builds URLs from `server_name + update_path + "/"` so the base path is a config value.

### Q-S2 — Firmware does not yet send `?id=` or parse `W.` → **RESOLVED (Phase 3.1)**

`ota_manager_task.c` appends `?id=%03u%03u` to both OTA URLs; `parse_version_response()` extracts the optional `W.<seconds>` suffix (missing → `W.0`). On `W > 0` the state machine returns to `OTA_STATE_IDLE` without downloading. See `IMPLEMENTATION_STATUS.md` Phase 3.1.

### Q-S3 — Rate-limit key is fleet-wide when cert is shared → **RESOLVED**

Every Phase 3.1+ device request carries `?id=<rrrsss>`; §5 keys `limit_req_zone` on `$arg_id` for a true per-device throttle (10 r/s, burst 20).

### Q-S4 — Previous firmware deletion can break active downloads → **RESOLVED (keep last N terminal)**

`FIRMWARE_KEEP_N = 3` retention sweep (see §3.4): draft/in-progress/paused campaigns are never eligible for deletion; among terminal campaigns the 3 most recent by version keep their `.bin`. Covers current production image, prior image (for re-upload rollback), plus one head-room slot. Tunable via `settings.FIRMWARE_KEEP_N`.

### Q-S5 — `success_rate` population path → **RESOLVED (status-change path)**

**Resolution:** Adopted the **status-change path**. `success_rate` is computed and written once when an admin transitions a campaign to a terminal state (`completed` or `cancelled`). The "< 80% after 24 h" alerting in §9 reads this column for post-rollout review. Live in-rollout progress (during the 10-day window) is derived on demand by the `GET /admin/campaign/{id}` endpoint from the `download_completions` table (Q-S13) — not from `success_rate`. No background scheduler is required.

### Q-S6 — Chunk-count arithmetic → **RESOLVED**

Use ceiling division `(firmware_size + 511) // 512` everywhere chunk totals matter (progress readouts, success-rate computation).

### Q-S7 — `region_id` / `station_id` value range → **RESOLVED (crop mod 1000)**

Both fields constrained to 0–999 (documented in `nv_database.h`). Firmware applies `% 1000` at URL-build time so any out-of-range stored value still produces a valid 6-character `id`.

### Q-S8 — Campaign terminology → **RESOLVED (`rolled_back` → `cancelled`)**

Terminal abort status renamed to `cancelled`. True field rollback = upload the prior binary as a new higher-version campaign (see §3.3 workflow step 5).

### Q-S9 — Server certificate trust at the device → **RESOLVED (separate listeners)**

Two `server{}` blocks on port 443, selected by SNI: `api.iot.example.com` (private CA server cert + mTLS) for devices; `admin.iot.example.com` (Let's Encrypt) for browsers. Device commissioning provisions only the private CA root to the modem.

### Q-S10 — `target_cohort_ids` NULL vs empty-array → **RESOLVED**

Both `IS NULL` and `cardinality = 0` mean "entire fleet" in the selection SQL. Admin write path normalises empty arrays to `NULL` on insert to keep a single canonical representation.

### Q-S11 — `firmware_dir` relative vs absolute path → **RESOLVED (absolute at startup)**

`FIRMWARE_DIR` is resolved to an absolute path at app startup (`config.py`); app refuses to start if the value is relative or unwritable. `html/etc/iot.env` must set `FIRMWARE_DIR=/absolute/path`.

### Q-S12 — PostgreSQL / TimescaleDB install → **RESOLVED (PG 17 + TSDB from source)**

Production host runs PostgreSQL 17; TimescaleDB built from source. No Timescale apt package used; CI cannot rely on package names.

### Q-S13 — Success-rate source of truth → **RESOLVED (counter table)**

The `/get_firmware` handler inserts `(campaign_id, device_id, chunk_index)` into the `download_completions` table (`ON CONFLICT DO NOTHING`) after serving each chunk body. This table is the sole source for both live rollout progress (`GET /admin/campaign/{id}` derived aggregate) and the terminal `success_rate` value written at `completed`/`cancelled` transition. Nginx access-log parsing is not used. Schema added to §3.3.

---

## 11. References

- **Device firmware:** [CLAUDE.md](CLAUDE.md), [OTA_Firmware_Architecture.md](OTA_Firmware_Architecture.md)
- **OTA protocol:** `shared/ota_control_block.h`, `lib/A7670/a7670_ssl_downloader.h`
- **Binary schema:** `lib/utils/weather_data.h` (source of truth for packed struct layout)
- **NTP sync:** `ntp_manual.md` (device time synchronization before TLS)
