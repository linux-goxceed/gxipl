#!/usr/bin/env python3
"""Host-side format and compatibility tests for GX6702/GX6706 artifacts."""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UTILS = ROOT / "utils"
sys.path.insert(0, str(UTILS))

import check_elf  # noqa: E402
import mk_flash_image  # noqa: E402
import mk_flash_probe64  # noqa: E402
import mkboot  # noqa: E402


def config_sum(config: bytes) -> int:
    return sum(value for index, value in enumerate(config)
               if index >= 8 and not 0x1F8 <= index < 0x1FC) & 0xFFFF


class BootRomContainerTests(unittest.TestCase):
    def run_mkboot(self, soc: str | None, payload: bytes = b"IPL") -> bytes:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "ipl.bin"
            output = Path(directory) / "ipl.boot"
            source.write_bytes(payload)
            command = [sys.executable, str(UTILS / "mkboot.py")]
            if soc is not None:
                command += ["--soc", soc]
            command += [str(source), str(output)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            return output.read_bytes()

    def test_crc_known_vector(self) -> None:
        self.assertEqual(mkboot.bootrom_stage1_crc(b"123456789"), 0x0376E6E7)

    def test_gx6706_exact_window_id_config_and_crc(self) -> None:
        image = self.run_mkboot("gx6706")
        self.assertEqual(len(image), 0x2020)
        self.assertEqual(image[:4], b"toob")
        self.assertEqual(struct.unpack_from("<H", image, 6)[0], 0x6705)
        body = image[0x20:]
        config = body[0x1E00:0x2000]
        self.assertEqual(struct.unpack_from("<H", config, 6)[0],
                         config_sum(config))
        self.assertEqual(struct.unpack_from("<I", body, 0x1FF8)[0],
                         mkboot.bootrom_stage1_crc(body[:0x1FF8]))

    def test_default_remains_gx6702_legacy_compatible(self) -> None:
        default = self.run_mkboot(None)
        explicit = self.run_mkboot("gx6702")
        self.assertEqual(default, explicit)
        self.assertEqual(struct.unpack_from("<H", default, 6)[0], 0x6701)
        self.assertEqual(default[0x20 + 0x1FF8:0x20 + 0x1FFC],
                         mkboot.LEGACY_TRAILER)

    def test_code_window_is_enforced(self) -> None:
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_mkboot("gx6706", bytes(0x1E01))

    def patch_config(self, image: bytes) -> bytes:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ipl.boot"
            path.write_bytes(image)
            subprocess.run([
                sys.executable, str(UTILS / "iplcfg.py"), str(path),
                "--verbose", "0",
            ], check=True, capture_output=True, text=True)
            return path.read_bytes()

    def test_iplcfg_reseals_gx6706_bootrom_crc(self) -> None:
        image = self.patch_config(self.run_mkboot("gx6706"))
        body = image[0x20:]
        config = body[0x1E00:0x2000]
        self.assertEqual(struct.unpack_from("<I", config, 8)[0] & 1, 0)
        self.assertEqual(struct.unpack_from("<H", config, 6)[0],
                         config_sum(config))
        self.assertEqual(struct.unpack_from("<I", body, 0x1FF8)[0],
                         mkboot.bootrom_stage1_crc(body[:0x1FF8]))

    def test_iplcfg_retains_gx6702_legacy_trailer(self) -> None:
        image = self.patch_config(self.run_mkboot("gx6702"))
        self.assertEqual(image[0x20 + 0x1FF8:0x20 + 0x1FFC],
                         mkboot.LEGACY_TRAILER)


class PackagingTests(unittest.TestCase):
    def test_gxbc_record(self) -> None:
        payload = b"payload"
        record = mk_flash_image.gxbc(payload, 0x93C00000)
        self.assertEqual(struct.unpack_from("<IIII", record),
                         (0x43425847, len(payload), 0x93C00000,
                          sum(payload)))
        self.assertEqual(record[16:], payload)

    def test_gx6706_128k_boot_layout(self) -> None:
        container = bytearray(0x2020)
        container[:4] = b"toob"
        container[0x20:0x2020] = bytes((index & 0xFF for index in range(0x2000)))
        bootcode = b"BC" * 127
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ipl = directory / "ipl.boot"
            bc = directory / "bc.bin"
            output = directory / "BOOT.bin"
            ipl.write_bytes(container)
            bc.write_bytes(bootcode)
            subprocess.run([
                sys.executable, str(UTILS / "splice_boot.py"),
                "--ipl-boot", str(ipl), "--bootcode", str(bc),
                "--size", "0x20000", "-o", str(output),
            ], check=True, capture_output=True, text=True)
            image = output.read_bytes()
        self.assertEqual(len(image), 0x20000)
        self.assertEqual(image[:4], bytes.fromhex("aa55aa55"))
        self.assertEqual(image[4:4 + 0x2000], container[0x20:])
        self.assertEqual(struct.unpack_from("<IIII", image, 0x4000),
                         (0x43425847, len(bootcode), 0x93C00000,
                          sum(bootcode)))
        self.assertEqual(image[0x4010:0x4010 + len(bootcode)], bootcode)

    def test_table_partition_and_crc_update(self) -> None:
        table_path = ROOT.parent / "extracted_partitions" / "TABLE.bin"
        if not table_path.is_file():
            self.skipTest("GX6706 TABLE dump is not present")
        boot = bytes((index * 17) & 0xFF for index in range(0x20000))
        table, info = mk_flash_probe64.patch_table_for_boot(
            table_path.read_bytes(), boot)
        self.assertEqual(info["boot_total"], 0x20000)
        self.assertEqual(int.from_bytes(table[13:17], "big"), 0x20000)
        self.assertEqual(int.from_bytes(table[21:25], "big"), 0)
        self.assertEqual(int.from_bytes(table[0x19D:0x1A1], "big"),
                         zlib.crc32(boot) & 0xFFFFFFFF)
        self.assertEqual(int.from_bytes(table[0x1FC:0x200], "big"),
                         zlib.crc32(table[:0x1FC]) & 0xFFFFFFFF)

    def test_sealing_preserves_config_and_source(self) -> None:
        original = bytearray(0x2020)
        original[:4] = b"toob"
        original[0x20 + 0x1E00:0x20 + 0x2000] = mkboot.default_config_blob()
        before = bytes(original)
        sealed = mk_flash_image.seal_ipl_container(before, 0x4000, 0, True)
        self.assertEqual(bytes(original), before)
        cfg = sealed[0x20 + 0x1E00:0x20 + 0x2000]
        self.assertEqual(struct.unpack_from("<I", cfg, 16)[0], 0x4000)
        self.assertEqual(struct.unpack_from("<H", cfg, 6)[0], config_sum(cfg))
        self.assertEqual(cfg[28:0x1F8], before[0x20 + 0x1E00 + 28:
                                             0x20 + 0x1E00 + 0x1F8])


def make_elf(vaddr: int = 0x90001000, memsz: int = 4) -> bytes:
    data = bytearray(0x104)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    data[:16] = ident
    struct.pack_into("<HHIIIIIHHHHHH", data, 16,
                     2, 252, 1, vaddr, 52, 0, 0x11000002,
                     52, 32, 1, 0, 0, 0)
    struct.pack_into("<IIIIIIII", data, 52,
                     1, 0x100, vaddr, vaddr, 4, memsz, 5, 4)
    data[0x100:0x104] = b"CODE"
    return bytes(data)


class ElfBoundsTests(unittest.TestCase):
    def test_valid_elf(self) -> None:
        self.assertEqual(check_elf.validate_elf(make_elf()), 0x90001000)

    def test_segment_past_64m_is_rejected(self) -> None:
        with self.assertRaises(check_elf.ElfError):
            check_elf.validate_elf(make_elf(0x93FFFFFC, 8))

    def test_abiv2_is_rejected(self) -> None:
        data = bytearray(make_elf())
        struct.pack_into("<I", data, 36, 0x20000002)
        with self.assertRaises(check_elf.ElfError):
            check_elf.validate_elf(bytes(data))


class VendorEvidenceTests(unittest.TestCase):
    def test_h5_s5_stage1_bodies_match_when_dumps_are_available(self) -> None:
        loaders = ROOT.parent / "libre-gxdl" / "loaders"
        h5 = loaders / "cygnus-6706H5-sflash-24M.boot"
        s5 = loaders / "cygnus-6706S5-sflash-24M.boot"
        if not h5.is_file() or not s5.is_file():
            self.skipTest("vendor H5/S5 loader evidence is not present")
        self.assertEqual(h5.read_bytes()[0x20:0x20 + 8188],
                         s5.read_bytes()[0x20:0x20 + 8188])


if __name__ == "__main__":
    unittest.main()
