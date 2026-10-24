#!/usr/bin/env bash
# Install portable CPython for Linux x86_64 without changing the system Python.
# Usage: scripts/bootstrap-python-linux.sh [DEST]
# DEST must not exist. The default is ~/dev-tools/cpython-3.15.1.
# Prints the absolute interpreter path on stdout and diagnostics on stderr.
# Requires bash, curl, tar, gzip and coreutils from the Linux distribution.
set -euo pipefail

if [[ $# -gt 1 ]]; then
    echo "Usage: scripts/bootstrap-python-linux.sh [DEST]" >&2
    exit 2
fi
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    echo "Usage: scripts/bootstrap-python-linux.sh [DEST]"
    echo "Install Python 3.15.1 into a new directory, default ~/dev-tools/cpython-3.15.1."
    exit 0
fi
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
    echo "This Python build requires Linux x86_64 with glibc." >&2
    exit 1
fi

VERSION="3.15.1"
RELEASE="20261021"
ARCHIVE="cpython-${VERSION}%2B${RELEASE}-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
URL="https://releases.astral.sh/github/python-build-standalone/releases/download/${RELEASE}/${ARCHIVE}"
# Official asset digest from the GitHub release below, also verified on download.
# https://github.com/astral-sh/python-build-standalone/releases/tag/20261021
SHA256="01d901e09ca673c99a6c9bd9ec2cfbd58dbe8813d07514c36cb73cfd20b18bd8"
destination="${1:-${HOME}/dev-tools/cpython-${VERSION}}"

# Refuse existing paths, including dangling symlinks, before any installation.
check_destination() {
    if [[ -e "${destination}" || -L "${destination}" ]]; then
        echo "Destination already exists. Choose a new directory for ${destination}." >&2
        return 1
    fi
}
check_destination
parent="$(dirname -- "${destination}")"
mkdir -p -- "${parent}"
parent="$(cd -- "${parent}" && pwd)"
destination="${parent}/$(basename -- "${destination}")"
check_destination

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p -- "${root}/dev-temp"
staging="$(mktemp -d "${root}/dev-temp/bootstrap-python-linux.XXXXXX")"
trap 'rm -rf -- "${staging}"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "Downloading Python ${VERSION} from python-build-standalone ${RELEASE}" >&2
curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
    --max-time 600 "${URL}" --output "${staging}/python.tar.gz"
printf '%s  %s\n' "${SHA256}" "${staging}/python.tar.gz" | sha256sum --check - >&2
tar -xzf "${staging}/python.tar.gz" -C "${staging}" --no-same-owner --no-same-permissions
"${staging}/python/bin/python3.15" -I -c \
    'import ensurepip, ssl, sys, venv; assert sys.version.split()[0] == sys.argv[1]; print(sys.version)' \
    "${VERSION}" >&2
check_destination
mv -T -- "${staging}/python" "${destination}"
printf '%s\n' "${destination}/bin/python3.15"
