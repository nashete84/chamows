#!/usr/bin/env bash
# Arranca Chamows de 64 bits en QEMU con teclado, ratón y un pendrive USB.
#   ./run-chamows.sh [chamows.bin] [pendrive.img]
# Mac: brew install qemu. El pendrive (64 MB, FAT32) se crea la primera vez
# y guarda las apps que instales.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="${1:-$here/chamows.bin}"
stick="${2:-$here/pendrive.img}"
[ -f "$bin" ] || { echo "No encuentro $bin"; exit 1; }
if [ ! -f "$stick" ]; then
  mk="$here/mkfat32.py"
  [ -f "$mk" ] || mk="$here/../tools/mkfat32.py"
  python3 "$mk" "$stick" 64
fi
exec qemu-system-x86_64 -kernel "$bin" -vga std -m 256 \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
  -drive if=none,id=stick,format=raw,file="$stick" -device usb-storage,bus=xhci.0,drive=stick
