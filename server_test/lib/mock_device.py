"""MockDevice: simulates the A7670E firmware upload + OTA poll/download protocol."""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field
from pathlib import Path

import httpx

from .crc32 import crc32_mpeg2
from .packed import Sample, encode

_META_RE = re.compile(
    r"V\.(\d+):L\.(\d+):H\.([0-9a-f]{64})(?::W\.(\d+))?"
)


@dataclass
class OtaMetadata:
    version: int
    size: int
    sha256: str
    wait_seconds: int = 0


class MockDevice:
    """Simulates one weather station.

    Two usage modes:
      mTLS mode  — cert + key + CA bundle; real Nginx path.
      Direct mode — no cert; inject X-SSL-Client-Verify header; internal URL.
    """

    def __init__(
        self,
        region: int,
        station: int,
        base_url: str,
        *,
        cert: tuple[Path, Path] | None = None,
        ca_bundle: Path | bool = True,
        inject_verify_header: bool = False,
    ) -> None:
        self.region = region
        self.station = station
        self._base = base_url.rstrip("/")

        headers = {}
        if inject_verify_header:
            headers["X-SSL-Client-Verify"] = "SUCCESS"

        httpx_cert = (str(cert[0]), str(cert[1])) if cert else None
        httpx_verify: str | bool = str(ca_bundle) if isinstance(ca_bundle, Path) else ca_bundle

        self._client = httpx.AsyncClient(
            cert=httpx_cert,
            verify=httpx_verify,
            headers=headers,
            timeout=30,
            http2=True,
        )

    @property
    def device_id(self) -> str:
        return f"{self.region:03d}{self.station:03d}"

    async def upload(self, samples: list[Sample]) -> dict:
        payload = encode(self.region, self.station, samples)
        r = await self._client.post(
            f"{self._base}/api/v1/weather/upload",
            content=payload,
            headers={"Content-Type": "application/octet-stream"},
        )
        r.raise_for_status()
        return r.json()

    async def upload_raw(self, payload: bytes) -> httpx.Response:
        """POST raw bytes without raising on error status."""
        return await self._client.post(
            f"{self._base}/api/v1/weather/upload",
            content=payload,
            headers={"Content-Type": "application/octet-stream"},
        )

    async def ota_poll(self) -> OtaMetadata | None:
        r = await self._client.get(
            f"{self._base}/api/v1/weather/",
            params={"id": self.device_id},
        )
        r.raise_for_status()
        m = _META_RE.search(r.text)
        if not m:
            return None
        return OtaMetadata(
            version=int(m.group(1)),
            size=int(m.group(2)),
            sha256=m.group(3),
            wait_seconds=int(m.group(4)) if m.group(4) is not None else 0,
        )

    async def ota_download_all(
        self,
        expected_size: int,
        expected_sha256: str,
        chunk_size: int = 512,
    ) -> bytes:
        buf = b""
        offset = 0
        while offset < expected_size:
            length = min(chunk_size, expected_size - offset)
            r = await self._client.get(
                f"{self._base}/api/v1/weather/get_firmware",
                params={"offset": offset, "length": length, "id": self.device_id},
            )
            r.raise_for_status()
            body = r.content
            assert len(body) >= 4, f"chunk too short at offset {offset}"
            chunk_data, crc_bytes = body[:-4], body[-4:]
            chunk_crc = int.from_bytes(crc_bytes, "little")
            assert crc32_mpeg2(chunk_data) == chunk_crc, (
                f"chunk CRC mismatch at offset {offset}"
            )
            buf += chunk_data
            offset += len(chunk_data)
        actual = hashlib.sha256(buf).hexdigest()
        assert actual == expected_sha256, (
            f"image SHA-256 mismatch: got {actual}"
        )
        return buf

    async def close(self) -> None:
        await self._client.aclose()

    async def __aenter__(self) -> "MockDevice":
        return self

    async def __aexit__(self, *_) -> None:
        await self.close()
