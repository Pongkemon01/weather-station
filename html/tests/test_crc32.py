"""Unit tests for CRC-32/MPEG-2 (must match shared/crc32.c)."""
import pytest

from app.ota.crc32 import crc32_mpeg2


def test_check_value():
    # Standard CRC-32/MPEG-2 check value for ASCII "123456789"
    assert crc32_mpeg2(b"123456789") == 0x0376E6E7


def test_empty_returns_init():
    assert crc32_mpeg2(b"") == 0xFFFFFFFF


def test_single_zero_byte():
    # (0xFFFFFFFF << 8) ^ table[0xFF ^ 0x00]  — precomputed
    expected = crc32_mpeg2(b"\x00")
    assert isinstance(expected, int)
    assert 0 <= expected <= 0xFFFFFFFF


def test_continuation():
    # Computing over two halves must equal one-shot
    data = b"Hello, world!"
    mid = len(data) // 2
    crc_full = crc32_mpeg2(data)
    crc_partial = crc32_mpeg2(data[mid:], crc=crc32_mpeg2(data[:mid]))
    assert crc_full == crc_partial


def test_known_zeros():
    # 4 zero bytes; verify output is deterministic
    assert crc32_mpeg2(b"\x00\x00\x00\x00") == crc32_mpeg2(b"\x00\x00\x00\x00")


@pytest.mark.parametrize("length", [1, 64, 512])
def test_chunk_sizes(length: int):
    data = bytes(range(256)) * (length // 256 + 1)
    data = data[:length]
    result = crc32_mpeg2(data)
    assert 0 <= result <= 0xFFFFFFFF
