#!/bin/bash
# Chamows installer: turns a fresh Ubuntu 24.04 server into Chamows.
#
#   sudo ./scripts/install.sh          # used by Packer to build the AMI
#   sudo ./scripts/install.sh --now    # install on a running machine and start it
#
# It is safe to run more than once.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
USER_NAME=chamows

# Pinned theme versions, so an upstream change can't silently break the image.
FLUENT_KDE_COMMIT=44794f29c89de994b0179aebabd2f5776c90d236
FLUENT_ICONS_COMMIT=c8a244f571ef8fb47812bdc9dbce76546cedce69
# Mozilla APT signing key (https://support.mozilla.org/kb/install-firefox-linux).
MOZILLA_KEY_FPR=35BAA0B33E9EB396F59CA838C0BA5CE6DC6315A3

RUN_NOW=0
[ "${1:-}" = "--now" ] && RUN_NOW=1

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "ejecuta este script con sudo"
# shellcheck disable=SC1091
. /etc/os-release
if [ "${VERSION_ID:-}" != "24.04" ] && [ "${CHAMOWS_ALLOW_OTHER_RELEASE:-0}" != "1" ]; then
  die "Chamows está pensado para Ubuntu 24.04 (esta máquina es ${PRETTY_NAME:-desconocida})"
fi

export DEBIAN_FRONTEND=noninteractive
APT_OPTS=(-y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold)

# Keep only the packages that exist in this release's archive.
available() {
  local p out=()
  for p in "$@"; do
    if apt-cache show "${p%%:*}" >/dev/null 2>&1; then out+=("$p"); else echo "  (omitido: $p)" >&2; fi
  done
  if [ "${#out[@]}" -gt 0 ]; then printf '%s\n' "${out[@]}"; fi
}

# ---------------------------------------------------------------- packages
log "Actualizando el sistema"
dpkg --add-architecture i386
apt-get update -q
apt-get "${APT_OPTS[@]}" dist-upgrade

log "Instalando escritorio, acceso remoto, Wine y Flatpak"
CORE_PKGS=(
  kde-plasma-desktop dolphin konsole kate ark gwenview okular
  xdg-desktop-portal-kde xdg-user-dirs
  locales
  fonts-noto-core fonts-noto-color-emoji
  tigervnc-standalone-server novnc websockify nginx xrdp ssl-cert
  flatpak wine wine64 wine32:i386
  python3 python3-pyqt5
  git gnupg curl ca-certificates openssl librsvg2-bin
)
OPTIONAL_PKGS=(tigervnc-tools kde-spectacle language-pack-es language-pack-kde-es)
mapfile -t EXTRA < <(available "${OPTIONAL_PKGS[@]}")
apt-get "${APT_OPTS[@]}" install "${CORE_PKGS[@]}" "${EXTRA[@]}"
command -v tigervncpasswd >/dev/null || die "falta tigervncpasswd"

log "Instalando Firefox desde el repositorio oficial de Mozilla"
install -d -m 0755 /etc/apt/keyrings
KEY_TMP=$(mktemp)
curl -fsSL https://packages.mozilla.org/apt/repo-signing-key.gpg -o "$KEY_TMP"
FPR=$(gpg --show-keys --with-colons "$KEY_TMP" 2>/dev/null | awk -F: '/^fpr/{print $10; exit}')
[ "$FPR" = "$MOZILLA_KEY_FPR" ] || die "la clave de Mozilla no coincide ($FPR)"
install -m 0644 "$KEY_TMP" /etc/apt/keyrings/packages.mozilla.org.asc
rm -f "$KEY_TMP"
echo "deb [signed-by=/etc/apt/keyrings/packages.mozilla.org.asc] https://packages.mozilla.org/apt mozilla main" \
  > /etc/apt/sources.list.d/mozilla.list
printf 'Package: *\nPin: origin packages.mozilla.org\nPin-Priority: 1000\n' > /etc/apt/preferences.d/mozilla
apt-get update -q
mapfile -t FF_L10N < <(available firefox-l10n-es-es)
apt-get "${APT_OPTS[@]}" install firefox "${FF_L10N[@]}"

log "Añadiendo Flathub"
flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

# ---------------------------------------------------------------- language
log "Idioma: español"
sed -i 's/^# *es_ES.UTF-8 UTF-8/es_ES.UTF-8 UTF-8/' /etc/locale.gen
locale-gen es_ES.UTF-8
update-locale LANG=es_ES.UTF-8 LANGUAGE=es_ES:es

# ---------------------------------------------------------------- theme
log "Tema Fluent (estilo Windows 11)"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fetch_commit() {  # url commit dir
  git init -q "$3"
  git -C "$3" fetch -q --depth 1 "$1" "$2"
  git -C "$3" checkout -q FETCH_HEAD
}
fetch_commit https://github.com/vinceliuice/Fluent-kde "$FLUENT_KDE_COMMIT" "$WORK/fluent-kde"
fetch_commit https://github.com/vinceliuice/Fluent-icon-theme "$FLUENT_ICONS_COMMIT" "$WORK/fluent-icons"

