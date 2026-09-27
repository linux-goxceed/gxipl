#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the packed cygnus route/field tables in usb_msc_min.c are lossless.

Parses the ORIGINAL (reference) tables and the PACKED tables straight out of
the C source, expands the packed form, replays both through an exact model of
cygnus_usb_clocks(), and diffs every register write.

A plain table roundtrip is NOT sufficient and this test exists because of
that: an earlier encoding used 0 as the "no gate_mask" sentinel, which
collides with a real shift index of 0 and silently dropped a gate write for
gate_mask == 1.  Only the register-write diff exposed it.  In USB PHY pin
config a silently dropped write is an intermittent hardware bug, not a
clean failure, so keep this test.

Run directly for a report, or via pytest.
"""
import re
import pathlib
import sys

B = lambda n: 1 << n
B30, B31 = B(30), B(31)
TARGET = {1: 0xa030a024, 2: 0xa030a178, 3: 0xa030a17c}
GATE = {1: 0xa030a170, 2: 0xa030a174}
NO_GATE = 0xff

# ---- reference tables, transcribed from the pre-packing source ----
# (index, gate, value, gate_mask)
REF_ROUTES = [(1,1,0x05555555,0x1),(2,1,0x05555555,0x2),(3,1,0x15555555,0x200),
              (4,1,0x0ccccccc,0x400),(5,1,0x0ccccccc,0x800),(6,1,0x10000000,0x1000),
              (7,1,0x0ccccccc,0x2000),(8,1,0x09249249,0x4000),(9,1,0x05d1745d,0x8000),
              (12,2,0x0ccccccc,0x200),(13,2,0x0aaaaaaa,0x80000),(14,2,0x08000000,0x8),
              (16,2,0x10000000,0x1),(17,2,0x0ccccccc,0x800),(18,2,0x10000000,0x400)]
REF_FIELDS = [(1,2,0xff800000,0x80000000,0x40000000,0x0a800000,0),
              (1,1,0x000ff000,0x00080000,0x00040000,0x0002b000,0x10),
              (1,1,0x000000ff,0x00000080,0x00000040,0x00000007,0x08),
              (2,1,0xff000000,0x80000000,0x40000000,0x0e000000,0x80000),
              (2,1,0x00ff0000,0x00800000,0x00400000,0x00050000,0x200000),
              (2,1,0x0000ff00,0x00008000,0x00004000,0x00000900,0x400000),
              (3,1,0xff000000,0x80000000,0x40000000,0x03000000,0x08000000),
              (3,2,0x00ff0000,0x00800000,0x00400000,0x002b0000,0x20000000),
              (3,1,0x00000078,0x00000040,0x00000040,0x00000038,0),
              (3,1,0x00000007,0,0,0x00000007,0)]

def high_bit(v):
    return (1 << (v.bit_length() - 1)) if v else 0

def parse_c(path):
    """Pull the packed tables + side tables out of the C source."""
    s = pathlib.Path(path).read_text()

    def block(name):
        m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\n\};", s, re.S)
        return m.group(1) if m else None

    routes = []
    rb = block("cygnus_usb_routes")
    for m in re.finditer(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+|CYGNUS_NO_GATE)\s*,"
                         r"\s*(0x[0-9a-f]+)u\s*\}", rb):
        idx, gate, gs, val = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4), 16)
        gshift = NO_GATE if gs == "CYGNUS_NO_GATE" else int(gs)
        routes.append((idx, gate, 0 if gshift == NO_GATE else B(gshift), val))

    fields = []
    fb = block("cygnus_usb_fields")
    for m in re.finditer(
            r"\{\s*\((\d+)\s*<<\s*4\)\s*\|\s*(\d+)\s*,\s*(\d+|CYGNUS_NO_GATE)\s*,\s*"
            r"([^,]*?)\s*,\s*(\d+|CYGNUS_NO_GATE)\s*,\s*"
            r"(0x[0-9a-f]+)u\s*,\s*(0x[0-9a-f]+)u\s*\}", fb):
        t, g, gs, fl, fsh, cl, val = (int(m.group(1)), int(m.group(2)), m.group(3),
                                      m.group(4), m.group(5), int(m.group(6), 16),
                                      int(m.group(7), 16))
        gshift = NO_GATE if gs == "CYGNUS_NO_GATE" else int(gs)
        fshift = NO_GATE if fsh == "CYGNUS_NO_GATE" else int(fsh)
        flags = (1 if "OVR_FIRST" in fl else 0) | (2 if "OVR_SECOND" in fl else 0)
        fields.append((t, g, gshift, flags, fshift, cl, val))

    of = re.search(r"cygnus_first_ovr\[1\]\s*=\s*\{\s*(0x[0-9a-f]+)u", s)
    os_ = re.search(r"cygnus_second_ovr\[2\]\s*=\s*\{([^}]*)\}", s)
    first_ovr = [int(of.group(1), 16)] if of else []
    second_ovr = [int(x, 16) for x in re.findall(r"0x[0-9a-f]+", os_.group(1))] if os_ else []
    return routes, fields, first_ovr, second_ovr

def expand(routes, fields, first_ovr, second_ovr):
    """packed -> (target, gate, clear, first, second, value, gate_mask)"""
    er = [(i, g, v, gm) for (i, g, gm, v) in routes]
    ef, i, j = [], 0, 0
    for (t, g, gshift, flags, fshift, cl, val) in fields:
        first = 0 if fshift == NO_GATE else B(fshift)
        second = first >> 1
        if flags & 1:
            first = first_ovr[i]; i += 1
        if flags & 2:
            second = second_ovr[j]; j += 1
        ef.append((t, g, cl, first, second, val, 0 if gshift == NO_GATE else B(gshift)))
    return er, ef

def run(routes, fields):
    reg, log = {}, []
    def rd(a): return reg.get(a, 0)
    def wr(a, v): reg[a] = v & 0xffffffff
    def clrset(a, c, s): wr(a, (rd(a) & ~c) | s)
    wr(0xa030a0cc, 0x7811102d)
    wr(0xa030a0cc, 0x0011102d)
    for (idx, gate, value, gm) in routes:
        a = 0xa0600ffc + idx * 4
        wr(a, value | B30); log.append((a, rd(a)))
        wr(a, value | B30 | B31); log.append((a, rd(a)))
        if gm:
            clrset(GATE[gate], 0, gm)
    for (t, g, clear, first, second, value, gm) in fields:
        a = TARGET[t]; v = (rd(a) & ~clear) | value | second
        wr(a, v); log.append((a, v))
        wr(a, v | first); log.append((a, rd(a)))
        if gm:
            clrset(GATE[g], 0, gm)
    clrset(0xa030a170, 0, 0x07000000 | B(30) | B(23) | 0x300001e0 | 0x00070000)
    clrset(0xa030a174, 0, B(1) | B(2) | B(14) | 0x1f800000 | B(4))
    return log, dict(reg)

def compare():
    src = pathlib.Path(__file__).resolve().parent.parent / "ipl/usb/usb_msc_min.c"
    packed = parse_c(src)
    la, ra = run(REF_ROUTES, REF_FIELDS)
    lb, rb = run(*expand(*packed))
    return la, ra, lb, rb, packed

def test_cygnus_tables_are_lossless():
    la, ra, lb, rb, packed = compare()
    assert packed[0], "failed to parse cygnus_usb_routes from C"
    assert packed[1], "failed to parse cygnus_usb_fields from C"
    assert len(packed[0]) == len(REF_ROUTES), \
        f"route count {len(packed[0])} != {len(REF_ROUTES)}"
    assert len(packed[1]) == len(REF_FIELDS), \
        f"field count {len(packed[1])} != {len(REF_FIELDS)}"
    assert la == lb, "register write sequence differs"
    assert ra == rb, "final register state differs"

if __name__ == "__main__":
    la, ra, lb, rb, packed = compare()
    ok = la == lb and ra == rb
    print(f"routes parsed: {len(packed[0])}, fields parsed: {len(packed[1])}")
    print(f"writes: {len(la)} vs {len(lb)}")
    if ok:
        print("RESULT: BIT-EXACT MATCH - packed encoding is lossless")
    else:
        print("RESULT: MISMATCH")
        for i, (x, y) in enumerate(zip(la, lb)):
            if x != y:
                print(f"  write#{i}: ref {x[0]:#x}={x[1]:#010x}  got {y[0]:#x}={y[1]:#010x}")
        for a in sorted(set(ra) | set(rb)):
            if ra.get(a) != rb.get(a):
                print(f"  final {a:#x}: ref={ra.get(a,0):#010x} got={rb.get(a,0):#010x}")
    sys.exit(0 if ok else 1)
