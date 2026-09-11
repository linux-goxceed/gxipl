#!/usr/bin/env python3
"""Build a BOOT/TABLE pair. Defaults: 64 KiB for SOC=gx6702, 128 KiB for SOC=gx6706.

Those sizes are this tree's packaging defaults, not a silicon limit. Vendor
images of either SoC can use the other BOOT size depending on SDK config.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

from mk_flash_probe64 import patch_table_for_boot
from mkboot import LEGACY_TRAILER, TRAILER_OFF, bootrom_stage1_crc


ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent
BOOTCODE_MAGIC = 0x43425847
BOOTCODE_ENTRY = 0x93C00000
UBOOT_ENTRY = 0x93CE8420
SOCS = {
    "gx6702": {"boot_size": 0x10000, "define": "SOC_GX6702"},
    "gx6706": {"boot_size": 0x20000, "define": "SOC_GX6706"},
}


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.check_call(command, cwd=ROOT)


def gxbc(payload: bytes, entry: int) -> bytes:
    return struct.pack("<IIII", BOOTCODE_MAGIC, len(payload), entry,
                       sum(payload) & 0xFFFFFFFF) + payload


def seal_ipl_container(container: bytes, bootcode_off: int, uboot_off: int,
                       crc_trailer: bool) -> bytes:
    """Return a configured IPL container without modifying the build artifact."""
    data = bytearray(container)
    if len(data) != 0x2020 or data[:4] != b"toob":
        raise SystemExit("IPL container must be exactly 8224 bytes")
    cfg_off = 0x20 + 0x1E00
    struct.pack_into("<III", data, cfg_off + 16, bootcode_off, uboot_off, 0)
    checksum = 0
    for index in range(8, 512):
        if 0x1F8 <= index < 0x1FC:
            continue
        checksum += data[cfg_off + index]
    struct.pack_into("<H", data, cfg_off + 6, checksum & 0xFFFF)
    trailer = 0x20 + TRAILER_OFF
    if crc_trailer:
        body = data[0x20:trailer]
        struct.pack_into("<I", data, trailer, bootrom_stage1_crc(bytes(body)))
    else:
        data[trailer:trailer + 4] = LEGACY_TRAILER
    return bytes(data)


def build_tiny(cross: str, soc: str) -> bytes:
    stem = f"{soc}-tiny-flash"
    obj = ROOT / "build" / stem / "tiny_flash_test.o"
    elf = ROOT / f"{stem}.elf"
    binary = ROOT / f"{stem}.bin"
    obj.parent.mkdir(parents=True, exist_ok=True)
    run([
        cross + "gcc", "-EL", "-mcpu=ck610", "-Os", "-std=c99",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-fomit-frame-pointer", "-nostdlib", "-Wall", "-Wextra",
        "-ffunction-sections", "-fdata-sections", f"-D{SOCS[soc]['define']}=1",
        "-Iinclude", "-Ibootcode", "-c", "bootcode/tiny_flash_test.c",
        "-o", str(obj),
    ])
    run([
        cross + "ld", "-EL", "-nostdlib", "--gc-sections",
        "-T", "ld/linker-bootcode.ld", "-o", str(elf), str(obj),
    ])
    run([cross + "objcopy", "-O", "binary", str(elf), str(binary)])
    return binary.read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--soc", choices=sorted(SOCS), default="gx6702")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--probe", action="store_true", help="use the tiny UART probe")
    mode.add_argument("--full-bootcode", action="store_true")
    mode.add_argument("--uboot", type=Path)
    parser.add_argument("--cross-compile", default=None)
    parser.add_argument("--table-in", type=Path)
    parser.add_argument("--table-out", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()

    cross = args.cross_compile or os.environ.get("CROSS_COMPILE", "csky-linux-")
    soc = args.soc
    if args.table_in is None:
        args.table_in = ((REPO.parent / "extracted_partitions" / "TABLE.bin")
                         if soc == "gx6702" else
                         (REPO / "extracted_partitions" / "TABLE.bin"))
    output = args.output or ROOT / f"BOOT-{soc}.bin"
    table_output = args.table_out or ROOT / f"TABLE-{soc}.bin"

    run(["make", f"SOC={soc}", "all", f"CROSS_COMPILE={cross}"])
    ipl_path = ROOT / f"{soc}-ipl.boot"
    bootcode_path = ROOT / f"{soc}-bootcode.bin"
    ipl = seal_ipl_container(
        ipl_path.read_bytes(), 0x5000 if args.probe else 0x4000,
        0x10000 if args.uboot else 0, soc == "gx6706")

    payload = build_tiny(cross, soc) if args.probe else bootcode_path.read_bytes()
    bootcode_off = 0x5000 if args.probe else 0x4000
    boot = bytearray(SOCS[soc]["boot_size"])
    boot[0:4] = bytes.fromhex("aa55aa55")
    body = ipl[0x20:0x2020]
    if len(body) != 0x2000:
        raise SystemExit("invalid IPL container body")
    boot[4:4 + len(body)] = body
    record = gxbc(payload, BOOTCODE_ENTRY)
    if bootcode_off + len(record) > len(boot):
        raise SystemExit("bootcode does not fit in BOOT partition")
    boot[bootcode_off:bootcode_off + len(record)] = record

    if args.uboot:
        uboot = args.uboot.read_bytes()
        record = gxbc(uboot, UBOOT_ENTRY)
        needed = 0x10000 + len(record)
        if needed > len(boot):
            grown = (needed + 0xffff) & ~0xffff
            boot.extend(bytes(grown - len(boot)))
        boot[0x10000:0x10000 + len(record)] = record

    output.write_bytes(boot)
    table, info = patch_table_for_boot(args.table_in.read_bytes(), bytes(boot))
    table_output.write_bytes(table)
    print(f"wrote {output}: {len(boot)} bytes; bootcode @{bootcode_off:#x}")
    print(f"wrote {table_output}: BOOT total={info['boot_total']:#x}, "
          f"TABLE CRC={info['table_crc']:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
