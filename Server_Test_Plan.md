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
│   ├── test_admin_campaign.py ← T3-series (Phase 7)
│   ├── test_mtls.py           ← T4-series (Phase 4)
│   └── test_load.py           ← T5-series (soak + rate-limit)
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

- [ ] T0-1 Scaffold `server_test/` per layout; add `requirements.txt`; `pip install -r requirements.txt` succeeds on Python 3.12
- [ ] T0-2 `conftest.py`: load `.env`; expose `base_url`, `ca_bundle`, `device_cert_dir` fixtures; skip suite with clear message if env missing
- [ ] T0-3 `lib/crc32.py`: implement CRC-32/MPEG-2; add test that reproduces a known digest from `shared/crc32.c` (embed 4–6 pre-computed vectors)
- [ ] T0-4 `lib/fixedpt.py`: `to_fixed(value: float) -> int` and `from_fixed(raw: int) -> float` for S9.7; property test: round-trip within ±1 LSB across [-256, 255.99]
- [ ] T0-5 `lib/packed.py`: `encode_chunks(region, station, samples)` and `decode_chunks(payload)` using the firmware's little-endian packed layout
- [ ] T0-6 `scripts/provision_test_certs.sh`: produces ≥ 3 device cert bundles (`TEST-0001..0003`) signed by the server's intermediate CA
- [ ] T0-7 Verification: `pytest server_test/lib/` runs the parity tests (CRC, fixedpt, packed) green ✓

---

## Phase T1 — Ingest Path (mirrors Server Phase 3)

- [ ] T1-1 Happy path: single chunk with known values → `200 {"status":"ok"}`; poll admin `/admin/devices/<region_id>/<station_id>/records?limit=1` (or query DB directly) and assert float fields match to ±0.01
- [ ] T1-2 Max batch: 28 chunks × 18 B + 5 B header = 509 B → `200`; all 28 rows appear in `weather_records` with correct timestamps
- [ ] T1-3 Idempotency: same payload POSTed twice → second response is `{"status":"duplicate"}`; `weather_records` row count unchanged
- [ ] T1-4 Field boundaries: fixed-point extremes (`-256.0`, `+255.9921875`) round-trip correctly after decode
- [ ] T1-5 Malformed header: `chunk_count=5` but body length = 5 + 4·18 → `400`; no rows inserted
- [ ] T1-6 First-seen device auto-upsert: upload with a `(region, station)` pair that has no existing `devices` row → `200`; a new row appears with `last_seen=now()`; the 5-byte header is the sole identity source (Arch §2.1 shared-cert note — do **not** gate on CN)
- [ ] T1-7 Idempotency key format: assert the `ingest_log.key` column stores `"{region:03d}{station:03d}:{first_sample_ts}"` (e.g. `042001:1713456000`) — pin the exact format so future refactors don't silently change it
- [ ] T1-8 Missing `X-SSL-Client-Verify` (simulated by hitting FastAPI directly if allowed during staging) → `403`
- [ ] T1-9 Timestamp edge case: Y2K epoch 0 (2000-01-01T00:00:00Z) stored as `'2000-01-01 00:00:00+00'` TIMESTAMPTZ

---

## Phase T2 — OTA Download (mirrors Server Phase 5)

All device requests pass `?id=<rrrsss>` (6-char decimal, `%03d%03d` of region/station). Slot schedule defined in Arch §3.3; metadata token regex: `V\.\d+:L\.\d+:H\.[0-9a-f]{64}(?::W\.\d+)?`.

