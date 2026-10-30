import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "store"))

from chamows_store import backends as be  # noqa: E402
from chamows_store.catalog import CatalogError, load_catalog, parse_app  # noqa: E402

CATALOG = ROOT / "store" / "catalog.json"
HELPER = ROOT / "rootfs" / "usr" / "lib" / "chamows" / "chamows-pkg"


class CatalogTests(unittest.TestCase):
    def test_shipped_catalog_is_valid(self):
        apps = load_catalog(CATALOG)
        self.assertGreaterEqual(len(apps), 10)
        self.assertEqual({a.source for a in apps}, {"apt", "flatpak", "wine"})

    def test_rejects_unknown_source(self):
        with self.assertRaises(CatalogError):
            parse_app({"id": "x", "name": "X", "description": "d", "category": "c",
                       "source": "snap", "package": "x"})

    def test_rejects_apt_name_with_shell_chars(self):
        with self.assertRaises(CatalogError):
            parse_app({"id": "x", "name": "X", "description": "d", "category": "c",
                       "source": "apt", "package": "vlc; rm -rf /"})

    def test_wine_requires_https_and_hash(self):
        base = {"id": "w", "name": "W", "description": "d", "category": "c", "source": "wine",
                "package": "w", "installed_path": "Program Files/W/w.exe",
                "sha256": "a" * 64, "url": "https://example.com/w.exe"}
        parse_app(base)
        with self.assertRaises(CatalogError):
            parse_app({**base, "url": "http://example.com/w.exe"})
        with self.assertRaises(CatalogError):
            parse_app({**base, "sha256": "abc"})
        with self.assertRaises(CatalogError):
            parse_app({**base, "installed_path": "../../etc/passwd"})

    def test_duplicate_ids_rejected(self):
        app = {"id": "a", "name": "A", "description": "d", "category": "c",
               "source": "apt", "package": "vlc"}
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
            json.dump({"apps": [app, app]}, fh)
        try:
            with self.assertRaises(CatalogError):
                load_catalog(Path(fh.name))
        finally:
            os.unlink(fh.name)


class FlatpakTests(unittest.TestCase):
    def test_install_cmd_is_user_scoped(self):
        cmd = be.flatpak_install_cmd("com.spotify.Client")
        self.assertIn("--user", cmd)
        self.assertEqual(cmd[-1], "com.spotify.Client")

    def test_install_cmd_rejects_bad_id(self):
        with self.assertRaises(ValueError):
            be.flatpak_install_cmd("--system")

    def test_parse_search(self):
        out = ("org.mozilla.firefox\tFirefox\tFast, Private & Safe Web Browser\n"
               "org.mozilla.firefox\tFirefox\tduplicate\n"
               "No matches found\n"
               "org.gimp.GIMP\tGIMP\n")
        self.assertEqual(be.parse_flatpak_search(out), [
            ("org.mozilla.firefox", "Firefox", "Fast, Private & Safe Web Browser"),
            ("org.gimp.GIMP", "GIMP", ""),
        ])

    def test_parse_list(self):
        self.assertEqual(be.parse_flatpak_list("com.a.B\n\nApplication ID\n"), {"com.a.B"})


class WineTests(unittest.TestCase):
    def test_msi_uses_msiexec(self):
        self.assertEqual(be.wine_installer_cmd("/x/a.msi")[:3], ["wine", "msiexec", "/i"])
        self.assertEqual(be.wine_installer_cmd("/x/a.exe", ("/S",)), ["wine", "/x/a.exe", "/S"])

    def _fake_opener(self, payload):
        class Resp(io.BytesIO):
            headers = {"Content-Length": str(len(payload))}

        return lambda url, timeout: Resp(payload)

    def test_download_verified_ok(self):
        payload = b"MZ fake installer"
        sha = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as d:
            path = be.download_verified("https://x/setup.exe", sha, Path(d), opener=self._fake_opener(payload))
            self.assertEqual(path.read_bytes(), payload)
            self.assertEqual(path.name, "setup.exe")

    def test_download_bad_hash_leaves_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(be.ChecksumError):
                be.download_verified("https://x/setup.exe", "0" * 64, Path(d), opener=self._fake_opener(b"evil"))
            self.assertEqual(list(Path(d).iterdir()), [])

    def test_download_rejects_http(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError):
                be.download_verified("http://x/setup.exe", "0" * 64, Path(d))


class HelperTests(unittest.TestCase):
    def run_helper(self, *args):
        env = {**os.environ, "CHAMOWS_PKG_CATALOG_FOR_TESTS": str(CATALOG), "CHAMOWS_PKG_DRY_RUN": "1"}
        return subprocess.run([sys.executable, "-I", str(HELPER), *args],
                              capture_output=True, text=True, env=env)

    @unittest.skipIf(os.getuid() == 0, "the helper ignores test overrides when run as root")
    def test_allows_catalog_package(self):
        res = self.run_helper("install", "vlc")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("apt-get install -y -q vlc", res.stdout)

    @unittest.skipIf(os.getuid() == 0, "the helper ignores test overrides when run as root")
    def test_rejects_other_package(self):
        self.assertEqual(self.run_helper("install", "openssh-server").returncode, 3)
        self.assertEqual(self.run_helper("install", "-oAPT::Something").returncode, 3)

    def test_rejects_bad_action(self):
        self.assertEqual(self.run_helper("purge", "vlc").returncode, 2)


if __name__ == "__main__":
    unittest.main()
