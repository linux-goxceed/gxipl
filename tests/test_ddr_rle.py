# SPDX-License-Identifier: MIT
"""Host-side checks for the RLE DDR register streams.

The IPL decodes these streams straight into the DDR controller, so a
round-trip failure would mean the board gets wrong memory timings.  These
tests re-encode from the canonical C tables and compare against both the
generator's own decoder and the committed header the build actually uses.
"""

import importlib.util
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
GEN = ROOT / "utils" / "gen_ddr_rle.py"

spec = importlib.util.spec_from_file_location("gen_ddr_rle", GEN)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def parse_committed(header_text, name):
    m = re.search(
        r"\b%s\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;" % re.escape(name),
        header_text,
        re.DOTALL,
    )
    if not m:
        raise AssertionError(f"{name} not found in generated header")
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


class DdrRleTests(unittest.TestCase):
    def assert_round_trip(self, soc, arrays):
        text = (ROOT / gen.SOURCES[soc]["file"]).read_text()
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "ddr_rle.h"
            subprocess.run(
                [sys.executable, str(GEN), "--soc", soc, "-o", str(out)],
                check=True,
                cwd=ROOT,
                capture_output=True,
            )
            header = out.read_text()

        for name, count in arrays.items():
            words = gen.parse_array(text, name, count)
            blob = gen.encode(words)
            # The generator's reference decoder must reproduce the table.
            self.assertEqual(
                gen.decode(blob, count),
                words,
                f"{soc}:{name} decode mismatch",
            )
            # The emitted header must carry the same bytes.
            committed_name = "ddr_r0" if name.endswith("regs_0") else "ddr_r100"
            self.assertEqual(
                parse_committed(header, committed_name),
                blob,
                f"{soc}:{name} header != freshly encoded stream",
            )
            # If a build tree exists, its generated header must match too.
            built = ROOT / "build" / soc / "ddr_rle.h"
            if built.exists():
                self.assertEqual(
                    parse_committed(built.read_text(), committed_name),
                    blob,
                    f"{soc}:{name} stale build/{soc}/ddr_rle.h",
                )
            self.assertLess(
                len(blob),
                count * 4,
                f"{soc}:{name} did not compress",
            )

    def test_gx6702_round_trip(self):
        self.assert_round_trip("gx6702", {"ddr_regs_0": 155, "ddr_regs_100": 26})

    def test_gx6706_round_trip(self):
        self.assert_round_trip(
            "gx6706",
            {"gx6706_ddr_regs_0": 155, "gx6706_ddr_regs_100": 31},
        )

    def test_decoder_token_ranges(self):
        """Every emitted tag must fall in a range the C decoder handles."""
        for soc in ("gx6702", "gx6706"):
            text = (ROOT / gen.SOURCES[soc]["file"]).read_text()
            for name, count in gen.SOURCES[soc]["arrays"].items():
                blob = gen.encode(gen.parse_array(text, name, count))
                p = 0
                while p < len(blob):
                    tag = blob[p]
                    p += 1
                    if tag < 0x80:
                        p += 4 * (tag + 1)
                    elif tag < 0xA0:
                        pass
                    else:
                        back = blob[p]
                        p += 1
                        self.assertGreaterEqual(back, 1)
                        self.assertLessEqual(back, 16)
                self.assertEqual(p, len(blob), f"{soc}:{name} token walk overran")


if __name__ == "__main__":
    unittest.main()
