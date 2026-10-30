#!/bin/bash
# Starts the single Chamows desktop: a TigerVNC X server on localhost:5901
# running KDE Plasma. Both noVNC (browser) and xrdp (RDP) connect to it.
set -uo pipefail

export DISPLAY=:1
PASSWD_FILE="$HOME/.config/tigervnc/passwd"

if [ ! -s "$PASSWD_FILE" ]; then
  echo "chamows: falta $PASSWD_FILE (¿se ejecutó chamows-set-password?)" >&2
  exit 1
fi

# Clean up after an unclean stop.
rm -f /tmp/.X1-lock /tmp/.X11-unix/X1 2>/dev/null || true

Xtigervnc "$DISPLAY" \
  -localhost -rfbport 5901 -nolisten tcp \
  -rfbauth "$PASSWD_FILE" -SecurityTypes VncAuth \
  -AlwaysShared -geometry 1920x1080 -depth 24 \
  -desktop Chamows &
XPID=$!
trap 'kill "$XPID" 2>/dev/null' EXIT

for _ in $(seq 50); do
  [ -S /tmp/.X11-unix/X1 ] && break
  sleep 0.2
done

export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
export DESKTOP_SESSION=plasma
export KDE_FULL_SESSION=true
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
# Apps installed from Flathub show up in the Start menu.
export XDG_DATA_DIRS="$HOME/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share"
# No GPU in a VM: skip compositing effects.
export KWIN_COMPOSE=N

dbus-run-session -- startplasma-x11
