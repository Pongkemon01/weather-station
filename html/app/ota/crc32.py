"""CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, MSB-first, no reflection, no final XOR.

Matches shared/crc32.c exactly. Known check value for b"123456789": 0x0376E6E7.
"""
from __future__ import annotations


def _build_table() -> list[int]:
    poly = 0x04C11DB7
    table = []
    for i in range(256):
        crc = i << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ poly) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
        table.append(crc)
    return table


_TABLE: list[int] = _build_table()


def crc32_mpeg2(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """Return CRC-32/MPEG-2 of data, optionally continuing from a prior crc."""
    for byte in data:
        crc = ((crc << 8) ^ _TABLE[((crc >> 24) ^ byte) & 0xFF]) & 0xFFFFFFFF
    return crc
