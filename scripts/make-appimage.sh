#!/usr/bin/env bash
# Package the private runtime installed with MERVIN_BUNDLE_LINUX_RUNTIME=ON.
# Run after `DESTDIR=$PWD/AppDir cmake --install <build> --prefix /usr`.
#
# Usage: scripts/make-appimage.sh <VERSION> [APPDIR]
# Build on Ubuntu 26.04, the minimum supported release. OCR language models
# are downloaded into the user profile and are not part of the AppDir.
set -euo pipefail

VERSION="${1:?usage: make-appimage.sh <VERSION> [APPDIR]}"
APPDIR="$(realpath "${2:-AppDir}")"
OUT="$PWD/MervinPDF-${VERSION}-x86_64.AppImage"
TOOLS_DIR="${MERVIN_APPIMAGE_TOOLS_DIR:-$HOME/dev-tools/mervin-appimage-tools}"

# The install step already gathered Qt and application dependencies. Redeploying
# here could pull in a different Qt or create a second runtime under usr/lib.
for required in usr/bin/MervinPDF usr/lib/mervin-pdf/bin/MervinPDF \
    usr/lib/mervin-pdf/bin/qt.conf usr/lib/mervin-pdf/lib/libQt6Core.so.6 \
    usr/lib/mervin-pdf/plugins/platforms/libqxcb.so \
    usr/lib/mervin-pdf/plugins/platforms/libqwayland.so; do
  if [[ ! -f "$APPDIR/$required" ]]; then
    echo "Missing $required. Install with MERVIN_BUNDLE_LINUX_RUNTIME=ON first." >&2
    exit 1
  fi
done

# No FUSE is needed on the builder.
export APPIMAGE_EXTRACT_AND_RUN=1
export VERSION
mkdir -p "$TOOLS_DIR"

# Reuse only a verified tool. Pin a numbered release instead of continuous.
dl() {
  local url="$1" out="$2" sha256="$3" tmp
  if [[ -f "$out" ]] && printf '%s  %s\n' "$sha256" "$out" | sha256sum --check --status; then
    return
  fi
  tmp="$(mktemp "${out}.download.XXXXXX")"
  curl -fsSL --retry 3 "$url" -o "$tmp"
  printf '%s  %s\n' "$sha256" "$tmp" | sha256sum --check -
  chmod 0755 "$tmp"
  mv -f "$tmp" "$out"
}
dl https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
  "$TOOLS_DIR/appimagetool" ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

cp "$APPDIR/usr/share/applications/mervin-pdf.desktop" "$APPDIR/mervin-pdf.desktop"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/mervin-pdf.png" "$APPDIR/mervin-pdf.png"
ln -sfn mervin-pdf.png "$APPDIR/.DirIcon"
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
set -eu
appdir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec "$appdir/usr/bin/MervinPDF" "$@"
APPRUN
chmod 0755 "$APPDIR/AppRun"
ARCH=x86_64 "$TOOLS_DIR/appimagetool" "$APPDIR" "$OUT"
echo "$OUT"
