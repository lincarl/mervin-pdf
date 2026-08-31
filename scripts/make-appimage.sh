#!/usr/bin/env bash
# Package a built+installed MervinPDF AppDir into an AppImage using linuxdeploy +
# the Qt plugin. Run after `DESTDIR=$PWD/AppDir cmake --install <build>`.
#
#   Usage: scripts/make-appimage.sh <VERSION> [APPDIR]
#
# Produces MervinPDF-<VERSION>-x86_64.AppImage in the current directory and prints
# its name. Build this on Ubuntu 26.04, the minimum supported release: building on
# an older one and relying on glibc forward-compatibility did not work in practice,
# so the bundle is made on the same release it targets. eng.traineddata is already
# inside the AppDir
# (cmake installs it to usr/share/mervin-pdf/tessdata); TessdataManager finds it at
# runtime via $APPDIR, so OCR works out of the box.
set -euo pipefail

VERSION="${1:?usage: make-appimage.sh <VERSION> [APPDIR]}"
APPDIR="${2:-AppDir}"

# No FUSE on CI runners: make every AppImage (the tools and the one we produce's
# build step) self-extract instead of mounting.
export APPIMAGE_EXTRACT_AND_RUN=1
export VERSION

dl() {
  local url="$1" out="$2" sha256="$3" tmp
  tmp="$(mktemp "${out}.download.XXXXXX")"
  curl -fsSL --retry 3 "$url" -o "$tmp"
  printf '%s  %s\n' "$sha256" "$tmp" | sha256sum --check -
  chmod 0755 "$tmp"
  mv -f "$tmp" "$out"
}
dl https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
  linuxdeploy c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
dl https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage \
  linuxdeploy-plugin-qt 15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
dl https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
  appimagetool a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0
export PATH="$PWD:$PATH"   # linuxdeploy locates the qt plugin + appimagetool here

# Help the Qt plugin find the Qt install. On the 26.04 builder this is the system
# Qt 6.10, and qmake6 comes from qt6-base-dev-tools; the lookup fails loudly under
# `set -e` if neither binary is on PATH, rather than silently deploying no plugins.
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"

./linuxdeploy \
  --appdir "$APPDIR" \
  --plugin qt \
  --output appimage \
  --desktop-file "$APPDIR/usr/share/applications/mervin-pdf.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/mervin-pdf.png"

# linuxdeploy names the output from the desktop "Name" (spaces -> underscores).
produced="$(find . -maxdepth 1 -iname '*.AppImage' \
            ! -iname 'linuxdeploy*' ! -iname 'appimagetool*' | head -n1)"
out="MervinPDF-${VERSION}-x86_64.AppImage"
mv "$produced" "$out"
echo "$out"
