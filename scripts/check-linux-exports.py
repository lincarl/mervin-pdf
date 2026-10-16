#!/usr/bin/env python3
"""Fail if a Linux binary exports symbols of MuPDF's bundled codec libraries.

MuPDF's static archives bundle libjpeg, FreeType, zlib and Brotli. If an
executable exports those definitions, the dynamic linker binds every library in
the process to them, so Qt's JPEG plugin, libqpdf and Qt's font code would call
MuPDF's copies instead of the system ones. cmake/FindMuPDF.cmake keeps them out
of the dynamic symbol table. The linux_symbol_exports CTest and the Linux
release jobs run this check.

Usage: check-linux-exports.py [--nm NM] BINARY...
"""

import argparse
import subprocess
import sys


# Name prefixes of the bundled libraries' functions and data that system
# libraries also define. libmupdf.a adds MuPDF's own libjpeg memory manager.
BUNDLED_PREFIXES = {
    "libjpeg": ("jpeg_", "jinit_", "jcopy_", "jdiv_round_up", "jround_up"),
    "FreeType": ("FT_", "TT_"),
    "zlib": ("inflate", "deflate", "adler32", "crc32", "compress", "uncompress",
             "zlib", "zError", "get_crc_table"),
    "Brotli": ("Brotli", "kBrotli", "_kBrotli"),
}


def bundled_exports(nm_output):
    """Return (library, symbol) pairs for bundled symbols in `nm -D --defined-only` output."""
    found = []
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        name = fields[-1].split("@", 1)[0]
        for library, prefixes in BUNDLED_PREFIXES.items():
            if name.startswith(prefixes):
                found.append((library, name))
                break
    return found


def check(nm, path):
    """Print the result for one binary and return whether it passed.

    Raises RuntimeError if nm cannot read the binary.
    """
    result = subprocess.run([nm, "-D", "--defined-only", path],
                            capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"{nm} failed for {path}: {result.stderr.strip()}")
    found = bundled_exports(result.stdout)
    if not found:
        print(f"{path}: no bundled codec symbols among "
              f"{len(result.stdout.splitlines())} exported symbols")
        return True
    print(f"{path} exports {len(found)} symbols of MuPDF's bundled libraries:")
    for library, name in found:
        print(f"  {library}: {name}")
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", default="nm", help="nm program (default: nm)")
    parser.add_argument("binaries", nargs="+", metavar="BINARY")
    args = parser.parse_args(argv)
    try:
        results = [check(args.nm, path) for path in args.binaries]
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
