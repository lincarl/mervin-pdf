#!/usr/bin/env bash
# Build MuPDF 1.28.0 static libraries (with bundled Tesseract OCR + codecs) on
# Linux, exactly as mervin-pdf's cmake/FindMuPDF.cmake expects.
#
#   Usage: scripts/build-mupdf-linux.sh [DEST_DIR]
#
# Produces DEST_DIR/build/release/{libmupdf.a,libmupdf-third.a} and headers in
# DEST_DIR/include/mupdf. Prints DEST_DIR on stdout (use it as MUPDF_DIR);
# all diagnostics go to stderr. Idempotent: if the archives already exist it
# skips straight to printing the path, so CI caching short-circuits the rebuild.
#
# Bundled third-party is used (no USE_SYSTEM_LIBS), so the only build dependency
# is a C/C++ toolchain (build-essential). The resulting archives are
# self-contained; FindMuPDF links them with --start-group + m/pthread/dl.
set -euo pipefail

VERSION="1.28.0"
SHA256="21c7f064903154f1c3a7458bee81f130fc36f9b5147ea13328f9980e02d2dea2"
DEST="${1:-$HOME/src/mupdf-${VERSION}-source}"
URL="https://mupdf.com/downloads/archive/mupdf-${VERSION}-source.tar.gz"

if [ -f "${DEST}/build/release/libmupdf.a" ]; then
    echo "MuPDF already built at ${DEST}" >&2
    echo "${DEST}"
    exit 0
fi

if [ ! -f "${DEST}/Makefile" ]; then
    parent="$(dirname "${DEST}")"
    mkdir -p "${parent}"
    tmptar="$(mktemp)"
    echo "Downloading ${URL}" >&2
    curl -fSL --retry 3 "${URL}" -o "${tmptar}"
    printf '%s  %s\n' "${SHA256}" "${tmptar}" | sha256sum --check - >&2
    rm -rf "${DEST}"
    # Extract straight into the parent, then rename within it (a same-dir rename,
    # not a cross-filesystem copy of MuPDF's large tree).
    tar -xzf "${tmptar}" -C "${parent}"
    rm -f "${tmptar}"
    produced="${parent}/mupdf-${VERSION}-source"
    [ -d "${produced}" ] || { echo "ERROR: expected ${produced} after extraction" >&2; exit 1; }
    [ "${produced}" = "${DEST}" ] || mv "${produced}" "${DEST}"
fi

cd "${DEST}"
# `libs` target builds only libmupdf.a + libmupdf-third.a (no command-line tools).
# Send make's chatter to stderr so stdout carries only the printed DEST path
# (callers do MUPDF_DIR="$(build-mupdf-linux.sh)").
make build=release USE_TESSERACT=yes HAVE_X11=no HAVE_GLUT=no -j"$(nproc)" libs 1>&2

test -f build/release/libmupdf.a       || { echo "ERROR: libmupdf.a not produced" >&2; exit 1; }
test -f build/release/libmupdf-third.a || { echo "ERROR: libmupdf-third.a not produced" >&2; exit 1; }
echo "Built MuPDF static libs:" >&2
ls -lh build/release/libmupdf*.a >&2
echo "${DEST}"
