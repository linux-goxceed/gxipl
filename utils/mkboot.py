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
    "gx6702": {"chip_id": 0x6701, "crc_trailer": False, "extra_chip_ids": ()},
    "gx6706": {"chip_id": 0x6705, "crc_trailer": True, "extra_chip_ids": ()},
    "universal": {
        "chip_id": 0x6701,
        "crc_trailer": True,
        # Same 8 KiB Stage 1 window. 0x6612 is 16 KiB and is not listed.
        "extra_chip_ids": (0x6705, 0x6616, 0x3211),
    },
}

GXMT_MAGIC = b"GXMT"
GXMT_VERSION = 1
GXMT_MAX_EXTRA = 6


def pack_target_catalog(extra_chip_ids: tuple[int, ...] | list[int]) -> bytes:
    """Pack extra chip IDs into toob[0x0C:0x20]. Empty input keeps zeros."""
    blob = bytearray(20)
    ids = [int(chip) & 0xFFFF for chip in extra_chip_ids if chip]
    if not ids:
        return bytes(blob)
    if len(ids) > GXMT_MAX_EXTRA:
        raise ValueError(f"at most {GXMT_MAX_EXTRA} extra chip IDs fit in the header")
    blob[0:4] = GXMT_MAGIC
    blob[4] = GXMT_VERSION
    blob[5] = len(ids)
    for index, chip in enumerate(ids):
        struct.pack_into("<H", blob, 6 + index * 2, chip)
    return bytes(blob)


def parse_target_catalog(image: bytes) -> list[int] | None:
    """Return extra chip IDs, or None when the reserved field is not GXMT."""
    if len(image) < 0x20:
        return None
    reserved = image[0x0C:0x20]
    if reserved[0:4] != GXMT_MAGIC:
        return None
    if reserved[4] != GXMT_VERSION:
        return None
    count = reserved[5]
    if count > GXMT_MAX_EXTRA:
        return None
    return [struct.unpack_from("<H", reserved, 6 + index * 2)[0]
            for index in range(count)]


def header_supported_chip_ids(image: bytes) -> list[int]:
    """Primary chip ID at +6, then unique GXMT extras."""
    if len(image) < 8 or image[:4] != b"toob":
        return []
    primary = struct.unpack_from("<H", image, 6)[0]
    ids = [primary]
    extras = parse_target_catalog(image)
    if extras:
        for chip in extras:
            if chip not in ids:
                ids.append(chip)
    return ids


def chip_id_arg(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid chip ID {text!r}") from exc
    if value <= 0 or value > 0xFFFF:
        raise argparse.ArgumentTypeError(f"chip ID out of range: {text}")
    return value


def resolve_extra_chip_ids(soc: dict, extra_chip_ids=None, no_extra_chip_ids: bool = False):
    """SoC default catalog, or CLI extras. Offset 6 is not stored in GXMT."""
    if no_extra_chip_ids:
        return ()
    if extra_chip_ids:
        ids = extra_chip_ids
    else:
        ids = soc.get("extra_chip_ids") or ()
    primary = int(soc["chip_id"]) & 0xFFFF
    out = []
    for chip in ids:
        chip = int(chip) & 0xFFFF
        if chip and chip != primary and chip not in out:
            out.append(chip)
    return tuple(out)


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
    parser.add_argument("--extra-chip-id", action="append", type=chip_id_arg,
                        dest="extra_chip_ids", metavar="ID",
                        help="GXMT extra chip ID (repeatable). Replaces the SoC default catalog")
    parser.add_argument("--no-extra-chip-ids", action="store_true",
                        help="leave toob[0x0C:0x20] zero")
    args = parser.parse_args()
    if args.no_extra_chip_ids and args.extra_chip_ids:
        parser.error("--no-extra-chip-ids conflicts with --extra-chip-id")

    payload = Path(args.input).read_bytes()
    if len(payload) > CODE_END:
        parser.error(f"IPL is {len(payload)} bytes; limit is {CODE_END} "
                     f"(8 KiB body minus 512-byte config)")

    header = bytearray(HEADER_SIZE)
    header[0:4] = b"toob"
    soc = SOCS[args.soc]
    extras = resolve_extra_chip_ids(soc, args.extra_chip_ids, args.no_extra_chip_ids)
    struct.pack_into("<HHI", header, 4, 0x0100, soc["chip_id"], 115200)
    header[0x0C:0x20] = pack_target_catalog(extras)

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
    extra_note = f", extra IDs {', '.join(f'0x{c:04x}' for c in extras)}" if extras else ""
    print(f"wrote {args.output}: {args.soc} IPL {len(payload)} bytes, body 0x{BODY_SIZE:x}, "
          f"config @{cfg_off:#x}, container {HEADER_SIZE + BODY_SIZE} bytes{extra_note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
