#!/usr/bin/env python3
"""Validate an ELF32 C-SKY ABIv1 image against the open IPL DDR contract."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

DDR_BASE = 0x90000000
DDR_END = 0x94000000
EM_CSKY = 252
ET_EXEC = 2
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PF_X = 1
EF_CSKY_ABIV1 = 0x10000000
EF_CSKY_ABIV2 = 0x20000000


class ElfError(ValueError):
    pass


def _in_ddr(address: int, size: int) -> bool:
    return (DDR_BASE <= address <= DDR_END and size >= 0 and
            address + size <= DDR_END and address + size <= 0xFFFFFFFF)


def validate_elf(data: bytes) -> int:
    """Validate *data* and return its entry address."""
    if len(data) < 52:
        raise ElfError("ELF image too small")
    ident = data[:16]
    if ident[:4] != b"\x7fELF" or ident[4:7] != b"\x01\x01\x01":
        raise ElfError("need little-endian ELF32")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    (etype, machine, version, entry, phoff, _shoff, flags, ehsize,
     phentsize, phnum, _shentsize, _shnum, _shstrndx) = fields
    if etype != ET_EXEC or machine != EM_CSKY or version != 1:
        raise ElfError("need an EM_CSKY ET_EXEC image")
    if flags & EF_CSKY_ABIV2 or not flags & EF_CSKY_ABIV1:
        raise ElfError("need C-SKY ABIv1")
    if ehsize < 52 or phentsize < 32 or not phnum:
        raise ElfError("invalid ELF/program header sizes")
    if phoff + phentsize * phnum > len(data):
        raise ElfError("program header table extends past file")
    if not _in_ddr(entry, 1):
        raise ElfError("entry is outside 0x90000000-0x94000000")

    saw_load = False
    entry_in_exec = False
    for index in range(phnum):
        off = phoff + index * phentsize
        ptype, poff, vaddr, _paddr, filesz, memsz, pflags, _align = \
            struct.unpack_from("<IIIIIIII", data, off)
        if ptype in (PT_DYNAMIC, PT_INTERP):
            raise ElfError("dynamic/interpreted ELF is unsupported")
        if ptype != PT_LOAD:
            continue
        saw_load = True
        if filesz > memsz or poff + filesz > len(data):
            raise ElfError("invalid PT_LOAD file bounds")
        if not _in_ddr(vaddr, memsz):
            raise ElfError("PT_LOAD is outside DDR")
        if pflags & PF_X and vaddr <= entry < vaddr + memsz:
            entry_in_exec = True
    if not saw_load or not entry_in_exec:
        raise ElfError("entry is not in an executable PT_LOAD")
    return entry


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    try:
        entry = validate_elf(args.elf.read_bytes())
    except ElfError as error:
        parser.error(str(error))
    print(f"ELF PASS entry={entry:#010x} DDR=0x90000000-0x94000000")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
