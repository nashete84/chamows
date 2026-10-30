"""Tienda Chamows: the Qt window."""

from __future__ import annotations

import os
import shutil
import sys
import threading
from pathlib import Path

from PyQt5.QtCore import QObject, QProcess, QProcessEnvironment, Qt, pyqtSignal
from PyQt5.QtGui import QFont, QIcon
from PyQt5.QtWidgets import (
    QApplication, QFileDialog, QFrame, QHBoxLayout, QLabel, QLineEdit,
    QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QPlainTextEdit,
    QProgressBar, QPushButton, QScrollArea, QSplitter, QStackedWidget,
    QVBoxLayout, QWidget,
)

from . import backends as be
from .catalog import App, CatalogError, categories, load_catalog

SOURCE_LABEL = {"apt": "Linux", "flatpak": "Flathub", "wine": "Windows (.exe)"}
ALL = "Todas las apps"
FLATHUB = "Buscar en Flathub"
EXE = "Instalar un .exe"


class Runner(QObject):
    """Runs a list of steps one after another.

    A step is ("cmd", argv, extra_env) run with QProcess, or ("py", fn) run in
    a thread. The first failing step stops the run.
    """

    log = pyqtSignal(str)
    done = pyqtSignal(bool, str)
    _py_finished = pyqtSignal(object)

    def __init__(self) -> None:
        super().__init__()
        self.steps: list = []
        self.title = ""
        self.proc: QProcess | None = None
        self._py_finished.connect(self._on_py_finished)

    @property
    def busy(self) -> bool:
        return bool(self.title)

    def start(self, title: str, steps: list) -> None:
        self.title, self.steps = title, list(steps)
        self.log.emit(f"\n=== {title} ===")
        self._next()

    def _next(self) -> None:
        if not self.steps:
            title, self.title = self.title, ""
            self.done.emit(True, title)
            return
        step = self.steps.pop(0)
        if step[0] == "cmd":
            self._run_cmd(step[1], step[2] if len(step) > 2 else {})
        else:
            fn = step[1]

            def work() -> None:
                try:
                    fn()
                    self._py_finished.emit(None)
                except Exception as exc:  # noqa: BLE001 - shown to the user
                    self._py_finished.emit(exc)

            threading.Thread(target=work, daemon=True).start()

    def _run_cmd(self, argv: list[str], extra_env: dict) -> None:
        self.log.emit("$ " + " ".join(argv))
        proc = QProcess(self)
        env = QProcessEnvironment.systemEnvironment()
        for key, value in extra_env.items():
            env.insert(key, value)
        proc.setProcessEnvironment(env)
        proc.setProcessChannelMode(QProcess.MergedChannels)
        proc.readyReadStandardOutput.connect(
            lambda: self.log.emit(bytes(proc.readAllStandardOutput()).decode(errors="replace").rstrip())
        )
        proc.finished.connect(lambda code, status: self._on_cmd_finished(code, status))
        proc.errorOccurred.connect(self._on_cmd_error)
        self.proc = proc
        proc.start(argv[0], argv[1:])

    def _on_cmd_error(self, error) -> None:
        if error == QProcess.FailedToStart:
            self._fail("no se pudo ejecutar el programa")

    def _on_cmd_finished(self, code: int, status) -> None:
        if status != QProcess.NormalExit or code != 0:
            self._fail(f"el comando terminó con código {code}")
        else:
            self._next()

    def _on_py_finished(self, exc) -> None:
        if exc is None:
            self._next()
        else:
            self._fail(str(exc))

    def _fail(self, reason: str) -> None:
        if not self.title:
            return
        title, self.title, self.steps = self.title, "", []
        self.log.emit(f"ERROR: {reason}")
        self.done.emit(False, f"{title}: {reason}")


class AppCard(QFrame):
    def __init__(self, store: "StoreWindow", app: App, installed: bool) -> None:
        super().__init__()
        self.setObjectName("card")
        row = QHBoxLayout(self)
        icon = QLabel()
        icon.setPixmap(QIcon.fromTheme(app.icon, QIcon.fromTheme("package-x-generic")).pixmap(48, 48))
        row.addWidget(icon)

        text = QVBoxLayout()
        title = QLabel(f"<b>{_esc(app.name)}</b>  <span style='color:#6b6b6b'>· {SOURCE_LABEL[app.source]}</span>")
        desc = QLabel(_esc(app.description))
        desc.setWordWrap(True)
        text.addWidget(title)
        text.addWidget(desc)
        row.addLayout(text, 1)

        if installed:
            open_btn = QPushButton("Abrir")
            open_btn.clicked.connect(lambda: store.open_app(app))
            remove_btn = QPushButton("Desinstalar")
            remove_btn.clicked.connect(lambda: store.remove_app(app))
            remove_btn.setEnabled(not store.runner.busy)
            row.addWidget(open_btn)
            row.addWidget(remove_btn)
        else:
            install_btn = QPushButton("Instalar")
            install_btn.setObjectName("primary")
            install_btn.clicked.connect(lambda: store.install_app(app))
            install_btn.setEnabled(not store.runner.busy)
            row.addWidget(install_btn)


