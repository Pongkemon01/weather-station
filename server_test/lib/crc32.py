"""CRC-32/MPEG-2 (mirrors shared/crc32.c).

Poly 0x04C11DB7, init 0xFFFFFFFF, refin=False, refout=False, xorout=0x00000000.
NOT the same as Python's zlib.crc32 (IEEE 802.3, reflected).
Used for per-chunk CRC appended by the /get_firmware handler.
"""
from __future__ import annotations

_POLY = 0x04C11DB7
_TABLE: list[int] = []


def _build() -> None:
    for i in range(256):
        crc = i << 24
        for _ in range(8):
            crc = ((crc << 1) ^ _POLY) if (crc & 0x80000000) else (crc << 1)
        _TABLE.append(crc & 0xFFFFFFFF)


_build()


def crc32_mpeg2(data: bytes, init: int = 0xFFFFFFFF) -> int:
    """Compute CRC-32/MPEG-2 over data."""
    crc = init
    for b in data:
        crc = ((crc << 8) ^ _TABLE[((crc >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return crc


# ── Parity vectors cross-checked against shared/crc32.c ──────────────────────
_PARITY = [
    (b"",           0xFFFFFFFF),   # empty — init value returned unchanged
    (b"123456789",  0x0376E6E7),   # standard CRC-32/MPEG-2 check value
]


def _selfcheck() -> None:
    for data, expected in _PARITY:
        got = crc32_mpeg2(data)
        assert got == expected, f"crc32_mpeg2({data!r}) = {got:#010x} != {expected:#010x}"


_selfcheck()