- [ ] T2-1 No active campaign: `GET /?id=042001` returns HTML body without a metadata token match
- [ ] T2-2 Active campaign, device in current slot: metadata token matches regex; `V`, `L`, `H` parse to known values from `fixtures/firmware_small.bin`; `W` field either absent or `W.0`
- [ ] T2-2a `W` field parsing: with `rollout_start` fresh and `rollout_window_days=10`, pick a device whose `crc32(id) % 20 > 0` → response contains `W.<positive>`; assert `W` is a multiple of 43200 (12 h in seconds)
- [ ] T2-2b Backward-compat: a legacy campaign row with NULL `rollout_start` (or `rollout_window_days=0`) → response token omits the `W` field (or emits `W.0`); device-side firmware must parse both forms (simulate both, assert both accepted)
- [ ] T2-2c Malformed `id`: `GET /?id=abc123`, `GET /?id=12345` (5 chars), `GET /?id=` (empty) → `400`
- [ ] T2-2d Missing `id`: `GET /` (no query string) → `400`
- [ ] T2-3 Slot partition determinism: with `rollout_window_days=10` and 20 test `(region, station)` pairs spread across the full id-space, exactly the pairs whose `zlib.crc32(id) % 20 <= now_slot` receive `W.0`; the rest receive `W.(dev_slot - now_slot) * 43200`. Verified by issuing a second set of test certs and replaying
- [ ] T2-4 Chunked download (in-slot): request 512-byte chunks from offset 0 to file_size with `?id=<in-slot>`; reassemble and verify SHA-256 matches metadata `H`
- [ ] T2-4a Out-of-slot GET: device whose token had `W>0` calls `GET /get_firmware?offset=0&length=512&id=<out-of-slot>` → `429` with `Retry-After: <seconds>` header (Arch §3.2); no firmware bytes returned
- [ ] T2-5 Per-chunk CRC: for each response, `CRC32_MPEG2(body[:-4]) == int.from_bytes(body[-4:], 'little')`
- [ ] T2-6 Resumable read: request 5 random non-contiguous offsets (same in-slot `id`); each response length = requested length + 4; reassembled image matches SHA-256
- [ ] T2-7 Boundary: `offset + length > file_size` returns `416`
- [ ] T2-8 Length clamp: `length=0` or `length=1024` rejected (or clamped to 512); documented behaviour matches contract
- [ ] T2-9 No-campaign chunk request: `GET /get_firmware?offset=0&length=512&id=042001` → `404`
- [ ] T2-10 Monotone retry: device with `dev_slot=5` hits endpoint at `now_slot=3` → `W=2*43200`; advance server clock (or wait) to `now_slot=5` → same device now gets `W.0`. Then at `now_slot=6` → still `W.0` (monotone; eligibility never revokes absent pause/cancel)

---

## Phase T3 — Admin Campaign Lifecycle (mirrors Server Phase 7)

- [ ] T3-1 Login: wrong password → `401`; correct → `200` + JWT; expired token → `401`; RBAC — `viewer` token on admin endpoint → `403`
- [ ] T3-2 Firmware upload: POST `firmware_small.bin` (no version field) → response `firmware_sha256` equals local `hashlib.sha256(fw).hexdigest()`; response `firmware_size` equals `len(fw)`; response `version` equals `prev_max + 1`; campaign row present with status `draft`; `html/firmware/v{version}.bin` exists on server and its SHA-256 matches; no previous firmware binary remains on disk
- [ ] T3-3 Auto-increment: upload a second distinct binary → response `version` = first response `version` + 1; first `html/firmware/v{v1}.bin` has been deleted; only `html/firmware/v{v2}.bin` present on disk
- [ ] T3-4 Start rollout: `POST /admin/campaign/{id}/start` with JSON body `{"rollout_window_days": 10}` → status flips to `in_progress`; `rollout_start=now()`; T2-2 for any in-slot `id` now succeeds. Attempt to pass legacy `cohort_size_percent` → `400` (field removed per Arch §3.3)
- [ ] T3-4a Start invariants: `rollout_window_days=0` → `400`; `rollout_window_days=31` → `400`; default (omitted) → `10`
- [ ] T3-5 Pause → Resume: metadata endpoint returns "No update available" while paused; on resume, `rollout_start` is **not** reset — `W` values continue from the original slot clock (assert `now_slot` after resume > `now_slot` at pause)
- [ ] T3-6 Cancel: `POST .../cancel` → status `cancelled` (Q-S8; `rolled_back` name is retired); all devices (including previously eligible) see "No update available"; `ota_campaigns.success_rate` is set (not NULL) after the transition; firmware binary retention sweep runs (`_sweep_firmware_retention`)
- [ ] T3-7 `target_cohort_ids` filter: start campaign with explicit list `["042001","042002"]` → only those two ids get tokens even if slot schedule says otherwise; `["042003"]` always gets "No update available"
- [ ] T3-7a Slot schedule determinism: for fixed `rollout_window_days=10` and a fixed set of 20 test ids, record the `W` value for each id at `t=rollout_start`. Recompute `zlib.crc32(id.encode("ascii")) % 20 * 43200` locally and assert equality for every id (proves server uses the same CRC32 variant as firmware side)
- [ ] T3-7b Monotone retry across cycles: device with `dev_slot=5` at `now_slot=3` fails download (induced SHA mismatch). Advance time to `now_slot=5`; re-run `ota_poll()` + `ota_download_all()` → succeeds. No admin intervention required (Arch §3.3 failure-retry semantics)
- [ ] T3-7c `rollout_window_days` immutability: attempt `POST /admin/campaign/{id}/start` again (or PATCH the field) after status=`in_progress` → `409` or `400`. Reshuffling `dev_slot` mid-rollout would break monotone retry (see Impl Plan S7-5)
- [ ] T3-7d Download completions tracking: after `ota_download_all()` completes for a test device, query `download_completions` where `campaign_id = {id}` and `device_id = {test_id}`; assert row count equals `(firmware_size + 511) // 512` (all chunks recorded, ceiling division per Q-S6). Re-run the same download — row count must not increase (`ON CONFLICT DO NOTHING` is idempotent). Verify `GET /admin/campaign/{id}` aggregate shows exactly 1 completed device (Impl Plan S7-4)
- [ ] T3-8 File integrity: mutate the on-disk firmware file (staging only); next `/admin/campaign/{id}/start` call rejects with SHA-256 mismatch
- [ ] T3-9 Teardown: `scripts/cleanup_test_rows.py` removes the campaign and the firmware file

