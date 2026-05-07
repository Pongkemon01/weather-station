"""Phase 4 mTLS & Nginx controls tests (T4-series).

All tests hit the live Nginx at BASE_URL (not INTERNAL_URL).
Requires in .env: BASE_URL, CA_BUNDLE, DEVICE_CERT, DEVICE_KEY.

T4-3 (revoked cert) and T4-6 (per-CN rate limit) are N/A — see test plan.
"""
from __future__ import annotations

import asyncio
import ssl

import httpx
import pytest


# ── T4-1: device path, no client cert → Nginx location block returns 403 ─────

async def test_t4_1_device_path_no_cert_returns_403(base_url, ca_bundle):
    async with httpx.AsyncClient(verify=ca_bundle, timeout=15) as client:
        r = await client.get(f"{base_url}/api/v1/weather/upload")
    assert r.status_code == 403


# ── T4-2: device path, self-signed (wrong-CA) cert → ssl_client_verify=FAILED → 403

async def test_t4_2_device_path_wrong_ca_returns_403(base_url, ca_bundle, tmp_path):
    import datetime

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(x509.NameOID.COMMON_NAME, "fake-device")])
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.utcnow())
        .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(hours=1))
        .sign(key, hashes.SHA256())
    )
    cert_path = tmp_path / "fake.crt"
    key_path  = tmp_path / "fake.key"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )

    async with httpx.AsyncClient(
        cert=(str(cert_path), str(key_path)), verify=ca_bundle, timeout=15
    ) as client:
        r = await client.get(f"{base_url}/api/v1/weather/upload")
    assert r.status_code == 403


# ── T4-4: admin path, no client cert → TLS succeeds; FastAPI handles (not 403) ─

async def test_t4_4_admin_path_no_cert_not_403(base_url, ca_bundle):
    async with httpx.AsyncClient(verify=ca_bundle, timeout=15) as client:
        r = await client.get(f"{base_url}/admin/login")
    assert r.status_code != 403, (
        f"/admin/login returned 403 — cert verification must not block admin path"
    )
    assert r.status_code in (200, 401, 302, 307)


# ── T4-5: rate limit — 10 concurrent req/s × 5 cycles → at least some 503 ────

async def test_t4_5_rate_limit_triggers_503(base_url, ca_bundle, device_cert):
    async def _hit(client: httpx.AsyncClient) -> int:
        try:
            r = await client.get(
                f"{base_url}/api/v1/weather/upload", params={"id": "999001"}
            )
            return r.status_code
        except httpx.ReadTimeout:
            return -1

    statuses: list[int] = []
    async with httpx.AsyncClient(
        cert=device_cert, verify=ca_bundle, timeout=10, http2=True
    ) as client:
        for _ in range(5):
            batch = await asyncio.gather(*[_hit(client) for _ in range(10)])
            statuses.extend(batch)
            await asyncio.sleep(1.0)

    assert 503 in statuses, (
        f"Expected ≥1 rate-limit 503 in {len(statuses)} requests; got: {set(statuses)}"
    )


# ── T4-7: TLS version enforcement ─────────────────────────────────────────────

async def test_t4_7_tls12_accepted(base_url, ca_bundle, device_cert):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_verify_locations(ca_bundle)
    ctx.load_cert_chain(device_cert[0], device_cert[1])
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2

    async with httpx.AsyncClient(verify=ctx, timeout=15) as client:
        r = await client.get(f"{base_url}/admin/login")
    assert r.status_code in (200, 401, 302, 307)


async def test_t4_7_tls11_rejected(base_url, ca_bundle, device_cert):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_verify_locations(ca_bundle)
    ctx.load_cert_chain(device_cert[0], device_cert[1])
    try:
        ctx.maximum_version = ssl.TLSVersion.TLSv1_1
    except AttributeError:
        pytest.skip("ssl.TLSVersion.TLSv1_1 not available in this Python build")

    with pytest.raises((httpx.ConnectError, httpx.RemoteProtocolError, OSError)):
        async with httpx.AsyncClient(verify=ctx, timeout=15) as client:
            await client.get(f"{base_url}/admin/login")


# ── T4-8: X-Client-DN / X-SSL-Client-Verify isolation on admin path ──────────

async def test_t4_8_spoofed_cert_header_on_admin_path_has_no_effect(base_url, ca_bundle):
    """Nginx must strip X-SSL-Client-Verify on /admin/* so injected headers
    cannot bypass admin authentication.  Without a valid JWT the response
    must be 401/302/307, not 200.
    """
    async with httpx.AsyncClient(verify=ca_bundle, timeout=15) as client:
        r = await client.get(
            f"{base_url}/admin/",
            headers={
                "X-SSL-Client-Verify": "SUCCESS",
                "X-Client-DN": "weather-test",
            },
        )
    assert r.status_code in (401, 302, 307), (
        f"Admin path returned {r.status_code} with spoofed cert header — "
        "Nginx may not be stripping X-SSL-Client-Verify / X-Client-DN"
    )
