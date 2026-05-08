# Server Verification Plan (Python Test Harness)

> Python black-box verifiers that exercise the deployed server from a dev machine.
> Source tree: `server_test/`.
> Tests target the live server at `https://robin-gpu.cpe.ku.ac.th/` (or a `STAGING_URL` override).
> Companion to `Server_Implementation_Plan.md` — every server phase has a matching test group.
>
> **Goal:** catch regressions on the wire, not in code. Unit tests for server internals live in `html/tests/`. This harness simulates real devices and real admins against the real (or staging) server.

---

## Design Principles

- **Mock device = real TLS client.** Use `httpx` with a device client cert/key to prove mTLS, rate limits, and CN authorisation all behave as designed.
- **No server-side imports.** Tests must not `from app import ...`. The target is a HTTP contract, not Python internals.
- **Fixtures match firmware bit patterns.** Binary payloads constructed with `struct.pack` using the exact format from `lib/utils/weather_data.h`. CRC-32 fixtures cross-checked against `shared/crc32.c` output.
- **Idempotent & safe.** Tests use dedicated fixture devices (`TEST-*` CNs) and campaign versions in a reserved range (`version >= 900000000`). Teardown removes test rows; production devices are never touched.
- **Configurable target.** Base URL, admin credentials, cert paths loaded from a `.env` file (gitignored). Default target is staging.

---

## Target Directory Layout (inside `server_test/`)

```
server_test/
├── README.md                  ← how to run, env vars, cert provisioning for tests
├── pyproject.toml             ← ruff + pytest config
├── requirements.txt           ← httpx[http2], pytest, pytest-asyncio, cryptography, python-dotenv
├── conftest.py                ← shared fixtures: base_url, device_client, admin_client, db_cleanup
├── .env.example               ← STAGING_URL, ADMIN_USER, ADMIN_PASS, CA_BUNDLE, DEVICE_CERT_DIR
├── lib/
│   ├── __init__.py
│   ├── packed.py              ← encode/decode Weather_Data_Packed_t exactly as firmware does
│   ├── fixedpt.py             ← S9.7 encoder (mirror of lib/utils/fixedptc.h; cross-checked)
│   ├── crc32.py               ← CRC-32/MPEG-2 (mirror of shared/crc32.c; cross-checked)
│   ├── mock_device.py         ← MockDevice class: upload(), ota_poll(), ota_download_all()
│   └── admin.py               ← AdminClient: login, upload_firmware, start/pause/cancel campaign
├── tests/
│   ├── test_ingest.py         ← T1-series (Phase 3 of implementation)
│   ├── test_ota_device.py     ← T2-series (Phase 5)
│   ├── test_admin_auth.py     ← S6 auth/RBAC integration tests (7 tests; not a named T-series)
│   ├── test_admin_campaign.py ← T3-series (Phase 7)
│   ├── test_admin_ui.py       ← S8 admin UI E2E tests (14 tests; not a named T-series)
│   ├── test_mtls.py           ← T4-series (Phase 4)
│   ├── test_load.py           ← T5-series (soak + rate-limit)
│   └── test_sensor_data.py   ← T6-series (Phase 11: sensor data browse UI)
├── fixtures/
│   ├── firmware_small.bin     ← 2 KB deterministic payload for OTA tests
│   ├── firmware_large.bin     ← 400 KB payload mimicking real firmware size
│   └── packed_samples.json    ← hex-encoded Weather_Data_Packed_t samples + expected float decode
└── scripts/
    ├── provision_test_certs.sh  ← request N test device certs from server PKI
    └── cleanup_test_rows.py     ← removes TEST-* devices + version >= 900000000 campaigns
```

---

## Phase T0 — Harness Bootstrap

