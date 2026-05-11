# Server Verification Plan

> Python black-box verifiers exercising the deployed server from a dev machine.
> Source tree: `server_test/`. Target: `https://robin-gpu.cpe.ku.ac.th/` (or `STAGING_URL` override).
> Companion to `Server_Implementation_Plan.md`. Status: all suites green (see `IMPLEMENTATION_STATUS.md`).
>
> **Scope:** wire contract regressions. Server internals tested by `html/tests/` (pytest).

---

## Design Principles

- **Mock device = real TLS client.** `httpx` + fleet client cert/key for mTLS, rate-limit, slot tests.
- **No server imports.** Tests never `from app import …`. Target = HTTP contract.
- **Bit-pattern fixtures.** Binary payloads constructed via `struct.pack` matching `lib/utils/weather_data.h`. CRC fixtures cross-checked against `shared/crc32.c`.
- **Reserved test space.** Region 999 + version ≥ 900,000,000. `db_cleanup` purges pre/post.
- **Configurable target.** `.env` (gitignored): `STAGING_URL`, `ADMIN_USER`, `ADMIN_PASS`, `CA_BUNDLE`, `DEVICE_CERT_DIR`.

---

## Directory Layout (`server_test/`)

```
server_test/
├── conftest.py                ; fixtures: base_url, dev / dev_mtls / dev_no_cert clients, db, db_cleanup, admin_session, campaign_cleanup, sensor_data_seed
├── pyproject.toml             ; ruff + pytest config (markers: slow)
├── requirements.txt           ; httpx[http2], pytest, pytest-asyncio, cryptography, python-dotenv
├── .env.example
├── lib/
│   ├── packed.py              ; encode/decode Weather_Data_Packed_t (parity-tested vs firmware)
│   ├── fixedpt.py             ; S9.7 codec (parity-tested)
│   ├── crc32.py               ; CRC-32/MPEG-2 (parity-tested; check `"123456789"` → 0x0376E6E7)
│   ├── mock_device.py         ; MockDevice: upload(), ota_poll(), ota_download_all()
│   ├── admin.py               ; AdminClient: login, upload_firmware, start/pause/cancel
│   └── test_parity.py         ; CRC + fixedpt + packed self-checks (no network)
├── tests/
│   ├── test_ingest.py         ; T1 (Phase 3)
│   ├── test_ota_device.py     ; T2 (Phase 5)
│   ├── test_admin_auth.py     ; 7 admin auth/RBAC integration tests (S6)
│   ├── test_admin_campaign.py ; T3 (Phase 7) — 25 tests
│   ├── test_admin_ui.py       ; 14 admin UI E2E tests (S8)
│   ├── test_mtls.py           ; T4 (Phase 4)
│   ├── test_load.py           ; T5 — soak/load (pytest -m slow)
│   ├── test_sensor_data.py    ; T6 (Phase 11) — 10 tests
│   └── test_user_management.py; TUM0–TUM11 — 75 tests (see User_Management_Test_Plan.md)
├── fixtures/                  ; firmware_small.bin, firmware_large.bin, packed_samples.json
└── scripts/
    ├── provision_test_certs.sh
    └── cleanup_test_rows.py
```

---

## Suite Summary

All suites currently green on `robin-gpu.cpe.ku.ac.th`.

