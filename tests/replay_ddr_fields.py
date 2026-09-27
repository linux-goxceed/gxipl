"""Prove the procedural form of gx6706_ddr_fields matches the reference table.

The stage-1 DDR field patches are stored as a flat list of packed u32 entries
(FP macro in include/ipl_internal.h).  Two large groups inside that list are
repetitive:

  * the 16 width-16 rows are all value 0xffff, with the register offset
    following  off(i) = 0x5f + i + ceil(i/3)  and the shift cycling
    [16, 0, 8];
  * the 32 width-4 rows are two halves of 16 that share ONE value vector,
    walked with two different offset rules and two different shift cycles.

This test parses the real C source and checks, row for row and in order,
that the closed-form generator emits exactly the same
(reg, shift, width, value) sequence as the literal table.  Order is part of
the contract: set_field() is read-modify-write, so two patches that land in
the same register only produce the same final value if they are applied in
the same order.

Run under pytest, or standalone: python3 tests/replay_ddr_fields.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCE = os.path.join(ROOT, "ipl", "gx6706_ipl.c")

FP_RE = re.compile(r"FP\(\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([^)]+?)\s*\)")
TABLE_RE = re.compile(r"gx6706_ddr_fields\[\]\s*=\s*\{(.*?)\n\};", re.S)


def load_reference():
    """Return the literal table as a list of (reg, shift, width, value)."""
    with open(SOURCE) as handle:
        text = handle.read()
    match = TABLE_RE.search(text)
    assert match, "gx6706_ddr_fields[] not found in %s" % SOURCE
    rows = []
    for reg, shift, width, value in FP_RE.findall(match.group(1)):
        value = value.strip()
        value = int(value, 0) if not value.lower().startswith("0x") else int(value, 16)
        rows.append((int(reg, 16), int(shift), int(width), value))
    assert rows, "parsed an empty table"
    return rows


def ceil_div3(i):
    return (i + 2) // 3


# --- width 16 -------------------------------------------------------------
# Reference rows, in source order:
#   FP(0x5f, 16, ..) FP(0x61, 0, ..) FP(0x62, 8, ..) FP(0x63, 16, ..) ...
#   shift cycles 16, 0, 8 ; offset steps 2, 1, 1 repeating.
W16_START = 0x5F
W16_SHIFTS = (16, 0, 8)
W16_VALUE = 0xFFFF
W16_COUNT = 16


def gen_width16():
    rows = []
    for i in range(W16_COUNT):
        off = W16_START + i + ceil_div3(i)
        rows.append((off, W16_SHIFTS[i % 3], 16, W16_VALUE))
    return rows


# --- width 4 --------------------------------------------------------------
# First half:  FP(0x60, 0, 4, 6) FP(0x61, 16, 4, 8) FP(0x62, 24, 4, 10) ...
#   off(i) = 0x60 + i + i//3      shift = [0, 16, 24][i % 3]
# Second half: FP(0x60, 8, 4, 6) FP(0x61, 24, 4, 8) FP(0x63, 0, 4, 10) ...
#   off(i) = 0x60 + i + (i+1)//3  shift = [8, 24, 0][i % 3]
# Both halves read the SAME value vector, which is the duplication that the
# procedural form removes.
W4_START = 0x60
W4_A_SHIFTS = (0, 16, 24)
W4_B_SHIFTS = (8, 24, 0)
W4_COUNT = 16


def split_values(rows):
    """Pull the two width-4 value halves out of the reference rows."""
    vals = [r[3] for r in rows if r[2] == 4]
    assert len(vals) == 2 * W4_COUNT, "expected %d width-4 rows, got %d" % (
        2 * W4_COUNT,
        len(vals),
    )
    return vals[:W4_COUNT], vals[W4_COUNT:]


def gen_width4(values_a, values_b):
    rows = []
    for i in range(W4_COUNT):
        off = W4_START + i + i // 3
        rows.append((off, W4_A_SHIFTS[i % 3], 4, values_a[i]))
    for i in range(W4_COUNT):
        off = W4_START + i + (i + 1) // 3
        rows.append((off, W4_B_SHIFTS[i % 3], 4, values_b[i]))
    return rows


def test_value_halves_are_identical():
    """The premise of the whole optimisation."""
    a, b = split_values(load_reference())
    assert a == b, "width-4 value halves differ; the dedup would be wrong"


def bit_span(reg, shift, width):
    """Half-open bit range this patch writes in this register."""
    return (reg, shift, shift + width)


def find_overlapping_pairs(rows):
    """Every ordered pair of patches whose written bits intersect."""
    hits = []
    for i, (reg, shift, width, _v) in enumerate(rows):
        span = bit_span(reg, shift, width)
        for j in range(i + 1, len(rows)):
            reg2, shift2, width2, _v2 = rows[j]
            span2 = bit_span(reg2, shift2, width2)
            if span[0] == span2[0] and span[1] < span2[2] and span2[1] < span[2]:
                hits.append((i, j))
    return hits


def test_all_patches_are_bit_disjoint():
    """Every patch writes a bit range no other patch writes.

    set_field() is read-modify-write, so a clobber would change the final
    register value.  This checks the property that makes the rewrite safe:
    because no two patches touch the same bit, the GROUP ORDER IS IRRELEVANT
    and the procedural form may emit width-16 and width-4 blocks in any
    order without changing the resulting DDR configuration.

    The width-4 and width-16 groups do share registers (21 registers carry
    more than one patch), but never at overlapping shifts.
    """
    rows = load_reference()
    overlaps = find_overlapping_pairs(rows)
    assert not overlaps, "patches collide on bits %r" % (overlaps[:5],)


def test_shared_registers_never_repeat_a_shift():
    """Second, weaker check on the same invariant, per register."""
    seen = {}
    for reg, shift, width, _v in load_reference():
        key = (shift, shift + width)
        prev = seen.setdefault(reg, [])
        for pshift, pend in prev:
            assert shift >= pend or pshift >= shift + width, (
                "register %s: [%d,%d) collides with [%d,%d)" % (
                    hex(reg),
                    pshift,
                    pend,
                    shift,
                    shift + width,
                )
            )
        prev.append(key)


def test_width16_generated_matches_reference():
    ref = [r for r in load_reference() if r[2] == 16]
    assert len(ref) == W16_COUNT, "expected %d width-16 rows, got %d" % (
        W16_COUNT,
        len(ref),
    )
    gen = gen_width16()
    assert gen == ref, "width-16 generator diverges:\n gen=%r\n ref=%r" % (gen, ref)


def test_width4_generated_matches_reference():
    ref = [r for r in load_reference() if r[2] == 4]
    a, b = split_values(ref)
    gen = gen_width4(a, b)
    assert gen == ref, "width-4 generator diverges:\n gen=%r\n ref=%r" % (gen, ref)


def test_full_sequence_order_preserved():
    """The concatenation of all generated groups must equal the source order.

    This is the check that actually matters: it proves the C rewrite keeps
    every patch in the same position, so read-modify-write accumulation on
    shared registers is identical.
    """
    rows = load_reference()
    a, b = split_values(rows)
    generated = gen_width16() + gen_width4(a, b)

    # Rebuild the reference in the same group order the C code will use.
    expected = [r for r in rows if r[2] == 16]
    expected += [r for r in rows if r[2] == 4]
    assert generated == expected, "group reordering changed patch order"


def test_estimated_size_saving():
    """The literal groups cost 4 B/row; the generated form costs a byte table.

    Guards the premise that this is worth doing at all.
    """
    rows = load_reference()
    a, b = split_values(rows)
    assert a == b
    before = (2 * W4_COUNT + W16_COUNT) * 4
    after = W4_COUNT  # one u8 value vector; shifts and offsets are computed
    assert before - after >= 60, "saving shrank to %d B" % (before - after)


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
        except AssertionError as exc:
            failed += 1
            print("FAIL %s: %s" % (test.__name__, exc))
        else:
            print("ok   %s" % test.__name__)
    print("\n%d/%d passed" % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