def _esc(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


STYLE = """
QMainWindow, QWidget#page { background: #f3f3f3; }
QListWidget#nav { background: #ebebeb; border: none; font-size: 14px; padding: 8px; }
QListWidget#nav::item { padding: 8px 10px; border-radius: 6px; }
QListWidget#nav::item:selected { background: #ffffff; color: #000; }
QFrame#card { background: #ffffff; border: 1px solid #e5e5e5; border-radius: 8px; }
QPushButton { padding: 6px 14px; border-radius: 4px; border: 1px solid #d0d0d0; background: #fbfbfb; }
QPushButton#primary { background: #0067c0; color: white; border: none; }
QPushButton:disabled { color: #9a9a9a; background: #f0f0f0; }
QLineEdit { padding: 6px 10px; border-radius: 4px; border: 1px solid #d0d0d0; background: white; }
"""


class StoreWindow(QMainWindow):
    def __init__(self, apps: list[App]) -> None:
        super().__init__()
        # Without Wine (e.g. ARM machines) hide the Windows programs.
        self.has_wine = shutil.which("wine") is not None
        self.apps = apps if self.has_wine else [a for a in apps if a.source != "wine"]
        apps = self.apps
        self.flathub_results: list[App] = []
        self.flatpak_ids: set[str] = set()
        self.runner = Runner()
        self.runner.log.connect(self._append_log)
        self.runner.done.connect(self._on_done)

        self.setWindowTitle("Tienda Chamows")
        self.setWindowIcon(QIcon.fromTheme("chamows-store", QIcon.fromTheme("system-software-install")))
        self.resize(1000, 700)
        self.setStyleSheet(STYLE)

        self.nav = QListWidget()
        self.nav.setObjectName("nav")
        self.nav.setFixedWidth(220)
        for name in [ALL, *categories(apps), FLATHUB, *([EXE] if self.has_wine else [])]:
            QListWidgetItem(name, self.nav)
        self.nav.currentTextChanged.connect(self._on_nav)

        # Catalog page
        self.filter_box = QLineEdit()
        self.filter_box.setPlaceholderText("Buscar en el catálogo…")
        self.filter_box.textChanged.connect(lambda _: self.refresh())
        self.catalog_list = QVBoxLayout()
        catalog_page = self._page([self.filter_box], self.catalog_list)

        # Flathub page
        self.flathub_box = QLineEdit()
        self.flathub_box.setPlaceholderText("Busca entre miles de apps de Flathub (ej. 'firefox', 'blender')")
        self.flathub_box.returnPressed.connect(self.search_flathub)
        search_btn = QPushButton("Buscar")
        search_btn.clicked.connect(self.search_flathub)
        self.flathub_list = QVBoxLayout()
        flathub_page = self._page([self.flathub_box, search_btn], self.flathub_list)

        # .exe page
        exe_page = QWidget()
        exe_page.setObjectName("page")
        exe_layout = QVBoxLayout(exe_page)
        exe_text = QLabel(
            "<h2>Instalar un programa de Windows</h2>"
            "<p>Elige un instalador <b>.exe</b> o <b>.msi</b> que hayas descargado. "
            "Chamows lo abrirá con Wine, que traduce los programas de Windows para que funcionen aquí.</p>"
            "<p>La mayoría de programas sencillos funcionan. Juegos con anti-trampas y "
            "programas que necesitan drivers especiales normalmente no.</p>"
            "<p>También puedes hacer doble clic en un .exe desde el explorador de archivos.</p>"
        )
        exe_text.setWordWrap(True)
        exe_btn = QPushButton("Elegir archivo .exe o .msi…")
        exe_btn.setObjectName("primary")
        exe_btn.clicked.connect(self.install_exe_file)
        uninstall_btn = QPushButton("Abrir desinstalador de programas de Windows")
        uninstall_btn.clicked.connect(lambda: self._detached(be.wine_uninstaller_cmd()))
        exe_layout.addWidget(exe_text)
        exe_layout.addWidget(exe_btn, 0, Qt.AlignLeft)
        exe_layout.addWidget(uninstall_btn, 0, Qt.AlignLeft)
        exe_layout.addStretch(1)

        self.stack = QStackedWidget()
        for page in (catalog_page, flathub_page, exe_page):
            self.stack.addWidget(page)

        # Status + log
        self.progress = QProgressBar()
        self.progress.setRange(0, 0)
        self.progress.setVisible(False)
        self.status = QLabel("Listo")
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setFont(QFont("monospace", 9))
        self.log_view.setVisible(False)
        log_btn = QPushButton("Ver detalles")
        log_btn.setCheckable(True)
        log_btn.toggled.connect(self.log_view.setVisible)

        status_row = QHBoxLayout()
        status_row.addWidget(self.status, 1)
        status_row.addWidget(self.progress)
        status_row.addWidget(log_btn)

        right = QWidget()
        right_layout = QVBoxLayout(right)
        splitter = QSplitter(Qt.Vertical)
        splitter.addWidget(self.stack)
        splitter.addWidget(self.log_view)
        right_layout.addWidget(splitter, 1)
        right_layout.addLayout(status_row)

        root = QWidget()
        root_layout = QHBoxLayout(root)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.addWidget(self.nav)
        root_layout.addWidget(right, 1)
        self.setCentralWidget(root)

        self.refresh_installed()
        self.nav.setCurrentRow(0)

    # ----- layout helpers -----

    def _page(self, header_widgets: list[QWidget], list_layout: QVBoxLayout) -> QWidget:
        page = QWidget()
        page.setObjectName("page")
        layout = QVBoxLayout(page)
        header = QHBoxLayout()
        for w in header_widgets:
            header.addWidget(w, 1 if isinstance(w, QLineEdit) else 0)
        layout.addLayout(header)
        holder = QWidget()
        holder.setObjectName("page")
        holder.setLayout(list_layout)
        list_layout.setAlignment(Qt.AlignTop)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        scroll.setWidget(holder)
        layout.addWidget(scroll, 1)
        return page

    @staticmethod
    def _clear(layout: QVBoxLayout) -> None:
        while layout.count():
            item = layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()

    # ----- views -----

    def _on_nav(self, name: str) -> None:
        if name == FLATHUB:
            self.stack.setCurrentIndex(1)
        elif name == EXE:
            self.stack.setCurrentIndex(2)
        else:
            self.stack.setCurrentIndex(0)
        self.refresh()

    def visible_apps(self) -> list[App]:
        current = self.nav.currentItem().text() if self.nav.currentItem() else ALL
        term = self.filter_box.text().strip().lower()
        apps = self.apps if current in (ALL, FLATHUB, EXE) else [a for a in self.apps if a.category == current]
        if term:
            apps = [a for a in apps if term in a.name.lower() or term in a.description.lower()]
        return apps

    def refresh_installed(self) -> None:
        self.flatpak_ids = be.installed_flatpaks()
        self.refresh()

    def refresh(self) -> None:
        self._clear(self.catalog_list)
        for app in self.visible_apps():
            self.catalog_list.addWidget(AppCard(self, app, be.is_installed(app, self.flatpak_ids)))
        if not self.visible_apps():
            self.catalog_list.addWidget(QLabel("No hay apps que coincidan."))
        self._clear(self.flathub_list)
        for app in self.flathub_results:
            self.flathub_list.addWidget(AppCard(self, app, app.package in self.flatpak_ids))

    # ----- actions -----

    def _start(self, title: str, steps: list) -> None:
        if self.runner.busy:
            QMessageBox.information(self, "Tienda Chamows", "Espera a que termine la operación en curso.")
            return
        self.status.setText(title + "…")
        self.progress.setVisible(True)
        self.runner.start(title, steps)
        self.refresh()

    def install_app(self, app: App) -> None:
        title = f"Instalando {app.name}"
        if app.source == "flatpak":
            steps = [("cmd", be.flatpak_ensure_remote_cmd()), ("cmd", be.flatpak_install_cmd(app.package))]
        elif app.source == "apt":
            steps = [("cmd", be.apt_cmd("install", app.package))]
        else:
            steps = self._wine_prefix_steps()
            holder: dict[str, Path] = {}

            def report(done: int, total: int) -> None:
                # Log about once per MB so the details panel stays readable.
                if done % (1 << 20) < (1 << 16) or done == total:
                    self.runner.log.emit(f"Descargado {done // 1024} KB de {total // 1024} KB")

            def fetch() -> None:
                self.runner.log.emit(f"Descargando {app.url}")
                holder["path"] = be.download_verified(app.url, app.sha256, be.cache_dir(), progress=report)
                self.runner.log.emit("Firma SHA-256 correcta")

            steps.append(("py", fetch))
            steps.append(("py", lambda: self._queue_wine_install(holder["path"], app.args)))
        self._start(title, steps)

    def _queue_wine_install(self, path: Path, args: tuple[str, ...]) -> None:
        # Runs in the worker thread: only appends to the step list.
        self.runner.steps.insert(0, ("cmd", be.wine_installer_cmd(str(path), args), be.WINE_QUIET_ENV))

    def _wine_prefix_steps(self) -> list:
        if be.wine_prefix_ready():
            return []
        return [("cmd", be.wine_init_cmd(), be.WINE_QUIET_ENV)]

    def remove_app(self, app: App) -> None:
        if app.source == "wine":
            self._detached(be.wine_uninstaller_cmd())
            return
        answer = QMessageBox.question(self, "Desinstalar", f"¿Desinstalar {app.name}?")
        if answer != QMessageBox.Yes:
            return
        if app.source == "flatpak":
            steps = [("cmd", be.flatpak_remove_cmd(app.package))]
        else:
            steps = [("cmd", be.apt_cmd("remove", app.package))]
        self._start(f"Desinstalando {app.name}", steps)

    def open_app(self, app: App) -> None:
        if app.source == "flatpak":
            self._detached(be.flatpak_run_cmd(app.package))
        elif app.source == "wine":
            self._detached(be.wine_run_cmd(app), be.WINE_QUIET_ENV)
        elif shutil.which(app.package):
            self._detached([app.package])
        else:
            QMessageBox.information(self, app.name, "Ábrela desde el menú Inicio.")

    def _detached(self, argv: list[str], extra_env: dict | None = None) -> None:
        proc = QProcess(self)
        if extra_env:
            env = QProcessEnvironment.systemEnvironment()
            for k, v in extra_env.items():
                env.insert(k, v)
            proc.setProcessEnvironment(env)
        proc.setProgram(argv[0])
        proc.setArguments(argv[1:])
        if not proc.startDetached()[0]:
            QMessageBox.warning(self, "Tienda Chamows", f"No se pudo abrir {argv[0]}.")

    def install_exe_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Elige un instalador de Windows", str(Path.home() / "Descargas"),
            "Programas de Windows (*.exe *.msi *.EXE *.MSI)",
        )
        if not path:
            return
        steps = self._wine_prefix_steps()
        steps.append(("cmd", be.wine_installer_cmd(path), be.WINE_QUIET_ENV))
        self._start(f"Instalando {Path(path).name}", steps)

    def search_flathub(self) -> None:
        term = self.flathub_box.text().strip()
        if not term:
            return
        if self.runner.busy:
            QMessageBox.information(self, "Tienda Chamows", "Espera a que termine la operación en curso.")
            return
        self.status.setText("Buscando en Flathub…")
        self.progress.setVisible(True)
        proc = QProcess(self)
        proc.setProcessChannelMode(QProcess.SeparateChannels)

        def finished(_code: int, _status) -> None:
            out = bytes(proc.readAllStandardOutput()).decode(errors="replace")
            self.flathub_results = [
                App(id=app_id.lower(), name=name, description=desc or app_id, category="Flathub",
                    source="flatpak", package=app_id, icon=app_id)
                for app_id, name, desc in be.parse_flatpak_search(out)[:50]
            ]
            self.progress.setVisible(False)
            self.status.setText(f"{len(self.flathub_results)} resultados para '{term}'")
            self.refresh_installed()

        proc.finished.connect(finished)
        # Refresh the app list from Flathub first, then search.
        cmd = " && ".join([
            _sh(be.flatpak_ensure_remote_cmd()),
            _sh(be.flatpak_refresh_appstream_cmd()) + " >/dev/null 2>&1",
            _sh(be.flatpak_search_cmd(term)),
        ])
        proc.start("sh", ["-c", cmd])

    # ----- runner callbacks -----

    def _append_log(self, text: str) -> None:
        if text:
            self.log_view.appendPlainText(text)

    def _on_done(self, ok: bool, message: str) -> None:
        self.progress.setVisible(False)
        if ok:
            self.status.setText(f"{message}: listo")
        else:
            self.status.setText("Error")
            self.log_view.setVisible(True)
            QMessageBox.warning(self, "Tienda Chamows", f"No se pudo completar la operación.\n\n{message}")
        self.refresh_installed()


def _sh(argv: list[str]) -> str:
    import shlex
    return " ".join(shlex.quote(a) for a in argv)


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Tienda Chamows")
    app.setDesktopFileName("chamows-store")
    try:
        apps = load_catalog()
    except CatalogError as exc:
        QMessageBox.critical(None, "Tienda Chamows", str(exc))
        return 1
    window = StoreWindow(apps)
    window.show()
    return app.exec_()


if __name__ == "__main__":
    os.environ.setdefault("QT_QPA_PLATFORMTHEME", "kde")
    sys.exit(main())
