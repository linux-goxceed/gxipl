#!/usr/bin/env python3
"""Patch or dump the 512-byte IPL config at the end of a .boot / body image."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

CONFIG_SIZE = 512
MAGIC = 0x47464331

FLAG_NAMES = {
    0: "verbose",
    1: "skip_usb",
    2: "skip_spi",
    3: "skip_uart",
    4: "uart_direct",
    5: "force_uart",
}


def crc16(data: bytes) -> int:
    """IPL-config checksum: sum of bytes after crc, skipping BootROM trailer."""
    total = 0
    for i, b in enumerate(data):
        abs_i = i  # data is config[8:]
        off = 8 + abs_i
        if 0x1F8 <= off < 0x1FC:
            continue
        total += b
    return total & 0xFFFF


def load_config(raw: bytes) -> dict:
    magic, ver, crc = struct.unpack_from("<IHH", raw, 0)
    flags, timeout, bc_off, ub_off, ub_max = struct.unpack_from("<IIIII", raw, 8)
    expect = crc16(raw[8:])
    return {
        "magic": magic,
        "version": ver,
        "crc": crc,
        "crc_ok": crc == expect,
        "flags": flags,
        "uart_timeout_s": timeout,
        "bootcode_flash_off": bc_off,
        "uboot_flash_off": ub_off,
        "uboot_flash_max": ub_max,
    }


def build_config(flags: int, timeout: int, bc_off: int, ub_off: int, ub_max: int) -> bytes:
    blob = bytearray(CONFIG_SIZE)
    struct.pack_into("<IHH", blob, 0, MAGIC, 1, 0)
    struct.pack_into("<IIIII", blob, 8, flags, timeout, bc_off, ub_off, ub_max)
    struct.pack_into("<H", blob, 6, crc16(blob[8:]))
    return bytes(blob)


def config_offset(image: bytes, size: str = "8k") -> int:
    if size != "8k":
        raise SystemExit(f"unsupported IPL size {size!r}; stage-1 is fixed at 8 KiB")
    body = 0x2000
    if len(image) == HEADER_SIZE + body or image[:4] == b"toob":
        return HEADER_SIZE + body - CONFIG_SIZE
    if len(image) >= body:
        # raw body
        return body - CONFIG_SIZE
    raise SystemExit(f"unrecognized image length {len(image)}")


HEADER_SIZE = 0x20


def main() -> int:
    ap = argparse.ArgumentParser(description="IPL config patcher")
    ap.add_argument("image", type=Path)
    ap.add_argument("--size", choices=("8k",), default="8k",
                    help="stage-1 body size (BootROM is fixed at 8 KiB)")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--verbose", type=int, choices=(0, 1))
    ap.add_argument("--skip-usb", type=int, choices=(0, 1))
    ap.add_argument("--skip-spi", type=int, choices=(0, 1))
    ap.add_argument("--skip-uart", type=int, choices=(0, 1))
    ap.add_argument("--uart-direct", type=int, choices=(0, 1))
    ap.add_argument("--force-uart", type=int, choices=(0, 1))
    ap.add_argument("--uart-timeout", type=int)
    ap.add_argument("-o", "--output", type=Path, help="write patched image (default: in-place)")
    args = ap.parse_args()

    data = bytearray(args.image.read_bytes())
    off = config_offset(bytes(data), args.size)
    cfg = load_config(bytes(data[off:off + CONFIG_SIZE]))

    if args.show or all(v is None for v in (
            args.verbose, args.skip_usb, args.skip_spi, args.skip_uart,
            args.uart_direct, args.force_uart, args.uart_timeout)):
        print(f"offset={off:#x} magic={cfg['magic']:#x} ver={cfg['version']} "
              f"crc_ok={cfg['crc_ok']} flags={cfg['flags']:#x}")
        for bit, name in FLAG_NAMES.items():
            print(f"  {name}: {bool(cfg['flags'] & (1 << bit))}")
        print(f"  uart_timeout_s={cfg['uart_timeout_s']}")
        print(f"  bootcode_flash_off={cfg['bootcode_flash_off']:#x}")
        print(f"  uboot_flash_off={cfg['uboot_flash_off']:#x}")
        print(f"  uboot_flash_max={cfg['uboot_flash_max']:#x}")
        if args.show and args.verbose is None:
            return 0

    flags = cfg["flags"]
    if args.verbose is not None:
        flags = (flags & ~1) | (args.verbose & 1)
    if args.skip_usb is not None:
        flags = (flags & ~(1 << 1)) | ((args.skip_usb & 1) << 1)
    if args.skip_spi is not None:
        flags = (flags & ~(1 << 2)) | ((args.skip_spi & 1) << 2)
    if args.skip_uart is not None:
        flags = (flags & ~(1 << 3)) | ((args.skip_uart & 1) << 3)
    if args.uart_direct is not None:
        flags = (flags & ~(1 << 4)) | ((args.uart_direct & 1) << 4)
    if args.force_uart is not None:
        flags = (flags & ~(1 << 5)) | ((args.force_uart & 1) << 5)

    timeout = cfg["uart_timeout_s"] if args.uart_timeout is None else args.uart_timeout
    new = build_config(flags, timeout, cfg["bootcode_flash_off"],
                       cfg["uboot_flash_off"], cfg["uboot_flash_max"])
    data[off:off + CONFIG_SIZE] = new
    # Preserve / restore legacy trailer inside the config window.
    # Trailer bytes are excluded from the IPL-config CRC (BootROM owns them).
    body_base = HEADER_SIZE if data[:4] == b"toob" else 0
    t_abs = body_base + 0x1FF8
    if t_abs + 4 <= len(data) and off <= t_abs < off + CONFIG_SIZE:
        data[t_abs:t_abs + 4] = bytes.fromhex("33dea189")
        struct.pack_into("<H", data, off + 6, crc16(data[off + 8:off + CONFIG_SIZE]))

    out = args.output or args.image
    out.write_bytes(data)
    print(f"wrote config to {out} @{off:#x} flags={flags:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
