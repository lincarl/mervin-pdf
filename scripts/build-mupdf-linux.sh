#!/usr/bin/env bash
# Build MuPDF 1.28.6 static libraries (with bundled Tesseract OCR + codecs) on
# Linux, exactly as mervin-pdf's cmake/FindMuPDF.cmake expects.
#
#   Usage: scripts/build-mupdf-linux.sh [DEST_DIR]
#          scripts/build-mupdf-linux.sh --patch-set
#
# Produces DEST_DIR/build/release/{libmupdf.a,libmupdf-third.a} and headers in
# DEST_DIR/include/mupdf. Prints DEST_DIR on stdout (use it as MUPDF_DIR);
# all diagnostics go to stderr. --patch-set only prints the patch set's identity,
# which the CI cache keys use.
#
# The build applies the patches in scripts/mupdf-patches to the verified source
# release (see "MuPDF patches" in docs/design.md), which needs git.
# DEST_DIR/build/release/mervin-patch-set.sha256 records the patch set the
# archives were built with. When it matches the current patch set, the script
# skips straight to printing the path, so CI caching short-circuits the rebuild.
# Otherwise it deletes that marker, brings the source to the current patch set
# and rebuilds what changed. DEST_DIR/mervin-patches records the set the source
# has, with copies of its patches; without that folder the source is the
# unchanged release. An existing unpatched build gets the patches. A build with
# another patch set has that set reverted first. If the current set then does
# not apply, the script puts the previous set back and stops.
# A new destination must be absent or an empty directory. Downloads and
# extraction stay in a private staging directory beside it, so unrelated
# destinations and neighboring source trees are never removed or overwritten.
#
# Bundled third-party is used (no USE_SYSTEM_LIBS), so the only build dependency
# is a C/C++ toolchain (build-essential). The resulting archives are
# self-contained; FindMuPDF links them with --start-group + m/pthread/dl and
# --exclude-libs, which keeps the bundled codecs' symbols out of the executable's
# exports so the system libqpdf and Qt keep their own libjpeg, FreeType, zlib and
# Brotli.
set -euo pipefail
# Byte-order globs, so the patch order and the patch set's hash do not depend on
# the locale.
export LC_ALL=C

VERSION="1.28.6"
SHA256="dc85000640a45d90ca9b77b061f6ca67cce0c4d4fbecf8819647baf573eef101"
DEST="${1:-$HOME/src/mupdf-${VERSION}-source}"
URL="https://mupdf.com/downloads/archive/mupdf-${VERSION}-source.tar.gz"
PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)/mupdf-patches"
shopt -s nullglob
PATCHES=("${PATCH_DIR}"/*.patch)
# The patch set's identity is the SHA-256 of sha256sum's lines ("<hash>  <name>")
# for the patches in name order. build-mupdf-windows.ps1, FindMuPDF.cmake and the
# CI cache keys compute the same value.
PATCH_SET="$(
    if [ ${#PATCHES[@]} -gt 0 ]; then (cd "${PATCH_DIR}" && sha256sum -- *.patch); fi \
        | sha256sum | cut -d' ' -f1
)"
if [ "${1:-}" = "--patch-set" ]; then
    echo "${PATCH_SET}"
    exit 0
fi
MARKER="build/release/mervin-patch-set.sha256"

# Remove only temporary directories created by this invocation, including when
# downloading, verifying, extracting, patching or building fails.
staging=""
previous=""
cleanup() {
    if [ -n "${staging}" ]; then rm -rf -- "${staging}"; fi
    if [ -n "${previous}" ]; then rm -rf -- "${previous}"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# A trailing slash must not hide a symlink when checking a new destination.
destination="${DEST}"
while [[ "${destination}" != / && "${destination}" == */ ]]; do
    destination="${destination%/}"
done