- [x] T0-1 Scaffold `server_test/` per layout; add `requirements.txt`; `pip install -r requirements.txt` succeeds on Python 3.12 ✓
- [x] T0-2 `conftest.py`: load `.env`; expose `dev` (INTERNAL_URL + injected header), `dev_mtls` (BASE_URL + real cert), `dev_no_cert`, `db`, `db_cleanup` fixtures; skip with message when env var missing ✓
- [x] T0-3 `lib/crc32.py`: table-based CRC-32/MPEG-2; `_selfcheck()` at import time; `lib/test_parity.py` pytest tests: empty-input + standard "123456789" check value `0x0376E6E7` + accumulation parity + confirmed differs from `zlib.crc32` ✓
- [x] T0-4 `lib/fixedpt.py`: `to_fixed` / `from_fixed` S9.7; `_selfcheck()` at import; `lib/test_parity.py` pytest tests: 6 known vectors + round-trip across full range + sign-extension ✓
- [x] T0-5 `lib/packed.py`: `encode(region, station, samples)` + `Sample` dataclass; `lib/test_parity.py` pytest tests: lengths, header bytes, temperature encoding ✓
- [x] T0-6 **N/A** — fleet uses one shared client cert (Arch §2.1); no per-device CNs. One shared test cert issued via `scripts/issue_device_cert.sh weather-test` from Phase 4 Deploy is sufficient ✓
- [x] T0-7 Verification: `pytest server_test/lib/` runs the parity tests (CRC, fixedpt, packed) green — run locally once `pip install -r server_test/requirements.txt` completes ✓

---

## Phase T1 — Ingest Path (mirrors Server Phase 3)

Tests in `server_test/tests/test_ingest.py`. Primary mode: `INTERNAL_URL` + injected header (no mTLS needed). DB assertions require `TEST_DB_DSN`. Region 999 reserved; `db_cleanup` pre- and post-purges all region=999 rows.

- [x] T1-1 Happy path: single chunk → `200 {"status":"ok"}`; DB assertion: float fields ±1 LSB (`test_t1_1_happy_path_response` + `test_t1_1_happy_path_db_float_fields`) ✓
- [x] T1-2 Max batch: 28 chunks × 18 B + 5 B = 509 B → `200`; DB assertion: 28 rows in `weather_records` (`test_t1_2_max_batch_response` + `test_t1_2_max_batch_row_count`) ✓
- [x] T1-3 Idempotency: same payload twice → `{"status":"duplicate"}`; row count unchanged (`test_t1_3_idempotency`) ✓
- [x] T1-4 Field boundaries: `-256.0` and `+255.9921875` accepted and stored within ±1 LSB (`test_t1_4_field_boundaries_response` + `test_t1_4_field_boundaries_db`) ✓
- [x] T1-5 Malformed header (count=5, body=4 records) → `400`; no rows inserted (`test_t1_5_count_mismatch_returns_400` + `test_t1_5_no_rows_on_bad_payload`) ✓
- [x] T1-6 First-seen device auto-upsert → `200`; `devices` row created with `last_seen ≥ upload_start` (`test_t1_6_new_device_response` + `test_t1_6_new_device_db_row`) ✓
- [x] T1-7 Idempotency key format: actual format is `"{region:03d}{station:03d}:{iso_datetime}"` (e.g. `999001:2024-05-19T00:00:00+00:00`) — the plan example showed a unix timestamp but the code stores ISO datetime from `datetime.isoformat()`; test pins the actual format (`test_t1_7_idempotency_key_format`) ✓
- [x] T1-8 Missing `X-SSL-Client-Verify` header → `403` (hits FastAPI directly without header via `dev_no_cert`; `test_t1_8_missing_verify_header_returns_403`) ✓
- [x] T1-9 Y2K epoch 0 → accepted; stored as `2000-01-01T00:00:00+00:00` TIMESTAMPTZ (`test_t1_9_y2k_epoch_zero_response` + `test_t1_9_y2k_epoch_zero_stored_correctly`) ✓

---

## Phase T2 — OTA Download (mirrors Server Phase 5)

All device requests pass `?id=<rrrsss>` (6-char decimal, `%03d%03d` of region/station). Slot schedule defined in Arch §3.3; metadata token regex: `V\.\d+:L\.\d+:H\.[0-9a-f]{64}(?::W\.\d+)?`.

