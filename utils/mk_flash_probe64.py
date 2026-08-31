#!/usr/bin/env python3
"""Build a flash BOOT image: 8 KiB IPL + SPI bootcode [+ U-Boot] + matching TABLE.

Flash BootROM maps BOOT.bin[4:0x2000] -> SRAM and CRC32-checks body[:0x1FF8]
against the trailer at 0x1FF8.  GxLoader partition CRCs in TABLE are separate
(not required for BootROM to run open IPL); this tool still patches TABLE so
listings stay consistent.

GxLoader packs IPL + bootloader in one BOOT partition; --uboot does the same
(open IPL + bootcode + GXBC U-Boot), growing BOOT and rewriting TABLE starts.

Modes:
  (default) tiny FLASHBC probe bootcode at 0x5000
  --full-bootcode  verbose DDR bootcode at 0x4000
  --uboot FILE     pack U-Boot GXBC into BOOT (implies --full-bootcode)

Outputs BOOT + TABLE images ready for serialdown.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # gxipl/
REPO = ROOT.parent  # re-boot/
BOOTCODE_MAGIC = 0x43425847
UBOOT_ENTRY = 0x93CE8420
IPL_BODY = 0x2000
BOOT_CRC_OFF = 0x19D
TABLE_CRC_OFF = 0x1FC
ERASE_SIZE = 64 * 1024
DEFAULT_UBOOT_OFF = 0x10000
DEFAULT_TABLE = REPO.parent / "extracted_partitions" / "TABLE.bin"
DEFAULT_STOCK_BOOT = REPO / "BOOT.bin"
DEFAULT_UBOOT = REPO / "u-boot" / "u-boot.bin"

_CRC_TABLE: list[int] | None = None


def _crc_table() -> list[int]:
    global _CRC_TABLE
    if _CRC_TABLE is None:
        poly = 0x04C11DB7
        tbl = []
        for i in range(256):
            c = i << 24
            for _ in range(8):
                if c & 0x80000000:
                    c = ((c << 1) ^ poly) & 0xFFFFFFFF
                else:
                    c = (c << 1) & 0xFFFFFFFF
            tbl.append(c)
        _CRC_TABLE = tbl
    return _CRC_TABLE


def bootrom_stage1_crc(data: bytes) -> int:
    crc = 0xFFFFFFFF
    tbl = _crc_table()
    for b in data:
        crc = (tbl[((crc >> 24) ^ b) & 0xFF] ^ ((crc << 8) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return crc


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=ROOT)


def _align_up(n: int, a: int) -> int:
    return (n + a - 1) // a * a


def _parse_parts(table: bytearray) -> list[dict]:
    count = table[4]
    parts: list[dict] = []
    pos = 5
    for _ in range(count):
        raw_name = bytes(table[pos:pos + 8])
        name = raw_name.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        parts.append({
            "pos": pos,
            "name": name,
            "total": int.from_bytes(table[pos + 8:pos + 12], "big"),
            "used": int.from_bytes(table[pos + 12:pos + 16], "big"),
            "start": int.from_bytes(table[pos + 16:pos + 20], "big"),
        })
        pos += 24
    return parts


def _write_parts(table: bytearray, parts: list[dict]) -> None:
    expect = 0
    for p in parts:
        pos = p["pos"]
        p["start"] = expect
        table[pos + 8:pos + 12] = p["total"].to_bytes(4, "big")
        table[pos + 12:pos + 16] = p["used"].to_bytes(4, "big")
        table[pos + 16:pos + 20] = p["start"].to_bytes(4, "big")
        expect += p["total"]


def patch_table_for_boot(table: bytes, boot: bytes) -> tuple[bytes, dict]:
    """Update BOOT used/CRC; grow BOOT (+shrink KERNEL) when image exceeds it."""
    if len(table) < 0x200:
        raise SystemExit(f"TABLE too small ({len(table)})")
    out = bytearray(table[:0x200])
    parts = _parse_parts(out)
    if not parts or parts[0]["name"] != "BOOT":
        raise SystemExit("BOOT must be the first TABLE partition")

    need = len(boot)
    old_total = parts[0]["total"]
    new_total = max(old_total, _align_up(need, ERASE_SIZE))
    delta = new_total - old_total
    parts[0]["total"] = new_total
    parts[0]["used"] = need if need <= new_total else new_total

    if delta:
        shrink_left = delta
        for p in parts[1:]:
            if p["name"] == "TABLE":
                continue
            if p["name"] != "KERNEL":
                continue
            take = min(shrink_left, p["total"] // 2)
            if p["total"] - take < ERASE_SIZE:
                take = max(0, p["total"] - ERASE_SIZE)
            p["total"] -= take
            if p["used"] > p["total"]:
                p["used"] = p["total"]
            shrink_left -= take
            break
        if shrink_left:
            raise SystemExit(
                f"cannot grow BOOT by {delta:#x}: KERNEL too small to shrink"
            )

    _write_parts(out, parts)

    used = parts[0]["used"]
    window = bytearray(boot[:used])
    if len(window) < used:
        window.extend(b"\xff" * (used - len(window)))
    new_boot_crc = zlib.crc32(bytes(window)) & 0xFFFFFFFF
    old_boot_crc = int.from_bytes(out[BOOT_CRC_OFF:BOOT_CRC_OFF + 4], "big")
    out[BOOT_CRC_OFF:BOOT_CRC_OFF + 4] = new_boot_crc.to_bytes(4, "big")
    table_crc = zlib.crc32(bytes(out[:TABLE_CRC_OFF])) & 0xFFFFFFFF
    out[TABLE_CRC_OFF:TABLE_CRC_OFF + 4] = table_crc.to_bytes(4, "big")
    return bytes(out), {
        "boot_used": used,
        "boot_total": parts[0]["total"],
        "old_boot_crc": old_boot_crc,
        "new_boot_crc": new_boot_crc,
        "table_crc": table_crc,
        "table_off": parts[1]["start"] if len(parts) > 1 else parts[0]["total"],
        "delta": delta,
    }


def seal_ipl_boot(ipl_boot: Path, bootcode_off: int, uboot_off: int = 0) -> None:
    """Patch bootcode/U-Boot offsets, IPL-config CRC, and BootROM stage-1 trailer.

    Config CRC skips the 4 trailer bytes at body 0x1FF8 (see ipl_config.c), so
    the BootROM CRC32 and IPL-config checksum no longer fight each other.
    """
    data = bytearray(ipl_boot.read_bytes())
    cfg_off = 0x20 + IPL_BODY - 512
    t_abs = 0x20 + 0x1FF8
    struct.pack_into("<I", data, cfg_off + 16, bootcode_off)
    struct.pack_into("<I", data, cfg_off + 20, uboot_off)
    struct.pack_into("<I", data, cfg_off + 24, 0)

    def cfg_sum() -> int:
        s = 0
        for i in range(8, 512):
            if 0x1F8 <= i < 0x1FC:
                continue
            s += data[cfg_off + i]
        return s & 0xFFFF

    struct.pack_into("<H", data, cfg_off + 6, cfg_sum())
    body = memoryview(data)[0x20:0x20 + IPL_BODY]
    stage_crc = bootrom_stage1_crc(bytes(body[:0x1FF8]))
    struct.pack_into("<I", data, t_abs, stage_crc)
    ipl_boot.write_bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cross-compile",
        default=os.environ.get("CROSS_COMPILE", "csky-linux-"),
    )
    ap.add_argument(
        "--full-bootcode",
        action="store_true",
        help="pack verbose gx6702-bootcode.bin at 0x4000 (default: tiny FLASHBC @0x5000)",
    )
    ap.add_argument(
        "--uboot",
        type=Path,
        nargs="?",
        const=DEFAULT_UBOOT,
        default=None,
        help="pack GXBC U-Boot into BOOT (GxLoader-style; implies --full-bootcode)",
    )
    ap.add_argument("--uboot-off", type=lambda x: int(x, 0), default=DEFAULT_UBOOT_OFF)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--table-in", type=Path, default=DEFAULT_TABLE)
    ap.add_argument("--table-out", type=Path, default=None)
    ap.add_argument("--stock-base", type=Path, default=DEFAULT_STOCK_BOOT)
    ap.add_argument("--no-stock-base", action="store_true")
    args = ap.parse_args()
    cc = args.cross_compile

    if args.uboot is not None:
        args.full_bootcode = True

    if args.full_bootcode:
        bootcode_off = 0x4000
        if args.output is None:
            args.output = ROOT / (
                "BOOT-flash-uboot.bin" if args.uboot else "BOOT-flash-verbose64.bin"
            )
        if args.table_out is None:
            args.table_out = ROOT / (
                "TABLE-flash-uboot.bin" if args.uboot else "TABLE-flash-verbose64.bin"
            )
    else:
        bootcode_off = 0x5000
        if args.output is None:
            args.output = ROOT / "BOOT-flash-probe64.bin"
        if args.table_out is None:
            args.table_out = ROOT / "TABLE-flash-probe64.bin"

    run(["make", "clean", f"CROSS_COMPILE={cc}"])
    if args.full_bootcode:
        run(["make", "all", f"CROSS_COMPILE={cc}"])
    else:
        run(["make", "ipl", f"CROSS_COMPILE={cc}"])

    ipl_boot = ROOT / "gx6702-ipl.boot"
    run([
        sys.executable, str(ROOT / "utils" / "iplcfg.py"), str(ipl_boot),
        "--size", "8k",
        "--uart-direct", "0",
        "--verbose", "1",
    ])
    uboot_off = args.uboot_off if args.uboot else 0
    seal_ipl_boot(ipl_boot, bootcode_off, uboot_off)

    if args.full_bootcode:
        bc_path = ROOT / "gx6702-bootcode.bin"
        if not bc_path.is_file():
            raise SystemExit("gx6702-bootcode.bin missing after make all")
        payload = bc_path.read_bytes()
        payload_label = "full bootcode"
    else:
        run([
            f"{cc}gcc", "-EL", "-mcpu=ck610", "-Os", "-std=c99",
            "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
            "-fomit-frame-pointer", "-nostdlib", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections",
            "-Iinclude", "-Ibootcode",
            "-c", "bootcode/tiny_flash_test.c", "-o", "bootcode/tiny_flash_test.o",
        ])
        run([
            f"{cc}ld", "-EL", "-nostdlib", "--gc-sections",
            "-T", "linker-bootcode.ld",
            "-o", "gx6702-tiny-flash.elf",
            "bootcode/tiny_flash_test.o",
        ])
        run([
            f"{cc}objcopy", "-O", "binary",
            "gx6702-tiny-flash.elf", "gx6702-tiny-flash.bin",
        ])
        payload = (ROOT / "gx6702-tiny-flash.bin").read_bytes()
        payload_label = "tiny FLASHBC"

    ipl = ipl_boot.read_bytes()
    ipl_body = ipl[0x20:0x20 + IPL_BODY]

    boot_size = 64 * 1024
    ub = b""
    if args.uboot:
        if not args.uboot.is_file():
            raise SystemExit(f"U-Boot not found: {args.uboot}")
        ub = args.uboot.read_bytes()
        boot_size = _align_up(args.uboot_off + 16 + len(ub), ERASE_SIZE)

    if args.no_stock_base or not args.stock_base.is_file() or boot_size != 64 * 1024:
        out = bytearray(boot_size)
        out[0:4] = bytes.fromhex("aa55aa55")
    else:
        out = bytearray(args.stock_base.read_bytes())
        if len(out) != 64 * 1024:
            raise SystemExit(f"stock BOOT must be 64 KiB, got {len(out)}")
        if out[0:4] != bytes.fromhex("aa55aa55"):
            raise SystemExit("stock BOOT missing AA55AA55")

    if len(out) < boot_size:
        out.extend(b"\xff" * (boot_size - len(out)))

    out[4:4 + len(ipl_body)] = ipl_body

    hdr = struct.pack(
        "<IIII",
        BOOTCODE_MAGIC,
        len(payload),
        0x93C00000,
        sum(payload) & 0xFFFFFFFF,
    )
    if bootcode_off + 16 + len(payload) > len(out):
        raise SystemExit(f"{payload_label} does not fit at {bootcode_off:#x}")
    out[bootcode_off:bootcode_off + 16] = hdr
    out[bootcode_off + 16:bootcode_off + 16 + len(payload)] = payload

    if args.uboot:
        uh = struct.pack(
            "<IIII",
            BOOTCODE_MAGIC,
            len(ub),
            UBOOT_ENTRY,
            sum(ub) & 0xFFFFFFFF,
        )
        if args.uboot_off + 16 + len(ub) > len(out):
            raise SystemExit("U-Boot does not fit in grown BOOT image")
        out[args.uboot_off:args.uboot_off + 16] = uh
        out[args.uboot_off + 16:args.uboot_off + 16 + len(ub)] = ub

    args.output.write_bytes(out)

    body = bytes(out[4:4 + IPL_BODY])
    expect = bootrom_stage1_crc(body[:0x1FF8])
    got = struct.unpack_from("<I", body, 0x1FF8)[0]
    assert got == expect, f"flash trailer {got:08x} != BootROM CRC {expect:08x}"
    cfg = body[0x1E00:0x2000]
    magic, ver, crc16 = struct.unpack_from("<IHH", cfg, 0)
    assert magic == 0x47464331 and ver == 1
    cfg_sum = sum(cfg[i] for i in range(8, 512) if not (0x1F8 <= i < 0x1FC)) & 0xFFFF
    assert cfg_sum == crc16
    flags, _, bc_off, ub_cfg = struct.unpack_from("<IIII", cfg, 8)
    assert not (flags & 0x10), "uart_direct must be clear for SPI bootcode"
    assert flags & 1, "verbose should be set"
    assert bc_off == bootcode_off
    if args.uboot:
        assert ub_cfg == args.uboot_off

    if not args.table_in.is_file():
        raise SystemExit(f"TABLE not found: {args.table_in}")
    table_out, info = patch_table_for_boot(args.table_in.read_bytes(), bytes(out))
    args.table_out.write_bytes(table_out)

    print(f"wrote {args.output}: {len(out)} bytes")
    print(f"  IPL body:     0x4..0x{4 + IPL_BODY:x} (8 KiB flash window, verbose)")
    print(f"  BootROM trailer@0x1FF8 {body[0x1FF8:0x1FFC].hex()}")
    print(f"  {payload_label} @0x{bootcode_off:x} ({len(payload)} bytes + GXBC)")
    if args.uboot:
        print(f"  U-Boot GXBC @0x{args.uboot_off:x} ({len(ub)} bytes)")
        print(f"  BOOT TOTAL {info['boot_total']:#x} (TABLE moves to {info['table_off']:#x})")
    print(
        f"wrote {args.table_out}: BOOT CRC "
        f"{info['old_boot_crc']:08X} -> {info['new_boot_crc']:08X}, "
        f"TABLE CRC {info['table_crc']:08X}"
    )
    print("Flash BOOT (required). TABLE optional for open IPL; recommended for GxLoader CRC map.")
    if args.uboot:
        print("Cold-boot expect: banner + Reading: BOOT (…size NKiB) + Jumping to U-Boot")
        print("Note: grown BOOT relocates TABLE — flash BOOT then new TABLE; re-flash")
        print("      LOGO/KERNEL/… if their starts moved (or install via U-Boot sf).")
    elif args.full_bootcode:
        print("Cold-boot expect: banner + Reading: BOOT (64KiB) + UART fallback")
        print("Then: gxupload_smoke.py --uboot-only --uboot ../u-boot/u-boot.bin -d /dev/ttyUSB0")
        print("Or: mk_flash_probe64.py --uboot  # pack IPL+U-Boot into one BOOT")
    else:
        print("Cold-boot expect: IRUN + FLASHBC + GET")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
