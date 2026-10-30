# Chamows

Un sistema operativo de 32 bits hecho desde cero, con aspecto de Windows 11.
No usa Linux ni Windows por debajo: el núcleo, el escritorio, las ventanas,
la tienda y las apps están escritos aquí, en C y ensamblador.

## Qué hace hoy

- Arranca en cualquier PC x86 emulado (QEMU, v86 en el navegador, Bochs).
- Escritorio con iconos, barra de tareas centrada, menú Inicio con buscador,
  ventanas que se mueven, minimizan y cierran, notificaciones y reloj.
- Teclado español (ñ, tildes con tecla muerta, AltGr) y ratón PS/2.
- **Tienda Chamows**: instala apps desde el disco del sistema a `C:\Apps`.
  Cada app es un programa ELF separado que el núcleo carga y enlaza al vuelo.
- Apps: Calculadora, Bloc de notas, Paint, Reloj (con cronómetro) y Serpiente.
- Terminal con `ayuda`, `apps`, `instalar <id>`, `desinstalar <id>`, `abrir <id>`,
  `mem`, `hora`, `ver`, `cls`, `reiniciar` y `apagar`.

Límites actuales: todo vive en memoria (al reiniciar se pierden las apps
instaladas), no hay red ni disco duro, y las apps corren con los mismos
permisos que el núcleo.

## Probarlo en tu Mac

```bash
brew install qemu
qemu-system-i386 -kernel chamows.bin -vga std -m 128
```

`chamows.bin` sale de `make` (abajo) o de la pestaña *Actions* de GitHub
(artefacto `chamows-bin`). Haz clic dentro de la ventana de QEMU para usar el
ratón; `Ctrl+Alt+G` lo suelta.

## Probarlo en el navegador

```bash
make -C os && make -C os web
os/tools/fetch-v86.sh            # una vez: descarga el emulador v86
python3 -m http.server -d os/web 8000   # abre http://localhost:8000
```

## Compilar

Necesitas Linux (o un contenedor) con `gcc`, `nasm`, `binutils`, `python3-pil`
y la fuente DejaVu. En Mac lo más fácil es usar el artefacto de GitHub Actions.

```bash
make -C os          # núcleo + apps -> os/build/chamows.bin
make -C os run      # arranca en QEMU con el registro en la terminal
make -C os test     # arranca sin pantalla y comprueba que llega al escritorio
```

## Cómo está hecho

| Pieza | Dónde |
|---|---|
| Arranque Multiboot, GDT, IDT, PIC, PIT | `os/kernel/boot.asm`, `isr.asm`, `cpu.c` |
| Memoria (montón first-fit) | `os/kernel/mem.c` |
| Teclado español y ratón | `os/kernel/ps2.c` |
| Gráficos (antialias, sombras, fuentes) | `os/kernel/gfx.c` |
| Escritorio, ventanas, menú Inicio | `os/kernel/gui.c` |
| Tienda, Terminal, Este equipo | `os/kernel/builtin.c` |
| Cargador ELF y API para apps | `os/kernel/apps.c`, `os/sdk/chamows.h` |
| Apps | `os/apps/<app>/main.c` + `manifest.ini` |
| Repositorio de apps (disco) | `os/tools/mkrepo.py` |

### Hacer una app nueva

1. Crea `os/apps/miapp/main.c` con `int app_main(const cw_api *api)` y un
   `manifest.ini` (copia el de `clock`).
2. Añade `miapp` a `APPS` en `os/Makefile` y su icono en `os/tools/genassets.py`.
3. `make -C os`: la app aparece en la Tienda.

Las decisiones de diseño están en [DECISIONS.md](DECISIONS.md).
