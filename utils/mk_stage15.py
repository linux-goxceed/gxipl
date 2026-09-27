#!/usr/bin/env python3
"""Wrap a bootcode binary into a stage-1.5 USB image.

The stage-1 IPL mounts a FAT16/FAT32 stick and streams one small flat binary
to BOOTCODE_ENTRY before jumping to it (see ipl/fat/stage15_usb.c).  The
binary it expects is the ordinary GXBC bootcode image, the same one that
normally lives in flash at offset 0x4000 -- so the USB-resident and
flash-resident paths converge on an identical stage 2 and nothing downstream
has to care where it came from.

Why a flat binary and not an ELF: the IPL would otherwise have to carry
bootcode/elf.c (221 lines of validation with 26 error codes) to relocate
PT_LOAD segments.  The 16-byte header already carries a magic, a length, an
entry point and a checksum, which is all a stage-2 loader actually needs.

Output layout, identical to the flash-resident form built by splice_boot.py:

    offset 0x00  struct bootcode_hdr { magic, size, entry, checksum }
    offset 0x10  size bytes of raw bootcode payload

Constraints imposed by Petit FatFs, not by this script:

  * The filename must be a valid 8.3 name (BOOT6702.BIN / BOOT6706.BIN).
    PFF has no long-filename support and silently truncates a longer name
    into a path that cannot exist.
  * The stick must carry a partition table.  pf_mount() is MBR-only in this
    tree; a super-floppy will not mount.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

BOOTCODE_MAGIC = 0x43425847          # 'GXBC' little-endian
HDR_SIZE = 16                         # sizeof(struct bootcode_hdr)

# Must match include/gx_hw.h.  The stage-1 loader refuses anything larger,
# and streams the payload to this absolute address before jumping.
BOOTCODE_ENTRY = 0x93C00000
BOOTCODE_MAX_SIZE = 512 * 1024

# Exact 8.3 names the IPL probes, in stage15_usb.c order.  PFF would truncate
# anything longer into a name that cannot exist, so these are not cosmetic.
SOC_FILENAMES = {
    "gx6702": "BOOT6702.BIN",
    "gx6706": "BOOT6706.BIN",
}

VALID_SOCS = tuple(sorted(SOC_FILENAMES))


def pack_stage15(payload: bytes, entry: int = BOOTCODE_ENTRY) -> bytes:
    """Return header + payload, checksummed exactly as the IPL recomputes it.

    The checksum is a plain 32-bit sum over the PAYLOAD ONLY -- byte-for-byte
    the same convention as try_spi_bootcode() in ipl_common.c and
    splice_boot.py, so the same bootcode file is valid in all three places.
    """
    if not payload:
        raise ValueError("empty bootcode payload")
    if len(payload) > BOOTCODE_MAX_SIZE:
        raise ValueError(
            f"bootcode is {len(payload)} bytes; IPL accepts at most "
            f"{BOOTCODE_MAX_SIZE}")

    checksum = sum(payload) & 0xFFFFFFFF
    header = struct.pack("<IIII", BOOTCODE_MAGIC, len(payload), entry, checksum)
    return header + payload


def checksum_matches_loader(image: bytes) -> bool:
    """True when the IPL's stage15_try() would accept this image verbatim."""
    magic, size, _entry, checksum = struct.unpack("<IIII", image[:HDR_SIZE])
    payload = image[HDR_SIZE:HDR_SIZE + size]
    return (magic == BOOTCODE_MAGIC
            and len(payload) == size
            and (sum(payload) & 0xFFFFFFFF) == checksum)


def verify(image: bytes, entry: int = BOOTCODE_ENTRY) -> tuple[int, int]:
    """Re-derive the header the IPL will compute.  Returns (size, checksum)."""
    if len(image) < HDR_SIZE:
        raise ValueError("image shorter than the header")
    magic, size, hdr_entry, checksum = struct.unpack("<IIII", image[:HDR_SIZE])
    payload = image[HDR_SIZE:HDR_SIZE + size]
    if magic != BOOTCODE_MAGIC:
        raise ValueError(f"bad magic {magic:#010x}, expected {BOOTCODE_MAGIC:#010x}")
    if len(payload) != size:
        raise ValueError(f"header claims {size} payload bytes, have {len(payload)}")
    recomputed = (sum(payload)) & 0xFFFFFFFF
    if recomputed != checksum:
        raise ValueError(
            f"checksum {checksum:#010x} does not match the IPL's "
            f"{recomputed:#010x}")
    if not (BOOTCODE_ENTRY <= hdr_entry < BOOTCODE_ENTRY + BOOTCODE_MAX_SIZE):
        raise ValueError(
            f"entry {hdr_entry:#010x} is outside the stage-2 window")
    return size, checksum


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Build a stage-1.5 USB boot image (BOOT6702.BIN / "
                    "BOOT6706.BIN).")
    ap.add_argument("bootcode", type=Path,
                    help="raw SoC-specific bootcode binary "
                         "(<soc>-bootcode.bin)")
    ap.add_argument("-o", "--output", type=Path, required=True,
                    help="output image to place on the USB stick")
    ap.add_argument("--soc", choices=VALID_SOCS,
                    help="print the 8.3 name the IPL probes for this SoC "
                         "and check it against --output")
    ap.add_argument("--entry", type=lambda x: int(x, 0), default=BOOTCODE_ENTRY,
                    help=f"stage-2 entry point (default {BOOTCODE_ENTRY:#x})")
    args = ap.parse_args()

    payload = args.bootcode.read_bytes()
    try:
        image = pack_stage15(payload, args.entry)
        size, checksum = verify(image, args.entry)
    except ValueError as exc:
        print(f"mk_stage15.py: error: {exc}", file=sys.stderr)
        return 1

    args.output.write_bytes(image)

    if args.soc:
        wanted = SOC_FILENAMES[args.soc]
        actual = args.output.name
        if actual != wanted:
            # Not fatal: the caller may be writing somewhere else on purpose.
            # But a wrong 8.3 name is the single most likely reason a stick
            # silently fails to boot, so say so loudly.
            print(f"warning: {args.soc} IPL probes for {wanted!r} but this "
                  f"file is {actual!r}; it will not be found", file=sys.stderr)

    print(f"wrote {args.output}: {len(image)} bytes "
          f"(header {HDR_SIZE} + payload {size}), "
          f"entry {args.entry:#x}, checksum {checksum:#010x}")
    if args.soc:
        print(f"place it on a FAT16/FAT32 MBR-partitioned stick as "
              f"{SOC_FILENAMES[args.soc]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
