#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
command -v make >/dev/null || { echo "Install base-devel first"; exit 1; }
command -v pkg-config >/dev/null || { echo "Install pkgconf first"; exit 1; }
pkg-config --exists libdrm || { echo "Missing libdrm development files. Run: sudo pacman -S --needed libdrm"; exit 1; }
make
make install-user
echo
echo "IDW installed. Run: idwctl info"
echo "Open the desktop app: IDW — Intel GPU Properties"
