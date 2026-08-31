#!/usr/bin/env python3
"""Wrap an open IPL in a GX6702/GX6706 BootROM UART container."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

HEADER_SIZE = 0x20
BODY_SIZE = 0x2000		# BootROM stage-1 window (UART and flash)
CODE_END = 0x1E00		# last 512 bytes reserved for IPL config
TRAILER_OFF = 0x1FF8		# BootROM CRC / legacy trailer word
LEGACY_TRAILER = bytes.fromhex("33dea189")
CONFIG_SIZE = 512

SOCS = {
    "gx6702": {"chip_id": 0x6701, "crc_trailer": False},
    "gx6706": {"chip_id": 0x6705, "crc_trailer": True},
}


def bootrom_stage1_crc(data: bytes) -> int:
    """MSB-first GX BootROM CRC-32, poly 0x04C11DB7, no final XOR."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000
                   else (crc << 1)) & 0xFFFFFFFF
    return crc


def _cfg_crc(cfg: bytes) -> int:
    total = 0
    for i, b in enumerate(cfg):
        if i < 8:
            continue
        if 0x1F8 <= i < 0x1FC:
            continue
        total += b
    return total & 0xFFFF


def default_config_blob() -> bytes:
    """Build a valid v1 IPL config (verbosity on)."""
    blob = bytearray(CONFIG_SIZE)
    struct.pack_into("<IHH", blob, 0, 0x47464331, 1, 0)
    struct.pack_into("<IIII", blob, 8, 0x1 | 0x10, 60, 0x4000, 0x10000)  # verbose|uart_direct
    struct.pack_into("<I", blob, 24, 512 * 1024)
    struct.pack_into("<H", blob, 6, _cfg_crc(blob))
    return bytes(blob)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--soc", choices=sorted(SOCS), default="gx6702")
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--config", type=Path,
                        help="optional 512-byte config blob to place at end")
    args = parser.parse_args()

    payload = Path(args.input).read_bytes()
    if len(payload) > CODE_END:
        parser.error(f"IPL is {len(payload)} bytes; limit is {CODE_END} "
                     f"(8 KiB body minus 512-byte config)")

    header = bytearray(HEADER_SIZE)
    header[0:4] = b"toob"
    soc = SOCS[args.soc]
    struct.pack_into("<HHI", header, 4, 0x0100, soc["chip_id"], 115200)

    body = bytearray(BODY_SIZE)
    body[:len(payload)] = payload

    if args.config is not None:
        cfg = args.config.read_bytes()
        if len(cfg) != CONFIG_SIZE:
            parser.error(f"config must be {CONFIG_SIZE} bytes, got {len(cfg)}")
    else:
        cfg = default_config_blob()

    # Config occupies the last 512 bytes. Legacy trailer word stays at the
    # historical offset inside that region (0x1FF8).
    cfg_off = BODY_SIZE - CONFIG_SIZE
    body[cfg_off:cfg_off + CONFIG_SIZE] = cfg
    struct.pack_into("<H", body, cfg_off + 6,
                     _cfg_crc(body[cfg_off:cfg_off + CONFIG_SIZE]))
    if soc["crc_trailer"]:
        struct.pack_into("<I", body, TRAILER_OFF,
                         bootrom_stage1_crc(bytes(body[:TRAILER_OFF])))
    else:
        body[TRAILER_OFF:TRAILER_OFF + 4] = LEGACY_TRAILER

    Path(args.output).write_bytes(header + body)
    print(f"wrote {args.output}: {args.soc} IPL {len(payload)} bytes, body 0x{BODY_SIZE:x}, "
          f"config @{cfg_off:#x}, container {HEADER_SIZE + BODY_SIZE} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
