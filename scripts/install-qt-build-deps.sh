#!/usr/bin/env bash
# Install source Qt's desktop integration headers on the supported CI images.
set -euo pipefail
if command -v apt-get >/dev/null; then
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends \
    build-essential pkg-config libgl1-mesa-dev libegl1-mesa-dev \
    libfontconfig1-dev libfreetype-dev libdbus-1-dev libssl-dev libcups2-dev \
    libgtk-3-dev libwayland-dev libwayland-bin wayland-protocols libx11-dev libx11-xcb-dev \
    libxcb1-dev libxcb-util-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-image0-dev \
    libxcb-keysyms1-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
    libxcb-xinerama0-dev libxcb-randr0-dev libxcb-sync-dev libxcb-xkb-dev \
    libxkbcommon-dev libxkbcommon-x11-dev zlib1g-dev
elif command -v dnf >/dev/null; then
  dnf install -y gcc-c++ make pkgconf-pkg-config mesa-libGL-devel mesa-libEGL-devel \
    fontconfig-devel freetype-devel dbus-devel openssl-devel cups-devel gtk3-devel \
    wayland-devel wayland-protocols-devel libX11-devel libxcb-devel \
    xcb-util-devel xcb-util-cursor-devel xcb-util-wm-devel xcb-util-image-devel \
    xcb-util-keysyms-devel xcb-util-renderutil-devel libxkbcommon-devel \
    libxkbcommon-x11-devel zlib-devel
else
  echo 'Unsupported Qt build platform' >&2
  exit 1
fi