- [x] T2-1 No active campaign: `GET /?id=042001` returns HTML body without a metadata token match ✓
- [x] T2-2 Active campaign, device in current slot: metadata token matches regex; `V`, `L`, `H` parse to known values from `fixtures/firmware_small.bin`; `W` field either absent or `W.0` ✓
- [x] T2-2a `W` field parsing: with `rollout_start` fresh and `rollout_window_days=10`, pick a device whose `crc32(id) % 20 > 0` → response contains `W.<positive>`; assert `W` is a multiple of 43200 (12 h in seconds) ✓
- [x] T2-2b Backward-compat: a legacy campaign row with NULL `rollout_start` (or `rollout_window_days=0`) → response token omits the `W` field (or emits `W.0`); device-side firmware must parse both forms (simulate both, assert both accepted) ✓
- [x] T2-2c Malformed `id`: `GET /?id=abc123`, `GET /?id=12345` (5 chars), `GET /?id=` (empty) → `400` ✓
- [x] T2-2d Missing `id`: `GET /` (no query string) → `400` ✓
- [x] T2-3 Slot partition determinism: with `rollout_window_days=10` and 20 test `(region, station)` pairs spread across the full id-space, exactly the pairs whose `zlib.crc32(id) % 20 <= now_slot` receive `W.0`; the rest receive `W.(dev_slot - now_slot) * 43200`. Verified by issuing a second set of test certs and replaying ✓
- [x] T2-4 Chunked download (in-slot): request 512-byte chunks from offset 0 to file_size with `?id=<in-slot>`; reassemble and verify SHA-256 matches metadata `H` ✓
- [x] T2-4a Out-of-slot GET: device whose token had `W>0` calls `GET /get_firmware?offset=0&length=512&id=<out-of-slot>` → `429` with `Retry-After: <seconds>` header (Arch §3.2); no firmware bytes returned ✓
- [x] T2-5 Per-chunk CRC: for each response, `CRC32_MPEG2(body[:-4]) == int.from_bytes(body[-4:], 'little')` ✓
- [x] T2-6 Resumable read: request 5 random non-contiguous offsets (same in-slot `id`); each response length = requested length + 4; reassembled image matches SHA-256 ✓
- [x] T2-7 Boundary: `offset + length > file_size` returns `416` ✓
- [x] T2-8 Length clamp: `length=0` or `length=1024` rejected (or clamped to 512); documented behaviour matches contract ✓
- [x] T2-9 No-campaign chunk request: `GET /get_firmware?offset=0&length=512&id=042001` → `404` ✓
- [x] T2-10 Monotone retry: device with `dev_slot=5` hits endpoint at `now_slot=3` → `W=2*43200`; advance server clock (or wait) to `now_slot=5` → same device now gets `W.0`. Then at `now_slot=6` → still `W.0` (monotone; eligibility never revokes absent pause/cancel) ✓

---

## Phase T3 — Admin Campaign Lifecycle (mirrors Server Phase 7)

Tests in `server_test/tests/test_admin_campaign.py`. 25 tests — all green 2026-05-07.
`AdminClient` helper in `server_test/lib/admin.py`. `campaign_cleanup` fixture in `conftest.py` tracks pre-test max version and removes all created campaigns + files on teardown.

