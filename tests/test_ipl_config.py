# SPDX-License-Identifier: MIT
"""Regression tests for the IPL config blob checksum.

The bug this guards against cost several hardware bring-up rounds on gx6706:
the checksum summed the final 4 bytes of the 64-byte config, but the BootROM
only copies ``BOOT[4:0x2000]`` == 8188 bytes (SRAM up to 0x00101FFB) while the
config runs to 0x00101FFF.  Bytes 60..63 therefore held whatever was already
in SRAM, so ``ipl_config_valid()`` failed on hardware even though the very
same blob validated fine in the built ``.boot`` file.  ``ipl_config_load()``
then silently fell back to defaults on *every* boot, so VERBOSE, SKIP_*,
UART_DIRECT, FORCE_UART, the timeout and the flash offsets were all ignored --
and ``ipl_config_set_flag()`` (guarded on validity) became a no-op, which is
what left the bootcode re-running the gx6706 PHY bring-up against a live PHY
and hanging.
"""

from __future__ import annotations

import re
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))

import iplcfg  # noqa: E402
import mkboot  # noqa: E402

CONFIG_SIZE = 64
TRAILER_OFF = CONFIG_SIZE - 8      # 0x38: BootROM CRC-32 / legacy trailer
UNCOPIED_OFF = CONFIG_SIZE - 4     # 0x3C: never written by the BootROM
BOOTROM_COPY_LEN = 0x2000 - 4      # 8188 -> SRAM ends at 0x00101FFB
CONFIG_VA = 0x00101FC0


def c_cfg_crc(cfg: bytes) -> int:
    """Transcription of cfg_crc() in ipl/ipl_config.c.

    Must be kept byte-for-byte in step with the C and with the two Python
    sealers; the three are cross-checked in test_all_three_agree.
    """
    total = 0
    for i in range(8, CONFIG_SIZE):
        if TRAILER_OFF <= i < TRAILER_OFF + 4:
            continue
        if i >= UNCOPIED_OFF:
            continue
        total += cfg[i]
    return total & 0xFFFF


class TestUncopiedTailIsExcluded(unittest.TestCase):
    def test_trailer_end_is_past_bootrom_copy(self):
        """The premise of the bug: the config tail outlives the copy."""
        copied_end = 0x00100000 + BOOTROM_COPY_LEN
        self.assertEqual(copied_end, 0x00101FFC)
        self.assertEqual(CONFIG_VA + CONFIG_SIZE, 0x00102000)
        self.assertGreater(CONFIG_VA + CONFIG_SIZE, copied_end)
        # ...and the uncopied region is exactly the last 4 bytes.
        self.assertEqual(CONFIG_VA + UNCOPIED_OFF, copied_end)

    def test_crc_ignores_uncopied_bytes(self):
        blob = bytearray(mkboot.default_config_blob())
        want = c_cfg_crc(bytes(blob))
        for junk in (b"\xff\xff\xff\xff", b"\xde\xa1\x89\x33", b"\x01\x02\x03\x04"):
            other = bytearray(blob)
            other[UNCOPIED_OFF:CONFIG_SIZE] = junk
            self.assertEqual(
                c_cfg_crc(bytes(other)), want,
                "checksum must not depend on bytes the BootROM never writes")

    def test_blob_stays_valid_with_sram_garbage_in_tail(self):
        """The real-world failure: valid in the file, invalid in SRAM."""
        blob = bytearray(mkboot.default_config_blob())
        sealed = struct.unpack("<H", bytes(blob[6:8]))[0]
        for junk in (b"\x00\x00\x00\x00", b"\xff\xff\xff\xff"):
            sram = bytearray(blob)
            sram[UNCOPIED_OFF:CONFIG_SIZE] = junk
            self.assertEqual(c_cfg_crc(bytes(sram)), sealed,
                             "blob must validate regardless of SRAM contents")

    def test_crc_still_covers_everything_else(self):
        """Guard against 'fixing' it by excluding too much."""
        blob = bytearray(mkboot.default_config_blob())
        sealed = struct.unpack("<H", bytes(blob[6:8]))[0]
        for i in range(8, UNCOPIED_OFF):
            if TRAILER_OFF <= i < TRAILER_OFF + 4:
                continue
            other = bytearray(blob)
            other[i] ^= 0xFF
            self.assertNotEqual(
                c_cfg_crc(bytes(other)), sealed,
                "byte %d must be covered by the checksum" % i)