# Refuse occupied destinations before downloading and again before publishing.
# nullglob includes hidden entries through the two explicit dot patterns.
check_new_destination() {
    if [ -L "${destination}" ] || { [ -e "${destination}" ] && [ ! -d "${destination}" ]; }; then
        echo "ERROR: ${DEST} already exists and is not an empty directory. Choose another destination." >&2
        return 1
    fi
    local entries=("${destination}"/* "${destination}"/.[!.]* "${destination}"/..?*)
    if [ ${#entries[@]} -gt 0 ]; then
        echo "ERROR: ${DEST} is not an empty directory or a MuPDF source tree. Choose another destination." >&2
        return 1
    fi
}

if [ -f "${DEST}/build/release/libmupdf.a" ]; then
    built="$(cat "${DEST}/${MARKER}" 2>/dev/null || echo none)"
    if [ "${built}" = "${PATCH_SET}" ]; then
        echo "MuPDF already built at ${DEST} with patch set ${PATCH_SET}" >&2
        echo "${DEST}"
        exit 0
    fi
    # Never replace a build that has no source tree to patch.
    [ -f "${DEST}/Makefile" ] || { echo "ERROR: ${DEST} holds MuPDF libraries without their source. Choose another destination." >&2; exit 1; }
    echo "MuPDF at ${DEST} was built with patch set ${built}, not ${PATCH_SET}. Rebuilding." >&2
fi

if [ ! -f "${DEST}/Makefile" ]; then
    check_new_destination
    parent="$(dirname -- "${destination}")"
    mkdir -p -- "${parent}"
    parent="$(cd -- "${parent}" && pwd)"
    staging="$(mktemp -d "${parent}/.mupdf-${VERSION}.XXXXXX")"
    tmptar="${staging}/source.tar.gz"
    echo "Downloading ${URL}" >&2
    curl -fSL --retry 3 "${URL}" -o "${tmptar}"
    printf '%s  %s\n' "${SHA256}" "${tmptar}" | sha256sum --check - >&2
    tar -xzf "${tmptar}" -C "${staging}"
    produced="${staging}/mupdf-${VERSION}-source"
    [ -d "${produced}" ] && [ ! -L "${produced}" ] \
        && [ -f "${produced}/Makefile" ] && [ -f "${produced}/include/mupdf/fitz.h" ] \
        || { echo "ERROR: expected MuPDF source at ${produced} after extraction" >&2; exit 1; }
    check_new_destination
    # Rename on the same filesystem. -T prevents nesting inside a destination
    # that appeared meanwhile, and refuses files, symlinks and nonempty folders.
    mv -T -- "${produced}" "${destination}"
fi

# A Makefile alone does not identify a MuPDF source tree. Check before removing
# build or patch records from a directory supplied by the caller.
[ -f "${DEST}/include/mupdf/fitz.h" ] \
    || { echo "ERROR: ${DEST} is not a MuPDF source tree. Choose another destination." >&2; exit 1; }

cd -- "${DEST}"
# Until make succeeds, the archives match no patch set.
rm -f "${MARKER}"

# Bring the source to the current patch set unless it already has it. Each set
# goes to git apply as one input, which changes nothing when any part fails
# (separate patch files would be applied one by one). With --reverse, git apply
# takes the patches in their forward order. The search for an enclosing
# repository stops at DEST_DIR, so a DEST_DIR inside a Git checkout behaves the
# same as one outside it.
#
# mervin-patches only ever appears once the source matches it, so next to it a
# mervin-patches.previous folder is a stale leftover. Without it, an update
# stopped while the source was changing.
if [ -e mervin-patches.previous ]; then
    [ -d mervin-patches ] \
        || { echo "ERROR: an earlier patch update of ${DEST} did not finish. Delete it and rerun." >&2; exit 1; }
    rm -rf mervin-patches.previous
fi
rm -rf mervin-patches.new
recorded="$(cat mervin-patches/patch-set.sha256 2>/dev/null || true)"
if [ "${recorded}" != "${PATCH_SET}" ]; then
    command -v git >/dev/null || { echo "ERROR: git is needed to apply the MuPDF patches" >&2; exit 1; }
    export GIT_CEILING_DIRECTORIES="${PWD%/*}"
    # Writes the record for the set in $1 with the patches that follow.
    record() {
        local id=$1
        shift
        mkdir mervin-patches.new
        if [ $# -gt 0 ]; then cp -- "$@" mervin-patches.new/; fi
        echo "${id}" > mervin-patches.new/patch-set.sha256
        mv mervin-patches.new mervin-patches
    }
    previous="$(mktemp -d)"
    old=(mervin-patches/*.patch)
    if [ ${#old[@]} -gt 0 ]; then
        echo "Reverting the MuPDF patch set recorded in ${DEST}/mervin-patches" >&2
        cp -- "${old[@]}" "${previous}/"
        old=("${previous}"/*.patch)
        # If this run stops while the record is renamed, the next run refuses to go on.
        mv mervin-patches mervin-patches.previous
        if ! cat -- "${old[@]}" | git apply --reverse >&2; then
            mv mervin-patches.previous mervin-patches
            echo "ERROR: cannot revert the recorded patches. Delete ${DEST} and rerun." >&2
            exit 1
        fi
    fi
    # The source is the unchanged release now, and so no record is the true one.
    rm -rf mervin-patches mervin-patches.previous
    if [ ${#PATCHES[@]} -gt 0 ]; then
        echo "Applying ${#PATCHES[@]} MuPDF patches from ${PATCH_DIR}" >&2
        if ! cat -- "${PATCHES[@]}" | git apply >&2; then
            # Put the previous set back, so the source matches a record again.
            if [ ${#old[@]} -gt 0 ] && cat -- "${old[@]}" | git apply >&2; then
                record "${recorded}" "${old[@]}"
                echo "ERROR: the patches in ${PATCH_DIR} do not apply. ${DEST} keeps its previous patch set." >&2
            else
                echo "ERROR: the patches do not apply to ${DEST}, which must hold the unchanged MuPDF ${VERSION} source. Delete it and rerun." >&2
            fi
            exit 1
        fi
    fi
    record "${PATCH_SET}" "${PATCHES[@]}"
fi

# `libs` target builds only libmupdf.a + libmupdf-third.a (no command-line tools).
# Send make's chatter to stderr so stdout carries only the printed DEST path
# (callers do MUPDF_DIR="$(build-mupdf-linux.sh)").
make build=release USE_TESSERACT=yes HAVE_X11=no HAVE_GLUT=no -j"$(nproc)" libs 1>&2

test -f build/release/libmupdf.a       || { echo "ERROR: libmupdf.a not produced" >&2; exit 1; }
test -f build/release/libmupdf-third.a || { echo "ERROR: libmupdf-third.a not produced" >&2; exit 1; }
echo "${PATCH_SET}" > "${MARKER}"
echo "Built MuPDF static libs with patch set ${PATCH_SET}:" >&2
ls -lh build/release/libmupdf*.a >&2
echo "${DEST}"
