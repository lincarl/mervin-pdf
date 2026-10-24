#!/usr/bin/env bash
# Prepare the manifest's exact vcpkg revision and print its directory.
# Usage: scripts/bootstrap-vcpkg-linux.sh [DEST_DIR]
# CMake installs the manifest dependencies into its own build directory.
set -euo pipefail

if [[ $# -gt 1 ]]; then
    echo 'Usage: scripts/bootstrap-vcpkg-linux.sh [DEST_DIR]' >&2
    exit 2
fi
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    echo 'Usage: scripts/bootstrap-vcpkg-linux.sh [DEST_DIR]'
    exit 0
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["builtin-baseline"])' "$root/vcpkg.json")"
[[ "$baseline" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid vcpkg baseline' >&2; exit 1; }
destination="${1:-$HOME/dev-tools/mervin-vcpkg-$baseline}"
repository=https://github.com/microsoft/vcpkg.git

if [[ ! -e "$destination" ]]; then
    mkdir -p "$destination"
fi
destination="$(cd "$destination" && pwd)"
if [[ ! -d "$destination/.git" ]]; then
    if [[ -n "$(ls -A "$destination")" ]]; then
        echo "Refusing occupied non-vcpkg directory $destination" >&2
        exit 1
    fi
    git -C "$destination" init -q >&2
    git -C "$destination" remote add origin "$repository"
fi
if [[ "$(git -C "$destination" remote get-url origin)" != "$repository" ]] \
    || [[ -n "$(git -C "$destination" status --porcelain --untracked-files=no)" ]]; then
    echo "Expected an unchanged vcpkg checkout at $destination" >&2
    exit 1
fi
if [[ "$(git -C "$destination" rev-parse --verify HEAD 2>/dev/null || true)" != "$baseline" ]]; then
    git -C "$destination" fetch --depth 1 origin "$baseline" >&2
    git -C "$destination" checkout --detach "$baseline" >&2
fi
"$destination/bootstrap-vcpkg.sh" -disableMetrics >&2
printf '%s\n' "$destination"
