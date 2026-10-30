#!/usr/bin/env bash
# Download the v86 emulator and its BIOS files into web/ so the browser page
# can run locally (python3 -m http.server -d web). Versions are pinned.
set -euo pipefail
cd "$(dirname "$0")/../web"
V86_VERSION=0.5.471
BIOS_COMMIT=94faf3d
tmp=$(mktemp -d)
curl -fsSL "https://registry.npmjs.org/v86/-/v86-${V86_VERSION}.tgz" | tar -xz -C "$tmp"
cp "$tmp/package/build/libv86.js" "$tmp/package/build/v86.wasm" .
for f in seabios.bin vgabios.bin; do
  curl -fsSL "https://raw.githubusercontent.com/copy/v86/${BIOS_COMMIT}/bios/$f" -o "$tmp/$f"
  base64 -w0 "$tmp/$f" > "${f%.bin}.b64.txt"
done
rm -rf "$tmp"
echo "v86 listo en web/"