class TestSealersAgree(unittest.TestCase):
    def setUp(self):
        self.blob = mkboot.default_config_blob()

    def test_all_three_agree(self):
        self.assertEqual(c_cfg_crc(self.blob), mkboot._cfg_crc(self.blob))
        self.assertEqual(c_cfg_crc(self.blob), iplcfg.crc16(self.blob[8:]))

    def test_c_source_matches(self):
        """The C implementation must stop at the same bound."""
        src = (ROOT / "ipl" / "ipl_config.c").read_text()
        self.assertRegex(src, r"CFG_UNCOPIED_OFF\s*\(IPL_CONFIG_SIZE - 4u\)")
        self.assertRegex(src, r"for \(i = 8; i < CFG_UNCOPIED_OFF; i\+\+\)")


class TestSetFlagIsUnconditional(unittest.TestCase):
    def test_no_validity_guard(self):
        """A validity guard made the setter a silent no-op on hardware."""
        src = (ROOT / "ipl" / "ipl_config.c").read_text()
        start = src.index("void ipl_config_set_flag")
        body = src[start:src.index("int ipl_config_verbose_early", start)]
        self.assertNotIn("return;", body,
                         "setter must not bail out before writing the bit")
        self.assertIn("cfg->flags |= bit", body)
        self.assertIn("cfg->crc = cfg_crc", body)

    def test_handoff_flag_is_bit6(self):
        hdr = (ROOT / "include" / "ipl_config.h").read_text()
        m = re.search(r"#define\s+IPL_CFG_USB_PHY_READY\s+BIT\((\d+)\)", hdr)
        self.assertIsNotNone(m, "IPL_CFG_USB_PHY_READY must be defined")
        self.assertEqual(int(m.group(1)), 6)

    def test_handoff_uses_ddr_slot_not_config_blob(self):
        """The config blob is NOT delivered, so it cannot carry the handoff.

        The uploader transmits only file bytes 0x20..0x201B, while the config
        occupies 0x1FE0..0x201F.  Hardware showed the blob arriving as all
        zeros, so a flag written there was silently lost and the bootcode
        re-ran the gx6706 PHY bring-up against a live PHY and hung.
        """
        s15 = (ROOT / "ipl" / "fat" / "stage15_usb.c").read_text()
        self.assertIn("STAGE_HANDOFF_VA", s15)
        self.assertNotIn("ipl_config_set_flag", s15,
                         "handoff must not go through the undelivered config")

        # The slot must be plain DDR, clear of the bootcode image.
        hw = (ROOT / "include" / "gx_hw.h").read_text()
        m = re.search(r"#define\s+STAGE_HANDOFF_VA\s+(0x[0-9a-fA-F]+)u", hw)
        self.assertIsNotNone(m, "STAGE_HANDOFF_VA must be defined")
        slot = int(m.group(1), 16)
        self.assertGreaterEqual(slot, 0x90000000, "slot must be in the DDR window")
        self.assertGreater(slot, 0x93C059D4,
                           "slot must clear the bootcode .bss end (0x93c059D4)")

    def test_bootcode_reads_the_slot(self):
        main = (ROOT / "bootcode" / "main.c").read_text()
        self.assertIn("stage_handoff = *(volatile u32 *)STAGE_HANDOFF_VA", main)
        self.assertIn("stage_from_usb", main)

        msc = (ROOT / "bootcode" / "usb" / "usb_msc.c").read_text()
        # The PHY bring-up must be skipped when stage 1 came in over USB.
        self.assertRegex(
            msc,
            r"if \(!stage_from_usb\)\s*\{\s*\n\s*if \(gx_usb_pad_phy\(\)\)")
        self.assertNotIn("IPL_CFG_USB_PHY_READY", msc)

    def test_handoff_uses_a_magic_not_a_bare_flag(self):
        """Stale DDR from a previous boot must not look like a handoff."""
        hw = (ROOT / "include" / "gx_hw.h").read_text()
        m = re.search(r"#define\s+STAGE_HANDOFF_USB_PHY_READY\s+"
                      r"(0x[0-9a-fA-F]+)u", hw)
        self.assertIsNotNone(m)
        magic = int(m.group(1), 16)
        self.assertNotIn(magic, (1, 0),
                         "use a distinctive magic, not a plain flag value")


if __name__ == "__main__":
    unittest.main()