- [x] T3-1 Auth smoke: wrong password → `401`; `viewer` token on `/admin/firmware/upload` → `403` ✓
- [x] T3-2 Firmware upload: response `firmware_sha256` == local SHA-256; `firmware_size` == `len(fw)`; `version` == `prev_max + 1`; campaign row status `draft`; `v{version}.bin` exists on disk with matching SHA-256 ✓
- [x] T3-3 Auto-increment: second distinct upload → `version` = first + 1 ✓
- [x] T3-2/T3-3 Oversize upload (481 KB) → `413` ✓
- [x] T3-4 Start rollout → status `in_progress`, `rollout_start` set, `rollout_window_days` stored ✓
- [x] T3-4a `rollout_window_days=0` → `422`; `rollout_window_days=31` → `422`; omitted → default 10 ✓
- [x] T3-5 Pause → status `paused`; Resume → `in_progress` with `rollout_start` unchanged ✓
- [x] T3-6 Cancel → status `cancelled`; `success_rate` not NULL (0.0 with no downloads); cancel from `draft` allowed ✓
- [x] T3-7 Cohort filter: restricted cohort excludes test device; NULL cohort includes all; empty list normalised to NULL in DB ✓
- [x] T3-7c `rollout_window_days` immutability: re-start in_progress campaign → `409` ✓
- [x] T3-8 SHA-256 tamper: mutate file on disk → start returns 409 ✓
- [x] T3-9 Campaign detail: returns `completed_device_count`, `eligible_device_count`, `current_slot`, `num_slots`; viewer role allowed; 404 for unknown id ✓
- [x] T3-7d Download completions tracking: `ota_download_all()` + assert chunk rows == `(size+511)//512`; idempotent re-download — covered by `test_t2_completions_tracking` in `test_ota_device.py` ✓
- [x] T3-7a Slot schedule determinism: 20 test ids, assert `W` == `zlib.crc32(id) % 20 * 43200` — covered by `test_t2_3_slot_determinism` ✓
- [x] T3-7b Monotone retry across cycles: advance `now_slot`, verify retry succeeds — covered by `test_t2_10_monotone_retry` ✓

---

## Phase T4 — mTLS & Nginx Controls (mirrors Server Phase 4)

**Auth model under test:**
- Device paths (`/api/v1/weather/*`, `/`, `/get_firmware`) require a valid client certificate. Nginx uses `ssl_verify_client optional`; the location block returns `403` if `$ssl_client_verify != SUCCESS`.
- Admin path (`/admin/*`) requires **no** client certificate. Human browsers authenticate via username/password. TLS handshake succeeds without a client cert.

- [x] T4-1 Device path, no client cert: `GET /api/v1/weather/upload` without a client cert → TLS handshake succeeds (server accepts); Nginx location block returns `403` ✓
- [x] T4-2 Device path, wrong CA (self-signed cert): `$ssl_client_verify` = FAILED → Nginx returns `403`; TLS handshake still completes; self-signed cert generated with `cryptography` in-test ✓
- [x] T4-3 **N/A** — requires pre-revoked cert + CRL reload; fleet uses one shared cert with no per-device revocation workflow yet; deferred ✓
- [x] T4-4 Admin path, no client cert: `GET /admin/login` without any client cert → TLS handshake succeeds; FastAPI handles the request (`200` login page or `401` — not `403`); confirms browsers can reach the admin UI ✓
- [x] T4-5 Rate limit: 10 concurrent requests per cycle × 5 cycles (1 s apart) with the same device cert → at least one `503` (Nginx `limit_req` burst exhausted) ✓
- [x] T4-6 **N/A** — requires a second distinct device cert; fleet uses one shared cert per Arch §2.1; per-CN vs per-IP distinction not testable without a second CN; deferred ✓
- [x] T4-7 TLS version: `ssl.TLSVersion.TLSv1_2` → handshake succeeds; `TLSv1_1` max → `ConnectError` (Nginx rejects) ✓
- [x] T4-8 `X-Client-DN` isolation: `GET /admin/` with spoofed `X-SSL-Client-Verify: SUCCESS` + `X-Client-DN: weather-test` (no JWT) → `401`/`302`/`307`; Nginx strips device-cert headers before forwarding to FastAPI on admin path ✓

---

## Phase T5 — Soak, Load & Failure Modes

Runs against a staging copy, never prod. Gated by `pytest -m slow`.
**All tests pass 2026-05-07** (`server_test/tests/test_load.py`; 8/8 in 11:11 against staging). T5-1 run with `SOAK_DURATION_SEC=300`; startup jitter added to avoid thundering herd. T5-2/2a/2b use `asyncio.Semaphore(50)` to stay within server connection limits.

