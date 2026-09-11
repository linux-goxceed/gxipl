#!/usr/bin/env python3
"""Optional GXAI second envelope for vendor gxdl file parsing.

Open UART flashers must send the shared Stage 1 once. They must not scan
payloads for ``toob`` or try both BootROM members. Concatenating the second
envelope into open-IPL Stage 2 causes EBUNDLE.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from mkboot import pack_target_catalog

HEADER_SIZE = 0x20
BODY_SIZE = 0x2000
MEMBER_SIZE = HEADER_SIZE + BODY_SIZE
GXAI_MAGIC = b"GXAI"
GXAI_VERSION = 1


def wrap_member(body: bytes, chip_id: int, extra_chip_ids=()) -> bytes:
    header = bytearray(HEADER_SIZE)
    header[0:4] = b"toob"
    struct.pack_into("<HHI", header, 4, 0x0100, chip_id, 115200)
    header[0x0C:0x20] = pack_target_catalog(extra_chip_ids)
    if len(body) != BODY_SIZE:
        raise ValueError(f"body must be {BODY_SIZE} bytes, got {len(body)}")
    return bytes(header) + body


def build_allinone(member0: bytes) -> bytes:
    if len(member0) != MEMBER_SIZE or member0[:4] != b"toob":
        raise ValueError("input must be a 0x2020-byte toob container")
    body = member0[HEADER_SIZE:]
    extras = (0x6705, 0x6616, 0x3211)
    prefix = wrap_member(body, 0x6701, extras)
    extra = wrap_member(body, 0x6705, extras)
    count = 1
    catalog_hdr = GXAI_MAGIC + struct.pack("<HH", GXAI_VERSION, count)
    catalog_size = len(catalog_hdr) + 12
    off1 = MEMBER_SIZE + catalog_size
    entry = struct.pack("<HHII", 0x6705, 0, off1, MEMBER_SIZE)
    return prefix + catalog_hdr + entry + extra


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Append a GXAI Cygnus envelope to a universal UART stub")
    parser.add_argument("input", help="gx-universal-ipl.boot (0x6701 wrapper)")
    parser.add_argument("output")
    args = parser.parse_args()
    Path(args.output).write_bytes(build_allinone(Path(args.input).read_bytes()))
    print(f"wrote {args.output}: GXAI member1 at 0x6705, same CRC-sealed body")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
