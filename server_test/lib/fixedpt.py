"""S9.7 fixed-point ↔ float (mirrors lib/utils/fixedptc.h).

16-bit signed integer, 7 fractional bits.
Resolution: 1/128 ≈ 0.0078125.  Range: [-256, 255.9921875].
"""
from __future__ import annotations

import struct

_PACK_S = struct.Struct("<h")   # signed 16-bit little-endian
_PACK_U = struct.Struct("<H")   # unsigned 16-bit little-endian
_LSB = 1 / 128


def to_fixed(v: float) -> int:
    """Float → signed 16-bit S9.7 (clamped)."""
    raw = round(v * 128)
    raw = max(-32768, min(32767, raw))
    return raw


def from_fixed(raw: int) -> float:
    """Signed 16-bit S9.7 → float (sign-extends from 16 bits)."""
    raw = _PACK_S.unpack(_PACK_U.pack(raw & 0xFFFF))[0]
    return raw / 128.0


# ── Parity vectors against html/app/ota/fixedpt.py ───────────────────────────
_PARITY: list[tuple[float, int]] = [
    (0.0,          0),       # 0x0000
    (1.0,          128),     # 0x0080
    (25.5,         3264),    # 0x0CC0
    (255.9921875,  32767),   # 0x7FFF  (signed max)
    (-1.0,         -128),    # 0xFF80 as signed = -128
    (-256.0,       -32768),  # 0x8000 as signed = -32768 (signed min)
]


def _selfcheck() -> None:
    for v, expected_raw in _PARITY:
        assert to_fixed(v) == expected_raw, f"to_fixed({v}) = {to_fixed(v):#06x} != {expected_raw:#06x}"
        assert abs(from_fixed(expected_raw) - v) <= _LSB, f"from_fixed({expected_raw:#06x}) drifted"


_selfcheck()
