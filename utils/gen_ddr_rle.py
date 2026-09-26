#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate RLE-compressed DDR register init streams.

Parses the canonical readable ``u32`` tables out of the IPL C sources and
emits a compact byte stream plus a generated header.  The decoder lives in
``ipl/ipl_common.c`` (``ddr_apply_rle``).

Token format (one-byte tag), matching ddr_apply_rle() in ipl/ipl_common.c:
  0x00-0x7F  LIT32   tag+1 words follow, little-endian u32 (1..128)
  0x80-0x9F  ZERO    (tag-0x80)+1 zero words (1..32)
  0xA0-0xBF  REPEAT  (tag-0xA0)+1 words copied from a back-reference of
                     1..16 words (next byte), LZ77-style with overlap

The decoded stream is always byte-identical to the source table; the test
suite (tests/test_ddr_rle.py) verifies this on the host.
"""

import argparse
import pathlib
import re
import sys

ZERO_MAX = 32
REP_MAX = 32
REP_BACK_MAX = 16
LIT32_MAX = 128

SOURCES = {
    "gx6702": {
        "file": "ipl/ipl.c",
        "arrays": {"ddr_regs_0": 155, "ddr_regs_100": 26},
    },
    "gx6706": {
        "file": "ipl/gx6706_ipl.c",
        "arrays": {"gx6706_ddr_regs_0": 155, "gx6706_ddr_regs_100": 31},
    },
}


def parse_array(text: str, name: str, expected: int) -> list:
    m = re.search(
        r"\b%s\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\}\s*;" % re.escape(name),
        text,
        re.DOTALL,
    )
    if not m:
        raise SystemExit(f"array {name} not found")
    words = [int(w, 16) for w in re.findall(r"0x([0-9a-fA-F]+)u?", m.group(1))]
    if len(words) != expected:
        raise SystemExit(f"array {name}: got {len(words)} words, want {expected}")
    return words


def best_repeat(words, p):
    """Longest (back, count) repeat at p with back<=16, count>=2."""
    best = (0, 0)
    n = len(words)
    for back in range(1, min(REP_BACK_MAX, p) + 1):
        count = 0
        while p + count < n and count < REP_MAX and words[p + count] == words[p + count - back]:
            count += 1
        if count >= 2 and count > best[1]:
            best = (back, count)
    return best


def run_len(words, p, limit):
    """Length of the run of zero words at p, capped."""
    n = len(words)
    i = 0
    while p + i < n and i < limit and words[p + i] == 0:
        i += 1
    return i


def lit32_run(words, p):
    """Literal u32 run: stop at zeros and at good back-reference starts."""
    n = len(words)
    i = 0
    while p + i < n and i < LIT32_MAX and words[p + i] != 0:
        if i and best_repeat(words, p + i)[1] >= 4:
            break
        i += 1
    return i


def encode(words):
    out = bytearray()
    p = 0
    n = len(words)
    while p < n:
        z = run_len(words, p, ZERO_MAX)
        back, rep = best_repeat(words, p)
        r32 = lit32_run(words, p)

        # (cost, words_consumed, kind) — pick the best cost-per-word ratio,
        # preferring longer runs on ties to reduce token count.
        cands = []
        if z:
            cands.append((1, z, "zero"))
        if rep >= 2:
            cands.append((2, rep, "rep"))
        if r32:
            cands.append((1 + 4 * r32, r32, "lit32"))
        if not cands:
            # Isolated zero word wedged in a literal region: one-word LIT32.
            cands.append((5, 1, "lit32"))
        cost, take, kind = min(
            cands, key=lambda c: (c[0] / c[1], -c[1])
        )

        if kind == "zero":
            out.append(0x80 | (take - 1))
        elif kind == "rep":
            out.append(0xA0 | (take - 1))
            out.append(back)
        else:
            out.append(take - 1)
            for w in words[p:p + take]:
                out += (w & 0xFFFFFFFF).to_bytes(4, "little")
        p += take
    return bytes(out)


def decode(blob, total):
    """Host-side reference decoder used by the test suite."""
    words = []
    p = 0
    while p < len(blob):
        tag = blob[p]
        p += 1
        if tag < 0x80:
            count = tag + 1
            for _ in range(count):
                words.append(int.from_bytes(blob[p:p + 4], "little"))
                p += 4
        elif tag < 0xA0:
            words += [0] * ((tag - 0x80) + 1)
        else:
            count = (tag - 0xA0) + 1
            back = blob[p]
            p += 1
            if back < 1 or back > 16:
                raise ValueError("back out of range")
            for _ in range(count):
                words.append(words[len(words) - back])
    if len(words) != total:
        raise ValueError(f"decoded {len(words)} words, want {total}")
    return words


def c_array(name, data, per_line=16):
    lines = [f"static const u8 {name}[] = {{"]
    for i in range(0, len(data), per_line):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + per_line])
        lines.append(f"\t{chunk},")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--soc", required=True, choices=sorted(SOURCES))
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    spec = SOURCES[args.soc]
    text = (root / spec["file"]).read_text()

    blobs = {}
    words_by_name = {}
    for name, count in spec["arrays"].items():
        words = parse_array(text, name, count)
        words_by_name[name] = words
        blob = encode(words)
        if decode(blob, count) != words:
            raise SystemExit(f"round-trip mismatch for {name}")
        blobs[name] = blob
        print(
            f"{name}: {count * 4} B -> {len(blob)} B "
            f"({100 * len(blob) // (count * 4)}%)",
            file=sys.stderr,
        )

    names = list(spec["arrays"])
    geom_word = words_by_name[names[0]][5]
    geometry = (((geom_word >> 16) & 0x1F) - 4) >> 1

    out = [
        "/* SPDX-License-Identifier: MIT */",
        "/* Generated by utils/gen_ddr_rle.py — do not edit. */",
        "#ifndef DDR_RLE_H",
        "#define DDR_RLE_H",
        "",
        c_array("ddr_r0", blobs[names[0]]),
        "",
        c_array("ddr_r100", blobs[names[1]]),
        "",
        f"#define DDR_REGS0_GEOMETRY {geometry}u",
        "",
        "#endif",
        "",
    ]
    pathlib.Path(args.output).write_text("\n".join(out))


if __name__ == "__main__":
    main()
