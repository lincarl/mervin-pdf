#!/usr/bin/env python3
"""Exercise the check for exported MuPDF codec symbols."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import textwrap
import unittest


spec = importlib.util.spec_from_file_location(
    "linux_exports", Path(__file__).with_name("check-linux-exports.py"))
exports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exports)

# Lines in the format of `nm -D --defined-only`, taken from a MervinPDF build
# that exported MuPDF's bundled libraries.
BUNDLED = """\
00000000009fd2a0 T jpeg_CreateDecompress
0000000000a0c6d0 T jinit_master_decompress
0000000000a03b70 T jcopy_sample_rows
0000000000a03b30 T jdiv_round_up
0000000000a03b50 T jround_up
0000000000a1e1c0 T jpeg_get_small
0000000000b0a200 T FT_New_Face
0000000000b4f0e0 T TT_RunIns
0000000000c0e0b0 T inflateInit2_
0000000000c0d2e0 T deflateBound
0000000000c0b010 T adler32_combine64
0000000000c0b6d0 T crc32_z
0000000000c0c1a0 T compressBound
0000000000c11f20 T uncompress
0000000000c0bb10 T zlibVersion
0000000000c0bb30 T zError
0000000000c0b6b0 T get_crc_table
0000000000c2a7b0 T BrotliDecoderCreateInstance
0000000000bd6540 R _kBrotliPrefixCodeRanges
0000000000c11f40 T inflate@@ZLIB_1.2.0
"""

# Exports that a correctly linked executable keeps.
UNRELATED = """\
0000000003518e00 B __libc_single_threaded@GLIBC_2.32
0000000003518de0 B stderr@GLIBC_2.2.5
000000000029ffb0 W _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE10_M_disposeEv
00000000013faee0 V _ZTI7QPDFExc
0000000000be1c2c u _ZSt19piecewise_construct
0000000000123450 T qt_jpeg_inflate
0000000000123460 T my_crc32
"""

FAKE_NM = textwrap.dedent("""\
    #!{python}
    import sys
    if sys.argv[1:3] != ["-D", "--defined-only"]:
        sys.exit("unexpected arguments")
    text = open(sys.argv[3]).read()
    if text.startswith("unreadable"):
        sys.exit("file format not recognized")
    print(text, end="")
    """)


class ClassificationTests(unittest.TestCase):
    def test_reports_every_bundled_family(self):
        found = exports.bundled_exports(BUNDLED)
        self.assertEqual(len(found), len(BUNDLED.splitlines()))
        self.assertEqual({library for library, _ in found},
                         {"libjpeg", "FreeType", "zlib", "Brotli"})
        self.assertIn(("zlib", "inflate"), found)
        self.assertIn(("Brotli", "_kBrotliPrefixCodeRanges"), found)

    def test_ignores_other_exports(self):
        self.assertEqual(exports.bundled_exports(UNRELATED), [])
        self.assertEqual(exports.bundled_exports(""), [])


class CommandTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.nm = self.root / "nm"
        self.nm.write_text(FAKE_NM.format(python=sys.executable))
        self.nm.chmod(0o755)

    def binary(self, name, listing):
        path = self.root / name
        path.write_text(listing)
        return str(path)

    def run_check(self, *paths):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = exports.main(["--nm", str(self.nm), *paths])
        return status, out.getvalue(), err.getvalue()

    def test_clean_binaries_pass(self):
        status, out, _ = self.run_check(self.binary("app", UNRELATED),
                                        self.binary("test", UNRELATED))
        self.assertEqual(status, 0)
        self.assertIn("no bundled codec symbols among 7 exported symbols", out)

    def test_one_exporting_binary_fails_the_check(self):
        status, out, _ = self.run_check(self.binary("app", UNRELATED),
                                        self.binary("test", UNRELATED + BUNDLED))
        self.assertEqual(status, 1)
        self.assertIn("exports 20 symbols of MuPDF's bundled libraries", out)
        self.assertIn("  libjpeg: jpeg_CreateDecompress", out)

    def test_unreadable_binary_is_an_error(self):
        status, _, err = self.run_check(self.binary("app", "unreadable"))
        self.assertEqual(status, 2)
        self.assertIn("file format not recognized", err)
        status, _, err = self.run_check(str(self.root / "missing"))
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
