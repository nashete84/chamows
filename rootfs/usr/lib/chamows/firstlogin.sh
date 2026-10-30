#!/bin/bash
# Applies the Chamows look the first time the user's desktop starts.
MARKER="$HOME/.config/chamows/firstlogin.done"
[ -e "$MARKER" ] && exit 0
mkdir -p "$(dirname "$MARKER")"

plasma-apply-colorscheme FluentLight || true
plasma-apply-desktoptheme Fluent-round-light || true
plasma-apply-cursortheme Fluent-cursors || true

# Desktop folders in Spanish (Escritorio, Descargas, ...).
xdg-user-dirs-update --force || true

touch "$MARKER"