- [x] T5-1 Soak ingest: 100 concurrent mock devices, 1 upload/min, 5 min (300 s) duration → no 5xx, p95 latency < 500 ms ✓
- [x] T5-2 OTA rollout at scale: 1000 mock devices poll with `rollout_window_days=10` → slot-0 cohort count within 29–71 (≈50 ± 3√50); all 1000 see the campaign ✓
- [x] T5-2a Slot advancement: `rollout_start` advanced 1 slot in DB → ~50 more devices become eligible; monotone invariant holds for previously eligible set ✓
- [x] T5-2b Jitter smoothing: CRC32-based delays for in-slot devices spread across 5 equal buckets; no bucket exceeds 3× expected count ✓
- [x] T5-3 Duplicate flood: 50 concurrent identical uploads → exactly 1 `weather_records` row (DB-level idempotency holds under concurrency) ✓
- [x] T5-4 Network chaos: disconnect after 3 chunks; new client resumes from saved offset; full image SHA-256 matches ✓
- [x] T5-5 Power-loss sim: stop after 2 chunks (FRAM offset saved); new client resumes; full image SHA-256 matches ✓
- [x] T5-6 Failure retry at scale: 20 in-slot devices download with 10 % chunk 500-fault injection; all retry to completion; zero admin intervention; all SHA-256 match ✓

---

## Phase T6 — Sensor Data Browse UI

Black-box tests for Phase 11. All tests use the shared admin session cookie obtained via `AdminClient.login()`. Sensor rows are seeded via the ingest endpoint (same mechanism as T1) using a reserved fixture device `(region=998, station=001)` with known fixed-point values.

**Prerequisite:** at least 60 rows must exist for the fixture device before this suite runs (seeded by the `sensor_data_seed` fixture in `conftest.py` — uploads 3 batches of 20 chunks spanning three calendar days with bus_value ranging from −5.0 to +15.0).

- [x] T6-1 Unauthenticated request — `GET /admin/sensor-data` without a session cookie → response is a redirect to `/admin/login.html` (status 303 or 302); no sensor data exposed.
- [x] T6-2 No filters → 200; response HTML contains a `<table>` element; the first page contains exactly `_PAGE_SIZE` (20) rows; each row has 10 `<td>` elements (time, region, station, temp, humidity, pressure, light, rainfall, dew_point, bus); region and station columns show `998` and `1` respectively for all rows on this page.
- [x] T6-3 Filter `region_id=998` → 200; all returned rows have region 998; rows for other regions (if any exist in the DB from other test runs) are absent.
- [x] T6-4 Filter `station_id=1` → 200; all rows belong to station 1; combination with region_id=998 (T6-5 below) is required for strict isolation.
- [x] T6-5 Filter `region_id=998&station_id=1` → 200; rows are strictly the fixture device's rows; no other region/station appears; total count matches the seeded 60 rows across pages.
- [x] T6-6 Filter `date_from=<day2>&date_to=<day2>` (one seeded day only) → 200; all returned rows have `time` values within that calendar day; rows from day1 and day3 are absent; verifies date range is inclusive on both ends (day_to includes the full 24 h of that day).
- [x] T6-7 Filter `bus_min=0.0&bus_max=5.0` → 200; every returned row's BUS value text parses to a float in [0.0, 5.0]; rows with bus_value < 0 or > 5.0 are absent.
- [x] T6-8 Combined filter `region_id=998&station_id=1&date_from=<day2>&date_to=<day2>&bus_min=0.0&bus_max=5.0` → 200; rows satisfy all four constraints simultaneously; count is a subset of T6-6 and T6-7 results.
- [x] T6-9 Filter that matches no rows (`region_id=0&station_id=0`) → 200; HTML contains the empty-state message "No records match the current filters"; no `<tbody><tr>` with data cells present; no 5xx error.
- [x] T6-10 Pagination: no-filter request `page=1` vs `page=2` → the sets of timestamp values in the two pages are disjoint; `page=2` is non-empty (requires ≥ 21 seeded rows, which the seed fixture guarantees).

> **2026-05-07: 10/10 pass** — `pytest server_test/tests/test_sensor_data.py` on robin-gpu.cpe.ku.ac.th (Python 3.13, pytest 9.0.3, 13 s). Fix: `::date` cast added to `_weather_filter()` in `queries.py` for asyncpg type inference on date params.

---

