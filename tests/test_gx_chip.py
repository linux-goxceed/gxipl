#!/usr/bin/env python3
"""Host-side contract for the 12-byte reversed silicon name field."""

from __future__ import annotations

import unittest


def gx_family_from_raw(raw: bytes) -> tuple[str, str]:
    """Mirror ipl/gx_chip.c: skip leading NULs, reverse printable bytes."""
    if len(raw) != 12:
        raise ValueError("chip name field is 12 bytes")
    first = 0
    while first < 12 and raw[first] == 0:
        first += 1
    valid = first < 12
    for index in range(first, 12):
        if raw[index] < 0x20 or raw[index] > 0x7E:
            valid = False
    if not valid:
        return "unknown", "unavailable"
    name = bytes(raw[index] for index in range(11, first - 1, -1)).decode("ascii")
    if any(key in name for key in ("6701", "6702", "6703")):
        return "gemini", name
    if any(key in name for key in ("6705", "6706")):
        return "cygnus", name
    if "6616" in name:
        return "gx6616", name
    if "3211" in name:
        return "gx3211", name
    if "6612" in name:
        return "gx6612", name
    return "unknown", name


def encode_chip_name(name: str) -> bytes:
    reversed_name = name.encode("ascii")[::-1]
    if len(reversed_name) > 12:
        raise ValueError("name exceeds 12 bytes")
    return bytes(12 - len(reversed_name)) + reversed_name


class ChipNameDecodeTests(unittest.TestCase):
    def test_gx6702s5_runtime_dump(self) -> None:
        # REVERSE_ENGINEERING.md: 0x4e4e4200 0x53352d4e 0x36373032
        raw = bytes.fromhex("00424e4e4e2d355332303736")
        self.assertEqual(gx_family_from_raw(raw), ("gemini", "6702S5-NNNB"))

    def test_gx6706s5_twelve_char_name(self) -> None:
        raw = encode_chip_name("6706S5-NNNB")
        self.assertEqual(gx_family_from_raw(raw), ("cygnus", "6706S5-NNNB"))

    def test_non_printable_is_unavailable(self) -> None:
        self.assertEqual(gx_family_from_raw(b"\x00" * 11 + b"\x01"),
                         ("unknown", "unavailable"))

    def test_all_nuls_are_unavailable(self) -> None:
        self.assertEqual(gx_family_from_raw(bytes(12)),
                         ("unknown", "unavailable"))

    def test_printable_unknown_family(self) -> None:
        raw = encode_chip_name("ABCD1234")
        self.assertEqual(gx_family_from_raw(raw), ("unknown", "ABCD1234"))

    def test_gx6616_detection_only(self) -> None:
        raw = encode_chip_name("GX6616")
        self.assertEqual(gx_family_from_raw(raw), ("gx6616", "GX6616"))

    def test_gx3211_detection_only(self) -> None:
        raw = encode_chip_name("3211")
        self.assertEqual(gx_family_from_raw(raw), ("gx3211", "3211"))

    def test_gx6612_detection_only(self) -> None:
        raw = encode_chip_name("6612A")
        self.assertEqual(gx_family_from_raw(raw), ("gx6612", "6612A"))


if __name__ == "__main__":
    unittest.main()
