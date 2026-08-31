#!/usr/bin/env python3
"""Splice open IPL + bootcode (+ optional U-Boot) into a flashable BOOT.bin."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

BOOTCODE_MAGIC = 0x43425847


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ipl-boot", type=Path, required=True,
                    help="GX6702/GX6706 toob container or raw 8 KiB IPL body")
    ap.add_argument("--bootcode", type=Path, required=True,
                    help="raw SoC-specific bootcode binary")
    ap.add_argument("--uboot", type=Path, help="optional raw u-boot.bin")
    ap.add_argument("--bootcode-off", type=lambda x: int(x, 0), default=0x4000)
    ap.add_argument("--uboot-off", type=lambda x: int(x, 0), default=0x10000)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=64 * 1024,
                    help="output BOOT image size (GX6702 64K, GX6706 128K)")
    args = ap.parse_args()

    ipl = args.ipl_boot.read_bytes()
    if ipl[:4] == b"toob":
        ipl_body = ipl[0x20:0x20 + 0x2000]
    else:
        ipl_body = ipl

    bootcode = args.bootcode.read_bytes()
    out = bytearray(args.size)
    out[0:4] = bytes.fromhex("aa55aa55")
    if len(ipl_body) != 0x2000:
        raise SystemExit("IPL body must be exactly 8 KiB")
    if 4 + len(ipl_body) > len(out):
        raise SystemExit("IPL does not fit in output image")
    out[4:4 + len(ipl_body)] = ipl_body

    hdr = struct.pack("<IIII", BOOTCODE_MAGIC, len(bootcode), 0x93c00000,
                      sum(bootcode) & 0xFFFFFFFF)
    if args.bootcode_off + 16 + len(bootcode) > len(out):
        raise SystemExit("bootcode does not fit in output image")
    out[args.bootcode_off:args.bootcode_off + 16] = hdr
    out[args.bootcode_off + 16:args.bootcode_off + 16 + len(bootcode)] = bootcode

    if args.uboot:
        ub = args.uboot.read_bytes()
        if args.uboot_off + 16 + len(ub) > len(out):
            # grow
            need = args.uboot_off + 16 + len(ub)
            out.extend(bytes(need - len(out)))
        uh = struct.pack("<IIII", BOOTCODE_MAGIC, len(ub), 0x93ce8420,
                         sum(ub) & 0xFFFFFFFF)
        out[args.uboot_off:args.uboot_off + 16] = uh
        out[args.uboot_off + 16:args.uboot_off + 16 + len(ub)] = ub

    args.output.write_bytes(out)
    print(f"wrote {args.output}: {len(out)} bytes "
          f"(ipl={len(ipl_body)}, bootcode={len(bootcode)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
