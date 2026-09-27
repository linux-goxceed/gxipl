#!/usr/bin/env python3
"""Tests for the stage-1.5 USB image packer (utils/mk_stage15.py).

The important invariant here is the checksum contract with
ipl/fat/stage15_usb.c.  It previously folded the 16 header bytes into the
running sum *including* the stored checksum field, which is unsatisfiable:
the check reduces to sum(H_other) + sum(payload) == 0 mod 2^32.  The loader
now sums the payload only, matching try_spi_bootcode() and splice_boot.py.
These tests pin that behaviour down from the C side so it cannot regress
silently.
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "utils"))

import mk_stage15  # noqa: E402


# --- Mirrors of the C loader, transcribed from ipl/fat/stage15_usb.c -------

BOOTCODE_MAGIC = 0x43425847


def loader_accepts(image: bytes) -> bool:
    """Replay stage15_try() exactly: validate header, sum payload only."""
    if len(image) < 16:
        return False
    magic, size, entry, checksum = struct.unpack("<IIII", image[:16])
    if magic != BOOTCODE_MAGIC or not size or size > 512 * 1024:
        return False
    payload = image[16:16 + size]
    if len(payload) != size:
        return False
    total = 0
    for byte in payload:                      # payload only -- see fix
        total = (total + byte) & 0xFFFFFFFF
    if total != checksum:
        return False
    # jump_to(hdr.entry ? hdr.entry : BOOTCODE_ENTRY): a zero entry falls
    # back to BOOTCODE_ENTRY, any non-zero value is honoured verbatim, so
    # the loader itself does not constrain the value -- mk_stage15.verify()
    # is what range-checks it at build time.
    del entry
    return True


class TestHeaderLayout(unittest.TestCase):
    def test_header_is_sixteen_bytes_little_endian(self):
        image = mk_stage15.pack_stage15(b"\x01\x02\x03\x04")
        magic, size, entry, checksum = struct.unpack("<IIII", image[:16])
        self.assertEqual(magic, BOOTCODE_MAGIC)
        self.assertEqual(size, 4)
        self.assertEqual(entry, 0x93C00000)
        self.assertEqual(checksum, 1 + 2 + 3 + 4)
        self.assertEqual(len(image), 16 + 4)

    def test_magic_is_ascii_gxbc(self):
        image = mk_stage15.pack_stage15(b"\xaa" * 32)
        self.assertEqual(image[:4], b"GXBC")

    def test_payload_follows_header_verbatim(self):
        payload = bytes(range(256))
        image = mk_stage15.pack_stage15(payload)
        self.assertEqual(image[16:], payload)

    def test_entry_is_overridable(self):
        image = mk_stage15.pack_stage15(b"\x00" * 8, entry=0x93CE8420)
        self.assertEqual(struct.unpack("<I", image[8:12])[0], 0x93CE8420)
        self.assertTrue(loader_accepts(image))
        # ...but the packer refuses an entry outside the stage-2 window.
        self.assertTrue(mk_stage15.checksum_matches_loader(image))


class TestChecksumContract(unittest.TestCase):
    """The checksum must satisfy the loader it is paired with."""

    def test_loader_accepts_packed_image(self):
        for payload in (b"\x00" * 1, bytes(range(256)), b"\xff" * 4096,
                        bytes(range(256)) * 40):
            with self.subTest(n=len(payload)):
                self.assertTrue(
                    loader_accepts(mk_stage15.pack_stage15(payload)),
                    "loader rejected a correctly packed image")

    def test_checksum_is_payload_only(self):
        """Changing entry must not change the checksum.

        Under the old header-folding rule the entry bytes were part of the
        sum, so this would fail.  Payload-only means the checksum is a
        property of the payload alone, identical to the SPI path.
        """
        payload = b"\x5a" * 256
        a = mk_stage15.pack_stage15(payload, entry=0x93C00000)
        b = mk_stage15.pack_stage15(payload, entry=0x93CE8420)
        self.assertEqual(struct.unpack("<I", a[12:16])[0],
                         struct.unpack("<I", b[12:16])[0])

    def test_corrupted_payload_is_rejected(self):
        image = bytearray(mk_stage15.pack_stage15(b"\x11" * 64))
        image[20] ^= 0xFF
        self.assertFalse(loader_accepts(bytes(image)))

    def test_corrupted_checksum_is_rejected(self):
        image = bytearray(mk_stage15.pack_stage15(b"\x11" * 64))
        image[12] ^= 0x01
        self.assertFalse(loader_accepts(bytes(image)))

    def test_bad_magic_is_rejected(self):
        image = bytearray(mk_stage15.pack_stage15(b"\x11" * 64))
        image[0] ^= 0xFF
        self.assertFalse(loader_accepts(bytes(image)))

    def test_matches_splice_boot_convention(self):
        """Same checksum rule as the flash-resident form in splice_boot.py.

        If these ever diverge, one of the two paths silently stops booting.
        """
        payload = bytes(range(200))
        image = mk_stage15.pack_stage15(payload)
        # splice_boot.py: sum(bootcode) & 0xFFFFFFFF, payload only.
        self.assertEqual(struct.unpack("<I", image[12:16])[0],
                         sum(payload) & 0xFFFFFFFF)


class TestRejections(unittest.TestCase):
    def test_empty_payload_rejected(self):
        with self.assertRaises(ValueError):
            mk_stage15.pack_stage15(b"")

    def test_oversize_payload_rejected(self):
        with self.assertRaises(ValueError):
            mk_stage15.pack_stage15(b"\x00" * (mk_stage15.BOOTCODE_MAX_SIZE + 1))

    def test_max_size_payload_accepted(self):
        payload = b"\x00" * mk_stage15.BOOTCODE_MAX_SIZE
        self.assertEqual(len(mk_stage15.pack_stage15(payload)),
                         16 + mk_stage15.BOOTCODE_MAX_SIZE)

    def test_verify_catches_bad_magic(self):
        image = bytearray(mk_stage15.pack_stage15(b"\x01" * 8))
        image[0] = 0
        with self.assertRaises(ValueError):
            mk_stage15.verify(bytes(image))

    def test_verify_catches_truncated_payload(self):
        image = mk_stage15.pack_stage15(b"\x01" * 64)[:-1]
        with self.assertRaises(ValueError):
            mk_stage15.verify(image)

    def test_verify_catches_out_of_range_entry(self):
        image = mk_stage15.pack_stage15(b"\x01" * 8, entry=0x1000)
        with self.assertRaises(ValueError):
            mk_stage15.verify(image)


class TestFilenames(unittest.TestCase):
    def test_names_are_valid_8_3(self):
        for soc, name in mk_stage15.SOC_FILENAMES.items():
            with self.subTest(soc=soc):
                base, dot, ext = name.partition(".")
                self.assertTrue(dot, f"{name} has no extension")
                self.assertLessEqual(len(base), 8, f"{name} base too long")
                self.assertLessEqual(len(ext), 3, f"{name} ext too long")
                self.assertTrue(name.isascii())
                self.assertEqual(name, name.upper())

    def test_names_match_ipl_probe_list(self):
        """The IPL hardcodes these; a rename here is a silent boot failure."""
        loader = (REPO / "ipl" / "fat" / "stage15_usb.c").read_text()
        for soc, name in mk_stage15.SOC_FILENAMES.items():
            with self.subTest(soc=soc):
                self.assertIn(f'"/{name}"', loader,
                              f"{name} is not probed by stage15_usb.c")


class TestRealBootcode(unittest.TestCase):
    """End-to-end against an actual build artifact, when one exists."""

    def test_roundtrip_with_built_bootcode(self):
        for soc in ("gx6702", "gx6706"):
            src = REPO / f"{soc}-bootcode.bin"
            if not src.exists():
                self.skipTest(f"{src.name} not built")
            with self.subTest(soc=soc):
                payload = src.read_bytes()
                image = mk_stage15.pack_stage15(payload)
                self.assertTrue(loader_accepts(image))
                mk_stage15.verify(image)


if __name__ == "__main__":
    unittest.main()
