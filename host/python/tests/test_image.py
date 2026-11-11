# SPDX-License-Identifier: MIT
"""Image handling: Intel HEX parsing, segment merging, erase-page runs."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import image  # noqa: E402

FLASH = [dict(start=0x1000, size=0x4000, sector=0x400, kind="flash"), dict(start=0x20000000, size=0x1000,
                                                                            sector=4, kind="ram")]


def hex_record(addr, typ, data):
    rec = bytes([len(data), addr >> 8, addr & 0xFF, typ]) + data
    return ":" + (rec + bytes([-sum(rec) & 0xFF])).hex().upper() + "\n"


class ImageTest(unittest.TestCase):
    def write_hex(self, records):
        f = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
        self.addCleanup(os.unlink, f.name)
        f.write("".join(records) + ":00000001FF\n")
        f.close()
        return f.name

    def test_hex_segments_merge_and_extended_address(self):
        path = self.write_hex([hex_record(0, 4, b"\x00\x00"), hex_record(0x1000, 0, b"\x01" * 16),
                               hex_record(0x1010, 0, b"\x02" * 16), hex_record(0x1800, 0, b"\x03" * 4)])
        self.assertEqual(image.hex_segments(path), [(0x1000, b"\x01" * 16 + b"\x02" * 16), (0x1800, b"\x03" * 4)])
        self.assertEqual(len(image.read_hex(path)), 36)

    def test_overlap_later_record_wins(self):
        recs = [(0x10, b"\x01\x02\x03\x04"), (0x0, b"\xaa" * 16), (0x12, b"\x09")]
        self.assertEqual(image._merge(recs), [(0, b"\xaa" * 16 + b"\x01\x02\x09\x04")])

    def test_page_runs_fill_and_merge(self):
        segs = [(0x1010, b"\x11" * 8), (0x1400, b"\x22" * 0x400), (0x1C00, b"\x33")]
        runs = image.page_runs(segs, FLASH)
        self.assertEqual([(a, len(b)) for a, b in runs], [(0x1000, 0x800), (0x1C00, 0x400)])
        first = runs[0][1]
        self.assertEqual(first[:0x10], b"\xff" * 0x10)
        self.assertEqual(first[0x10:0x18], b"\x11" * 8)
        self.assertEqual(first[0x400:0x800], b"\x22" * 0x400)
        self.assertEqual(runs[1][1][:2], b"\x33\xff")
        as_dict = {a + i: b for a, d in segs for i, b in enumerate(d)}
        self.assertEqual(image.page_runs(as_dict, FLASH), runs)   # the old {address: byte} form gives the same

    def test_outside_or_not_flash_refused(self):
        with self.assertRaises(ValueError):
            image.page_runs([(0x9000, b"\x00")], FLASH)
        with self.assertRaises(ValueError):
            image.page_runs([(0x20000000, b"\x00")], FLASH)

    def test_bin_at_default_and_given_address(self):
        f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.addCleanup(os.unlink, f.name)
        f.write(b"\x5a" * 10)
        f.close()
        self.assertEqual(image.load(f.name, FLASH), [(0x1000, b"\x5a" * 10)])
        self.assertEqual(image.load(f.name, FLASH, 0x2000), [(0x2000, b"\x5a" * 10)])
        self.assertEqual(image.size(image.load(f.name, FLASH)), 10)


if __name__ == "__main__":
    unittest.main()
