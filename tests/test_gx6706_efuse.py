# SPDX-License-Identifier: MIT
"""Round-trip checks for compact GX6706 per-unit DDR eFuse descriptors."""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "ipl" / "gx6706_ipl.c"
TABLE_RE = re.compile(
    r"gx6706_efuse_tweaks\[\]\s*=\s*\{(.*?)\n\};", re.S
)
ENTRY_RE = re.compile(r"ET3\(([^)]*)\)")
WIDTH_CODE = {1: 0, 2: 1, 3: 7, 4: 2, 8: 3}
CODE_WIDTH = {code: width for width, code in WIDTH_CODE.items()}


def source_rows():
    text = SOURCE.read_text()
    match = TABLE_RE.search(text)
    if not match:
        raise AssertionError("compact GX6706 eFuse table not found")

    rows = []
    for args in ENTRY_RE.findall(match.group(1)):
        fields = [int(value.strip().rstrip("uU"), 0) for value in args.split(",")]
        if len(fields) != 5:
            raise AssertionError(f"expected five ET3 fields, got {args!r}")
        rows.append(tuple(fields))
    if not rows:
        raise AssertionError("parsed an empty GX6706 eFuse table")
    return rows


def pack(row):
    address, shift, width, byte_index, source_shift = row
    base = 1 if address & 0x00F00000 == 0x00C00000 else 0
    offset_words = (address & 0xFFF) >> 2
    return (
        base
        | (offset_words << 1)
        | (shift << 10)
        | (WIDTH_CODE[width] << 15)
        | (byte_index << 18)
        | (source_shift << 21)
    )


def unpack(descriptor):
    base = descriptor & 1
    offset_words = (descriptor >> 1) & 0x1FF
    shift = (descriptor >> 10) & 0x1F
    width_code = (descriptor >> 15) & 7
    byte_index = (descriptor >> 18) & 7
    source_shift = (descriptor >> 21) & 7
    address = (0x00C00000 if base else 0x0030A000) | (offset_words << 2)
    return (
        address,
        shift,
        CODE_WIDTH[width_code],
        byte_index,
        source_shift,
    )


class Gx6706EfuseDescriptorTests(unittest.TestCase):
    def test_all_original_fields_round_trip(self):
        rows = source_rows()
        self.assertEqual(len(rows), 50)
        for row in rows:
            with self.subTest(row=row):
                descriptor = pack(row)
                self.assertLess(descriptor, 1 << 24)
                self.assertEqual(unpack(descriptor), row)

    def test_field_updates_match_for_every_source_byte_value(self):
        """Decoded source selectors produce the same values as the old ET path."""
        for row in source_rows():
            descriptor = pack(row)
            _address, shift, width, byte_index, source_shift = unpack(descriptor)
            for byte_value in range(256):
                expected = byte_value >> row[4]
                actual = byte_value >> source_shift
                self.assertEqual((shift, width, byte_index, actual),
                                 (row[1], row[2], row[3], expected))


if __name__ == "__main__":
    unittest.main()
