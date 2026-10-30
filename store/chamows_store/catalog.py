"""Load and validate the Chamows app catalog."""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

SOURCES = ("apt", "flatpak", "wine")

APT_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9+.-]+$")
FLATPAK_ID_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*(\.[A-Za-z0-9_-]+){2,}$")
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

SYSTEM_CATALOG = Path("/usr/share/chamows/catalog.json")
REPO_CATALOG = Path(__file__).resolve().parent.parent / "catalog.json"


class CatalogError(ValueError):
    pass


@dataclass(frozen=True)
class App:
    id: str
    name: str
    description: str
    category: str
    source: str
    package: str
    icon: str = "package-x-generic"
    url: str | None = None
    sha256: str | None = None
    args: tuple[str, ...] = field(default_factory=tuple)
    installed_path: str | None = None


def default_catalog_path() -> Path:
    env = os.environ.get("CHAMOWS_CATALOG")
    if env:
        return Path(env)
    if SYSTEM_CATALOG.exists():
        return SYSTEM_CATALOG
    return REPO_CATALOG


def _require_str(raw: dict, key: str, where: str) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value.strip():
        raise CatalogError(f"{where}: falta el campo '{key}'")
    return value


def parse_app(raw: dict) -> App:
    if not isinstance(raw, dict):
        raise CatalogError("cada app debe ser un objeto")
    app_id = _require_str(raw, "id", "app")
    where = f"app '{app_id}'"
    if not SLUG_RE.match(app_id):
        raise CatalogError(f"{where}: id no válido")
    source = _require_str(raw, "source", where)
    if source not in SOURCES:
        raise CatalogError(f"{where}: origen '{source}' desconocido")
    package = _require_str(raw, "package", where)

    url = sha256 = installed_path = None
    args: tuple[str, ...] = ()
    if source == "apt" and not APT_NAME_RE.match(package):
        raise CatalogError(f"{where}: nombre de paquete APT no válido")
    if source == "flatpak" and not FLATPAK_ID_RE.match(package):
        raise CatalogError(f"{where}: id de Flatpak no válido")
    if source == "wine":
        url = _require_str(raw, "url", where)
        if not url.startswith("https://"):
            raise CatalogError(f"{where}: la descarga debe ser https")
        sha256 = _require_str(raw, "sha256", where).lower()
        if not SHA256_RE.match(sha256):
            raise CatalogError(f"{where}: sha256 no válido")
        installed_path = _require_str(raw, "installed_path", where)
        parts = PurePosixPath(installed_path).parts
        if installed_path.startswith("/") or ".." in parts:
            raise CatalogError(f"{where}: installed_path debe ser relativo a C:")
        raw_args = raw.get("args", [])
        if not isinstance(raw_args, list) or not all(isinstance(a, str) for a in raw_args):
            raise CatalogError(f"{where}: args debe ser una lista de textos")
        args = tuple(raw_args)

    return App(
        id=app_id,
        name=_require_str(raw, "name", where),
        description=_require_str(raw, "description", where),
        category=_require_str(raw, "category", where),
        source=source,
        package=package,
        icon=raw.get("icon") or "package-x-generic",
        url=url,
        sha256=sha256,
        args=args,
        installed_path=installed_path,
    )


def load_catalog(path: Path | None = None) -> list[App]:
    path = path or default_catalog_path()
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CatalogError(f"no se pudo leer el catálogo {path}: {exc}") from exc
    raw_apps = data.get("apps") if isinstance(data, dict) else None
    if not isinstance(raw_apps, list):
        raise CatalogError("el catálogo no tiene lista 'apps'")
    apps = [parse_app(raw) for raw in raw_apps]
    seen: set[str] = set()
    for app in apps:
        if app.id in seen:
            raise CatalogError(f"id repetido: {app.id}")
        seen.add(app.id)
    return apps


def categories(apps: list[App]) -> list[str]:
    return sorted({a.category for a in apps})