install -d /usr/share/plasma/desktoptheme /usr/share/color-schemes /usr/share/aurorae/themes
cp -r "$WORK/fluent-kde/plasma/desktoptheme/Fluent-round-light" /usr/share/plasma/desktoptheme/
cp "$WORK/fluent-kde/color-schemes/FluentLight.colors" /usr/share/color-schemes/
cp -r "$WORK/fluent-kde/aurorae/Fluent-round-light" /usr/share/aurorae/themes/
(cd "$WORK/fluent-icons" && ./install.sh >/dev/null)
(cd "$WORK/fluent-icons/cursors" && ./install.sh >/dev/null)

# ---------------------------------------------------------------- Chamows files
log "Copiando archivos de Chamows"
cp -r --no-preserve=ownership "$REPO_DIR/rootfs/." /
chmod 0440 /etc/sudoers.d/chamows-store
visudo -cf /etc/sudoers.d/chamows-store >/dev/null || die "sudoers no válido"
chmod 0755 /usr/lib/chamows/*.sh /usr/lib/chamows/chamows-pkg /usr/lib/chamows/chamows-set-password /usr/bin/chamows-store

rm -rf /usr/lib/chamows/store
install -d -m 0755 /usr/lib/chamows/store
cp -r "$REPO_DIR/store/chamows_store" /usr/lib/chamows/store/
find /usr/lib/chamows/store -name '__pycache__' -prune -exec rm -rf {} +
install -m 0644 "$REPO_DIR/store/catalog.json" /usr/share/chamows/catalog.json
PYTHONPATH=/usr/lib/chamows/store python3 -c "from chamows_store.catalog import load_catalog; load_catalog()" \
  || die "el catálogo no es válido"

rsvg-convert -w 1920 -h 1080 /usr/share/chamows/wallpaper.svg -o /usr/share/chamows/wallpaper.png
gtk-update-icon-cache -q -f /usr/share/icons/hicolor 2>/dev/null || true

# ---------------------------------------------------------------- user
log "Usuario $USER_NAME"
if ! id "$USER_NAME" >/dev/null 2>&1; then
  useradd -m -s /bin/bash -c "Chamows" -G sudo "$USER_NAME"
  passwd -l "$USER_NAME" >/dev/null   # unlocked on first boot by chamows-set-password
fi
install -d -m 0700 -o "$USER_NAME" -g "$USER_NAME" "/home/$USER_NAME/.config" "/home/$USER_NAME/.config/tigervnc"

# ---------------------------------------------------------------- remote access
log "Acceso remoto: navegador (nginx + noVNC) y RDP (xrdp)"
rm -f /etc/nginx/sites-enabled/default
ln -sf /etc/nginx/sites-available/chamows /etc/nginx/sites-enabled/chamows

# xrdp: one session type that forwards to the shared Chamows desktop.
[ -f /etc/xrdp/xrdp.ini.orig ] || cp /etc/xrdp/xrdp.ini /etc/xrdp/xrdp.ini.orig
python3 - /etc/xrdp/xrdp.ini.orig /etc/xrdp/xrdp.ini <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
keep = {"Globals", "Logging", "LoggingPerLogger", "Channels"}
out, section = [], None
for line in open(src, encoding="utf-8"):
    m = re.match(r"^\[([^\]]+)\]", line)
    if m:
        section = m.group(1)
    if section is None or section in keep:
        out.append(line)
out.append("""
[Chamows]
name=Chamows
lib=libvnc.so
ip=127.0.0.1
port=5901
username=na
password=ask
""")
open(dst, "w", encoding="utf-8").writelines(out)
PY
adduser xrdp ssl-cert >/dev/null 2>&1 || true

# ---------------------------------------------------------------- services
log "Activando servicios"
systemctl daemon-reload
systemctl disable sddm >/dev/null 2>&1 || true
systemctl set-default multi-user.target
systemctl enable chamows-firstboot.service chamows-desktop.service chamows-websockify.service nginx xrdp

if [ "$RUN_NOW" -eq 1 ]; then
  log "Arrancando Chamows"
  systemctl start chamows-firstboot.service
  systemctl restart nginx xrdp
  systemctl start chamows-desktop.service chamows-websockify.service
  if [ -f /root/chamows-password.txt ]; then
    echo "Contraseña inicial (usuario $USER_NAME): $(cat /root/chamows-password.txt)"
  fi
else
  # Building an image: don't keep services running with build-time state.
  systemctl stop nginx xrdp >/dev/null 2>&1 || true
fi

log "Chamows instalado"
