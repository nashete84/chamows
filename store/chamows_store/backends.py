"""Commands and checks for each install source (APT, Flatpak, Wine).

Everything here is plain Python without Qt so it can be unit-tested.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path
from typing import Callable

from .catalog import FLATPAK_ID_RE, App

FLATHUB_URL = "https://dl.flathub.org/repo/flathub.flatpakrepo"
APT_HELPER = "/usr/lib/chamows/chamows-pkg"
# Skip Wine's Mono/Gecko download prompts when the prefix is created.
WINE_QUIET_ENV = {"WINEDLLOVERRIDES": "mscoree,mshtml=", "WINEDEBUG": "-all"}


# ---------- Flatpak ----------

def flatpak_ensure_remote_cmd() -> list[str]:
    return ["flatpak", "remote-add", "--user", "--if-not-exists", "flathub", FLATHUB_URL]


def flatpak_refresh_appstream_cmd() -> list[str]:
    return ["flatpak", "update", "--user", "--appstream", "flathub"]


def flatpak_install_cmd(app_id: str) -> list[str]:
    if not FLATPAK_ID_RE.match(app_id):
        raise ValueError(f"id de Flatpak no válido: {app_id}")
    return ["flatpak", "install", "--user", "-y", "--noninteractive", "flathub", app_id]


def flatpak_remove_cmd(app_id: str) -> list[str]:
    if not FLATPAK_ID_RE.match(app_id):
        raise ValueError(f"id de Flatpak no válido: {app_id}")
    return ["flatpak", "uninstall", "--user", "-y", "--noninteractive", app_id]


def flatpak_run_cmd(app_id: str) -> list[str]:
    return ["flatpak", "run", app_id]


def flatpak_search_cmd(term: str) -> list[str]:
    return ["flatpak", "search", "--columns=application,name,description", "--", term]


def parse_flatpak_list(text: str) -> set[str]:
    """Parse `flatpak list --app --columns=application`."""
    return {line.strip() for line in text.splitlines() if FLATPAK_ID_RE.match(line.strip())}


def parse_flatpak_search(text: str) -> list[tuple[str, str, str]]:
    """Parse `flatpak search --columns=application,name,description` output.

    Returns (app_id, name, description). Lines that do not start with a valid
    app id (headers, "No matches found") are skipped.
    """
    results: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    for line in text.splitlines():
        cols = [c.strip() for c in line.split("\t")]
        if len(cols) < 2 or not FLATPAK_ID_RE.match(cols[0]) or cols[0] in seen:
            continue
        seen.add(cols[0])
        desc = cols[2] if len(cols) > 2 else ""
        results.append((cols[0], cols[1], desc))
    return results


def installed_flatpaks() -> set[str]:
    if not shutil.which("flatpak"):
        return set()
    out = subprocess.run(
        ["flatpak", "list", "--app", "--columns=application"],
        capture_output=True, text=True, check=False,
    )
    return parse_flatpak_list(out.stdout)


# ---------- APT ----------

def apt_cmd(action: str, package: str) -> list[str]:
    if action not in ("install", "remove"):
        raise ValueError(action)
    # The helper only accepts packages listed in the system catalog.
    return ["sudo", "-n", APT_HELPER, action, package]


def apt_is_installed(package: str) -> bool:
    out = subprocess.run(
        ["dpkg-query", "-W", "-f=${Status}", package],
        capture_output=True, text=True, check=False,
    )
    return out.returncode == 0 and out.stdout.strip().endswith("install ok installed")


# ---------- Wine ----------

def wine_prefix() -> Path:
    return Path(os.environ.get("WINEPREFIX", Path.home() / ".wine"))


def wine_prefix_ready(prefix: Path | None = None) -> bool:
    return ((prefix or wine_prefix()) / "system.reg").exists()


def wine_init_cmd() -> list[str]:
    return ["wineboot", "--init"]


def wine_installer_cmd(path: str, args: tuple[str, ...] = ()) -> list[str]:
    if path.lower().endswith(".msi"):
        return ["wine", "msiexec", "/i", path, *args]
    return ["wine", path, *args]


def wine_app_path(app: App, prefix: Path | None = None) -> Path:
    assert app.installed_path
    return (prefix or wine_prefix()) / "drive_c" / app.installed_path


def wine_run_cmd(app: App) -> list[str]:
    return ["wine", str(wine_app_path(app))]


def wine_uninstaller_cmd() -> list[str]:
    return ["wine", "uninstaller"]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class ChecksumError(Exception):
    pass


def download_verified(
    url: str,
    expected_sha256: str,
    dest_dir: Path,
    progress: Callable[[int, int], None] | None = None,
    opener: Callable = urllib.request.urlopen,
) -> Path:
    """Download `url` into `dest_dir` and check its SHA-256.

    The file is written under a temporary name and only renamed once the hash
    matches, so a bad download never sits at the final path.
    """
    if not url.startswith("https://"):
        raise ValueError("solo se permiten descargas https")
    dest_dir.mkdir(parents=True, exist_ok=True)
    name = Path(urllib.request.url2pathname(url.rsplit("/", 1)[-1])).name or "installer.exe"
    final = dest_dir / name
    if final.exists() and sha256_file(final) == expected_sha256:
        return final

    fd, tmp_name = tempfile.mkstemp(dir=dest_dir, suffix=".part")
    tmp = Path(tmp_name)
    try:
        digest = hashlib.sha256()
        with os.fdopen(fd, "wb") as out, opener(url, timeout=60) as resp:
            total = int(resp.headers.get("Content-Length") or 0)
            done = 0
            while True:
                chunk = resp.read(1 << 16)
                if not chunk:
                    break
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        actual = digest.hexdigest()
        if actual != expected_sha256:
            raise ChecksumError(
                f"la descarga no coincide con el catálogo (esperado {expected_sha256[:12]}…, recibido {actual[:12]}…)"
            )
        tmp.replace(final)
        return final
    finally:
        if tmp.exists():
            tmp.unlink()


# ---------- Common ----------

def is_installed(app: App, flatpak_ids: set[str]) -> bool:
    if app.source == "flatpak":
        return app.package in flatpak_ids
    if app.source == "apt":
        return apt_is_installed(app.package)
    if app.source == "wine":
        return wine_app_path(app).exists()
    return False


def cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "chamows-store"