## Mock Device Contract (`lib/mock_device.py`)

Minimal interface a test writes against:

```python
class MockDevice:
    def __init__(self, region: int, station: int, shared_cert: Path, shared_key: Path,
                 base_url: str, ca_bundle: Path): ...
    # device_id property returns f"{region:03d}{station:03d}" (6 chars)
    async def upload(self, samples: list[Sample]) -> dict: ...
    async def ota_poll(self) -> OtaMetadata | None: ...  # OtaMetadata.wait_seconds mirrors `W.`
    async def ota_download_all(self, expected_size: int, expected_sha256: str,
                               apply_jitter: bool = False) -> bytes: ...
    async def close(self) -> None: ...
```

Notes:
- All mock devices share **one** client cert + key (Arch §2.1). Identity is the `?id=<rrrsss>` query param derived from `(region, station)`.
- `OtaMetadata` is a `dataclass` with fields `version: int`, `size: int`, `sha256: str`, `wait_seconds: int` (default 0 when `W.` absent).
- `ota_download_all` skips if `wait_seconds > 0`; tests pin the gate explicitly with `assert meta.wait_seconds == 0` before calling.
- `ota_download_all` issues chunked GETs (default 512 B, with `&id=<rrrsss>`), verifies per-chunk CRC, and raises `AssertionError` on mismatch. Used by every T1–T5 test.
- `apply_jitter=True` sleeps `crc32_mpeg2(id.encode()) % OTA_JITTER_MAX_SEC` seconds before the first chunk (used only by T5-2b).

---

## Execution

| Scope | Command |
|-------|---------|
| Unit parity checks (no network) | `pytest server_test/lib/` |
| Per-phase verification (staging) | `pytest server_test/tests/test_ingest.py` (or other file) |
| Fast subset on every push | `pytest server_test/ -m "not slow"` |
| Full soak & load | `pytest server_test/ -m slow` (manual, scheduled nightly) |
| Teardown | `python server_test/scripts/cleanup_test_rows.py` |

---

## Open Questions

1. **DB read access for assertions.** Prefer a read-only test DB role (`akp` in development, `iotsrv_test` in production) for fast, deterministic assertions vs. going exclusively through admin HTTP endpoints (true black-box). Plan assumes both are available; admin endpoints are preferred where they exist.
2. **Staging vs. prod isolation.** Dedicated staging DB + separate firmware dir is ideal. If only one host is available, reserve `(region_id, station_id)` values in the `TEST-*` range (e.g. region 999) and `version >= 900000000`; add a `FORCE_PROD=1` guard to prevent accidental prod runs. No CN prefix needed — no per-device CN exists.
3. **Test cert lifecycle.** The fleet-wide shared client cert (Arch §2.1) is the single source of TLS identity. Per-device test CNs are no longer required; `scripts/provision_test_certs.sh` shrinks to "issue one shared test cert" unless mTLS revocation tests (T4-3) still need unique certs. Decide before T0-6 lands.
4. **Clock advancement for slot tests.** T2-10 / T3-7b / T5-2a all rely on advancing `now_slot`. Prefer a server test hook (e.g. `X-Fake-Now` header honoured only in staging) over real `sleep(12h)`; without the hook those tests run only nightly. Revisit after S5-2a lands.
5. **Slot hash parity.** The server uses Python's `zlib.crc32` (IEEE 802.3 poly, reflected); the firmware's `shared/crc32.c` is CRC-32/MPEG-2 (non-reflected). These are **different** algorithms — T3-7a must assert slot assignment with the server's variant (`zlib.crc32`), not the MPEG-2 variant used for chunk CRCs. The device does not hash its own id for eligibility; it reads `W` from the token.

---

## References

- **Implementation plan:** `Server_Implementation_Plan.md`
- **Architecture spec:** `Server_Architecture.md`
- **Binary schema / fixed-point:** `lib/utils/weather_data.h`, `lib/utils/fixedptc.h`
- **CRC-32 firmware implementation:** `shared/crc32.c`
- **Device firmware OTA state machine:** `Src/ota_manager_task.c`
