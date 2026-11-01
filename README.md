# Chamows

Un sistema operativo de 64 bits hecho desde cero, con aspecto de Windows 11.
También compila en 32 bits, que es la versión que corre en el navegador.
No usa Linux ni Windows por debajo: el núcleo, el escritorio, las ventanas,
la tienda y las apps están escritos aquí, en C y ensamblador.

## Qué hace hoy

- Dos versiones del mismo código: **64 bits** (QEMU, con USB) y **32 bits** (navegador con v86).
- **USB** (controlador xHCI): teclados, ratones, tabletas y pendrives.
- **Pendrive FAT32**: las apps instaladas se guardan en `D:\CHAMOWS\APPS64` y vuelven
  al reiniciar. Sin pendrive viven en memoria.
- Aspecto moderno ("Windows 12" a nuestra manera): barra de tareas flotante en
  tres piezas, paneles de vidrio (fondo difuminado), esquinas redondeadas,
  panel de **Widgets** (reloj, calendario, memoria) y **modo oscuro / claro**.
- **Programas .exe de Windows** (solo 64 bits): cargador PE propio y un mini
  Win32 (consola, ventanas, dibujo GDI, temporizadores, MessageBox). Sirve para
  programas pequeños compilados con MinGW; no abre programas grandes como
  Office, Steam o Chrome. Desde la Tienda o con `ejecutar JUEGOS\JUEGO.EXE`.
  También hilos, archivos del pendrive (C:\ y D:\, nombres largos) y lo que
  usa SDL2: el **ScummVM oficial de Windows (64 bits) abre su menú** y se
  maneja con ratón y teclado; The Secret of Monkey Island se juega. Sin sonido
  ni música, sin OpenGL y sin teclas F1-F12.
  Para probarlo: copia el contenido del zip de ScummVM al pendrive y ejecuta
  `ejecutar scummvm.exe` (QEMU necesita `-m 1024`). Un juego directo:
  `ejecutar scummvm.exe -p MONKEY --auto-detect`.
- **Red** (tarjeta Intel e1000): DHCP, DNS, ping y TCP. En la terminal: `red`,
  `ping <equipo>`, `nslookup <nombre>` y `http <equipo[:puerto]> [ruta]`.
  Los programas .exe tienen Winsock: sockets TCP y UDP, select, WSAAsyncSelect y getaddrinfo.
- **DLL**: los .exe cargan sus propias DLL desde el pendrive (junto al programa,
  la carpeta actual o `C:\CHAMOWS\SYSTEM`), con DllMain, TLS, LoadLibrary y GetProcAddress.
  Probado con el SDL2.dll oficial.
- **Hilos con turnos**: si un hilo de un .exe no suelta la CPU, el reloj se la quita cada 20 ms;
  el escritorio no se congela y los cerrojos de Windows (secciones críticas, SRW, variables de condición) funcionan.
- **Qt 6 (paso 5 hacia Telegram)**: las DLL oficiales de Qt 6.12 (MSVC) funcionan. `rcc.exe` compila
  archivos .qrc y **Qt Designer abre y se usa**: menús desplegables, diálogos, ratón y teclado.
  Las fuentes TrueType se ponen en `WINDOWS\FONTS` del pendrive (por ejemplo DejaVu Sans).
  Los programas Qt usan el aspecto **Windows 11** (botones redondeados, acento azul, menús translúcidos);
  Chamows pone su propia fuente de iconos (`CHAMOWSI.TTF`) en `WINDOWS\FONTS` la primera vez.
- **Telegram Desktop (en camino)**: el `Telegram.exe` oficial arranca y muestra su primera ventana
  con texto (Chamows dibuja las letras TrueType con las funciones GDI de Windows, usando stb_truetype).
  Ya llega a su ventana principal ("Start Messaging") y a la pantalla para entrar con un código QR.
  Todavía no conecta con los servidores de Telegram; es el siguiente paso.
- Ventanas como en Windows 11: botón **maximizar/restaurar** (y doble clic en el título) para los
  programas de Windows que cambian de tamaño, y cada programa con **su propio icono** en el título,
  la barra de tareas y los avisos.
- Teclado español (ñ, tildes con tecla muerta, AltGr) y ratón PS/2.
- **Tienda Chamows**: instala apps desde el disco del sistema a `C:\Apps`.
  Cada app es un programa ELF separado que el núcleo carga y enlaza al vuelo.
- **Explorador de archivos**: Apps (C:) y pendrive (D:), carpetas, abrir .exe y
  archivos de texto, crear carpetas y eliminar archivos.
- Apps: Calculadora, Bloc de notas, Paint, Reloj (con cronómetro), Serpiente y
  Burbujas. Programas .exe: Hola Windows (consola) y el juego Esquiva.
- Terminal con `ayuda`, `apps`, `instalar <id>`, `desinstalar <id>`, `abrir <id>`,
  `usb`, `dir [carpeta]`, `ejecutar <archivo.exe>`, `mem`, `hora`, `ver`, `cls`, `reiniciar` y `apagar`.

Límites actuales: no hay navegador ni disco duro interno, no se admiten hubs USB
(los dispositivos van directos al equipo), los nombres de archivo son 8.3 y
las apps corren con los mismos permisos que el núcleo.

## Probarlo en tu Mac (64 bits con USB)

1. `brew install qemu`
2. Descarga el artefacto `chamows` de la pestaña *Actions* de GitHub y descomprímelo.
3. En esa carpeta: `./run-chamows.sh`

El script crea un pendrive virtual de 64 MB (`pendrive.img`) la primera vez y
arranca Chamows con teclado, ratón y ese pendrive conectados por USB. Instala
apps, cierra QEMU, vuelve a lanzarlo: siguen ahí.

La versión de 32 bits sin USB: `qemu-system-i386 -kernel chamows-32bits.bin -vga std -m 128`.

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
make -C os                # 64 bits -> os/build/x86_64/chamows.bin
make -C os ARCH=i386      # 32 bits -> os/build/i386/chamows.bin
make -C os both           # las dos
make -C os run            # arranca en QEMU con USB y pendrive de prueba
make -C os test           # arranca sin pantalla y comprueba que llega al escritorio
```

## Cómo está hecho

| Pieza | Dónde |
|---|---|
| Arranque Multiboot (y paso a 64 bits), IDT, PIC, PIT | `os/kernel/arch/<x86_64\|i386>/`, `cpu.c` |
| USB: xHCI, teclado/ratón/tableta, pendrives | `os/kernel/usb/` |
| Sistema de archivos FAT32 | `os/kernel/fat.c` |
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
