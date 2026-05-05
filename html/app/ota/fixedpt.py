"""S9.7 signed 16-bit fixed-point ↔ float conversion.

Mirrors lib/utils/fixedptc.h with FIXEDPT_BITS=16, FIXEDPT_WBITS=9.
Fractional bits: 16 - 9 = 7. Scale factor: 2^7 = 128.
"""
from __future__ import annotations

_SCALE = 128  # 2 ** 7


def from_fixed(raw: int) -> float:
    """Convert S9.7 int16 fixed-point to float."""
    raw = raw & 0xFFFF
    if raw & 0x8000:
        raw -= 0x10000
    return raw / _SCALE


def to_fixed(value: float) -> int:
    """Convert float to S9.7 int16 fixed-point (for test fixtures)."""
    raw = round(value * _SCALE)
    return max(-32768, min(32767, raw))
