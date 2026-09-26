#!/usr/bin/env python3
"""UART stub Stage 1 / GXID / GXBC helpers used by libre_gxdl."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import libre_gxdl  # noqa: E402


class GxidContractTests(unittest.TestCase):
    def test_parse_gxid_ignores_bootrom_junk(self) -> None:
        buffer = b"B0B8X\x00GXID family=gemini name=6702S5-NNNB\r\nRUNGET"
        parsed = libre_gxdl.parse_gxid(buffer)
        self.assertEqual(parsed, {"family": "gemini", "name": "6702S5-NNNB"})

    def test_parse_gxid_cygnus(self) -> None:
        parsed = libre_gxdl.parse_gxid(b"GXID family=cygnus name=6706S5-NNNB\r\n")
        self.assertEqual(parsed, {"family": "cygnus", "name": "6706S5-NNNB"})

    def test_parse_gxid_detection_only_families(self) -> None:
        for family, name in (
                ("gx6616", "GX6616"),
                ("gx3211", "3211"),
                ("gx6612", "6612A"),
        ):
            parsed = libre_gxdl.parse_gxid(
                f"GXID family={family} name={name}\r\nENODDR\r\n".encode())
            self.assertEqual(parsed, {"family": family, "name": name})
            self.assertFalse(libre_gxdl.family_trains_ddr(family))
            self.assertIsNone(libre_gxdl.bootcode_filename_for_family(family))

    def test_gxid_line_contract(self) -> None:
        line = "GXID family=gemini name=6702S5-NNNB\r\n"
        self.assertRegex(
            line,
            r"^GXID family=(gemini|cygnus|unknown|gx6616|gx3211|gx6612) name=\S+\r\n$")

    def test_wrap_gxbc(self) -> None:
        payload = b"bootcode"
        record = libre_gxdl.wrap_gxbc(payload)
        self.assertEqual(struct.unpack_from("<IIII", record),
                         (0x43425847, len(payload), 0x93C00000, sum(payload)))
        self.assertEqual(record[16:], payload)

    def test_bootcode_filename_map(self) -> None:
        self.assertEqual(libre_gxdl.bootcode_filename_for_family("gemini"),
                         "gx6702-bootcode.bin")
        self.assertEqual(libre_gxdl.bootcode_filename_for_family("cygnus"),
                         "gx6706-bootcode.bin")
        self.assertIsNone(libre_gxdl.bootcode_filename_for_family("unknown"))
        self.assertIsNone(libre_gxdl.bootcode_filename_for_family("gx6616"))
        self.assertTrue(libre_gxdl.family_trains_ddr("gemini"))
        self.assertFalse(libre_gxdl.family_trains_ddr("gx6612"))

    def test_gxid_without_bootcode_is_not_vendor_stage2(self) -> None:
        gxid = {"family": "cygnus", "name": "6706S5-NNNBE"}
        self.assertEqual(libre_gxdl.select_open_ipl_stage2(gxid, None),
                         "missing_bootcode")
        self.assertEqual(libre_gxdl.select_open_ipl_stage2(gxid, Path("x.bin")),
                         "gxbc")
        self.assertEqual(libre_gxdl.select_open_ipl_stage2(None, None), "vendor")
        self.assertEqual(
            libre_gxdl.bootcode_build_hint("cygnus"),
            "make SOC=gx6706 bootcode")


class Stage1LayoutTests(unittest.TestCase):
    def make_toob(self, chip_id: int, size: int = 0x2020) -> bytes:
        image = bytearray(size)
        image[0:4] = b"toob"
        struct.pack_into("<H", image, 6, chip_id)
        image[0x20:0x20 + 4] = b"IPL0"
        return bytes(image)

    def test_shared_8k_stage1_header(self) -> None:
        header, payload, marker = libre_gxdl.stage1_8k_parts(self.make_toob(0x6701))
        self.assertEqual(header, bytes.fromhex("5900080000"))
        self.assertEqual(len(payload), 8188)
        self.assertEqual(marker, b"boot")

    def test_stub_ignores_chip_id_for_transfer_size(self) -> None:
        uploader = libre_gxdl.GXUploader("/dev/null")
        for chip_id in (0x6701, 0x6705, 0x1234):
            header, payload, marker = uploader._build_stage1_parts(
                self.make_toob(chip_id))
            self.assertEqual(header, bytes.fromhex("5900080000"))
            self.assertEqual(len(payload), 8188)
            self.assertEqual(marker, b"boot")

    def test_vendor_chip_override_does_not_resize_stub(self) -> None:
        uploader = libre_gxdl.GXUploader("/dev/null")
        uploader.chip_override = 0x6612
        header, payload, _marker = uploader._build_stage1_parts(
            self.make_toob(0x6701))
        self.assertEqual(header, bytes.fromhex("5900080000"))
        self.assertEqual(len(payload), 8188)

    def test_gxai_prefix_still_sends_first_member_once(self) -> None:
        stub = self.make_toob(0x6701)
        extra = self.make_toob(0x6705)
        image = stub + b"GXAI" + extra
        self.assertTrue(libre_gxdl.is_uart_ipl_stub(image))
        header, payload, _marker = libre_gxdl.GXUploader(
            "/dev/null")._build_stage1_parts(image)
        self.assertEqual(header, bytes.fromhex("5900080000"))
        self.assertEqual(payload, stub[0x20:0x20 + 8188])

    def test_gxmt_catalog_does_not_change_stage1(self) -> None:
        image = bytearray(self.make_toob(0x6701))
        image[0x0C:0x10] = b"GXMT"
        image[0x10] = 1
        image[0x11] = 2
        struct.pack_into("<HH", image, 0x12, 0x6705, 0x6616)
        image = bytes(image)
        self.assertEqual(libre_gxdl.parse_target_catalog(image),
                         [0x6705, 0x6616])
        self.assertEqual(libre_gxdl.header_supported_chip_ids(image),
                         [0x6701, 0x6705, 0x6616])
        header, payload, marker = libre_gxdl.GXUploader(
            "/dev/null")._build_stage1_parts(image)
        self.assertEqual(header, bytes.fromhex("5900080000"))
        self.assertEqual(len(payload), 8188)
        self.assertEqual(marker, b"boot")


class BootcodeHandshakeTests(unittest.TestCase):
    def test_payload_stage2_probes_before_metadata(self) -> None:
        class FakeSerial:
            def __init__(self):
                self.writes = []
                self.flushes = 0
                self.ack_offset = 0

            def write(self, data):
                self.writes.append(bytes(data))
                return len(data)

            def flush(self):
                self.flushes += 1

            def read(self, size):
                if size < 1:
                    raise AssertionError(f"unexpected read size: {size}")
                byte = libre_gxdl.UART_HELO_ACK[self.ack_offset:self.ack_offset + 1]
                self.ack_offset += len(byte)
                return byte

        uploader = libre_gxdl.GXUploader("/dev/null")
        serial = FakeSerial()
        uploader.ser = serial
        payload = b"bootcode"

        self.assertTrue(uploader.send_payload_stage2(payload))
        self.assertEqual(serial.writes[0], libre_gxdl.UART_HELO)
        self.assertEqual(serial.writes[1], struct.pack("<I", sum(payload)))
        self.assertEqual(serial.writes[2], struct.pack("<I", len(payload)))
        self.assertEqual(b"".join(serial.writes[3:]), payload)
        self.assertGreaterEqual(serial.flushes, 1)


if __name__ == "__main__":
    unittest.main()