---

## Phase T4 — mTLS & Nginx Controls (mirrors Server Phase 4)

**Auth model under test:**
- Device paths (`/api/v1/weather/*`, `/`, `/get_firmware`) require a valid client certificate. Nginx uses `ssl_verify_client optional`; the location block returns `403` if `$ssl_client_verify != SUCCESS`.
- Admin path (`/admin/*`) requires **no** client certificate. Human browsers authenticate via username/password. TLS handshake succeeds without a client cert.

- [ ] T4-1 Device path, no client cert: `GET /api/v1/weather/upload` without a client cert → TLS handshake succeeds (server accepts); Nginx location block returns `403`
- [ ] T4-2 Device path, wrong CA (self-signed cert): `$ssl_client_verify` = FAILED → Nginx returns `403`; TLS handshake still completes
- [ ] T4-3 Revoked cert: revoke a test cert, refresh CRL, wait for Nginx reload → device path requests return `403`
- [ ] T4-4 Admin path, no client cert: `GET /admin/login` without any client cert → TLS handshake succeeds; FastAPI handles the request (`200` login page or `401` — not `403`); confirms browsers can reach the admin UI
- [ ] T4-5 Rate limit: issue 10 requests/s with the same device cert for 5 s → at least some responses are `503` (Nginx `limit_req` burst exhausted)
- [ ] T4-6 Different certs, shared IP: two test devices each at 1 req/s → both succeed (limit is per-CN, not per-IP)
- [ ] T4-7 TLS version: force `--tlsv1.2` on client → handshake succeeds; `--tlsv1.1` → fails
- [ ] T4-8 `X-Client-DN` isolation: proxy request to `/admin/` and verify the forwarded request headers do **not** contain `X-Client-DN` (prevents cert spoofing via admin path)

---

## Phase T5 — Soak, Load & Failure Modes

Runs against a staging copy, never prod. Gated by `pytest -m slow`.

- [ ] T5-1 Soak ingest: 100 concurrent mock devices, 1 upload/min, 1 h duration → no 5xx, p95 latency < 500 ms, `ingest_lag` Prometheus gauge stays < 10 s
- [ ] T5-2 OTA rollout at scale: 1000 mock devices (unique `(region, station)` pairs) poll simultaneously with `rollout_window_days=10` → exactly `1000 / 20 ≈ 50 ± √50` receive `W.0` in slot 0; the remaining ~950 receive positive `W`. Only the in-slot subset initiates chunked download; peak concurrent `/get_firmware` connections observed at the server stays ≤ 75 (Arch §4 egress target ≤ 90 KB/s)
- [ ] T5-2a Slot advancement: advance server clock by 12 h → next cohort (~50 more) becomes in-slot; previously-completed cohort stays idle (no re-download). Monotone eligibility holds
- [ ] T5-2b Jitter smoothing: in-slot devices apply CRC32-based first-chunk delay (0–`OTA_JITTER_MAX_SEC`, default 1800 s); measured time-to-first-chunk for the 50 in-slot devices should be roughly uniform across [0, 30 min], not bunched at t=0
- [ ] T5-3 Duplicate flood: 1 device retries same payload 50 × in 10 s → exactly one `weather_records` row exists
- [ ] T5-4 Network chaos: mid-download client disconnect → subsequent resume with correct offset completes image; SHA-256 matches
- [ ] T5-5 Power-loss sim: kill mock-device process after N chunks; restart with FRAM-equivalent offset tracking → finishes the image
- [ ] T5-6 Failure retry at scale: inject a server-side fault on cycle N (return 500 on `/get_firmware` for 10% of requests) → those devices fail; cycle N+1 (after 12 h or forced clock advance) → failed devices automatically retry and complete. Zero admin intervention required (Arch §3.3)

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