| Suite | Mirrors | Tests | Notes |
|-------|---------|-------|-------|
| **T0** Harness bootstrap | — | 5 | Parity self-checks: CRC, fixedpt (6 vectors + round-trip), packed struct (lengths, header, temp encoding). Fleet shares one cert (no per-device CN). |
| **T1** Ingest | S3 | 9 | Happy / max-batch / idempotency / boundary values (`-256.0` to `+255.992`) / count mismatch 400 / first-seen device upsert / idempotency key format (`{rrr}{sss}:{iso}`) / no-cert 403 / Y2K epoch 0 stored as `2000-01-01T00:00:00+00:00`. |
| **T2** OTA download | S5 | 18 | No-campaign HTML body / metadata regex match (with + without `W.`) / malformed-id 400 / missing-id 400 / slot determinism over 20 ids / chunked download SHA-256 reassembly / out-of-slot 429 + `Retry-After` / per-chunk CRC validation / resumable random offsets / `offset+length > size` 416 / `length=0` or `1024` clamped or rejected / no-campaign chunk 404 / monotone retry across cycles / `download_completions` chunk rows == ceil(size/512), idempotent re-download. |
| **T3** Admin campaign | S7 | 25 | Auth/RBAC; firmware upload response + DB + disk; auto version increment; oversize 413 (491,521 B = 480 KB + 1); start sets `rollout_start`; `rollout_window_days` Pydantic ge=1 le=30 (422); pause/resume preserves `rollout_start`; cancel writes `success_rate`; cohort filter (NULL = all, empty → NULL, restricted excludes); re-start 409; SHA-256 disk-tamper 409; detail aggregates. |
| **S6** Admin auth | S6 | 7 unit + 7 integration | Wrong password 401; role-restricted endpoints; `require_role` factory levels. |
| **T4** mTLS + Nginx | S4 | 6 active (T4-3, T4-6 N/A) | No-cert 403 device path (Nginx 400 also valid — `ssl_verify_client on` blocks at SSL layer); wrong-CA 403 (self-signed via `cryptography`); admin path no-cert → 200/401 (not 403); rate limit (10 conc × 5 cycles → ≥ 1 × 503); TLS 1.2 accept / 1.1 reject; admin vhost header isolation (spoofed `X-SSL-Client-Verify` + `X-Client-DN` ignored). T4-3 (revoked-cert + CRL) deferred — fleet has one shared cert; T4-6 (per-CN throttle) deferred — no second CN exists. |
| **S8** Admin UI | S8 | 14 E2E | Login flow, dashboard pagination, campaign upload/start/pause/cancel via HTMX. Two bugs found + fixed during testing: `POST /admin/logout` route shadowed by admin router (renamed to `/logout-ui`); `delete_cookie` missing `httponly + samesite` to match `set_cookie`. |
| **T5** Soak + load | (post-rollout) | 8 (pytest -m slow) | Soak 100 dev × 1 upload/min × 300 s — no 5xx, p95 < 500 ms. OTA at scale (1000 mock devices, slot-0 cohort 29–71, all see campaign). Slot advancement (~50 more devices become eligible per slot). Jitter smoothing across 5 buckets. Duplicate flood (50 conc → 1 row). Network chaos (resume after disconnect). Power-loss sim (resume from saved offset). 10 % chunk fault injection (all retry to completion). Notes: `SOAK_DURATION_SEC=300`, startup jitter, `Semaphore(50)`. |
| **T6** Sensor data UI | S11 | 10 | Unauth redirect; no-filter default; per-filter (region / station / date-range / BUS); combined; empty-set message; pagination disjoint sets. Prereq fixture seeds 60 rows across 3 days. Fix found: `::date` cast in `_weather_filter` for asyncpg type inference. |
| **TUM0–TUM11** User management | UM1–UM5 | 75 | See `User_Management_Test_Plan.md`. |

---

## Mock Device Contract (`lib/mock_device.py`)

```python
class MockDevice:
    def __init__(self, region: int, station: int, shared_cert: Path, shared_key: Path,
                 base_url: str, ca_bundle: Path): ...
    # device_id = f"{region:03d}{station:03d}"
    async def upload(self, samples: list[Sample]) -> dict: ...
    async def ota_poll(self) -> OtaMetadata | None: ...
    async def ota_download_all(self, expected_size: int, expected_sha256: str,
                               apply_jitter: bool = False) -> bytes: ...
    async def close(self) -> None: ...

@dataclass
class OtaMetadata:
    version: int
    size: int
    sha256: str
    wait_seconds: int = 0   # mirrors W. token (0 if absent)
```

- All mocks share one client cert (Arch §2.1); identity = `?id=` query param.
- `ota_download_all` skips when `wait_seconds > 0`; tests assert `wait_seconds == 0` before calling.
- Per-chunk CRC verified; `AssertionError` on mismatch.
- `apply_jitter=True` sleeps `crc32_mpeg2(id) % OTA_JITTER_MAX_SEC` before first chunk (T5-2b only).

---

## Execution

| Scope | Command |
|-------|---------|
| Parity (no network)                | `pytest server_test/lib/` |
| Per-suite (live server)            | `pytest server_test/tests/test_<name>.py` |
| Fast subset on every push          | `pytest server_test/ -m "not slow"` |
| Full soak + load (nightly)         | `pytest server_test/ -m slow` |
| Teardown                           | `python server_test/scripts/cleanup_test_rows.py` |

---

## Notes & Open Items

- **DB read access for assertions.** Prefer a read-only test role for fast deterministic assertions; admin HTTP endpoints used otherwise.
- **Staging vs prod.** Reserve `region=999` and `version >= 900_000_000`. Add `FORCE_PROD=1` guard before any test that mutates campaigns.
- **Clock advancement for slot tests** (T2-10 / T3-7b / T5-2a). Currently runs nightly; a staging-only `X-Fake-Now` header would let these run on every push.
- **Slot hash parity.** Server uses `zlib.crc32` (IEEE 802.3, reflected). Firmware uses CRC-32/MPEG-2 (non-reflected). **Different algorithms.** T3-7a asserts slot assignment against `zlib.crc32` only. Devices never hash their own id — they read `W` from the token.
- **Test cert lifecycle.** Single fleet test cert issued via `scripts/issue_device_cert.sh weather-test` is sufficient (Arch §2.1). T4-3 / T4-6 would need additional certs; deferred.

---

## References

- `Server_Implementation_Plan.md`
- `Server_Architecture.md`
- `lib/utils/weather_data.h` — binary schema
- `shared/crc32.c` — firmware CRC implementation
- `Src/ota_manager_task.c` — device OTA state machine
