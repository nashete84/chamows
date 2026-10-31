# Decisiones de Chamows

## 1. Núcleo propio en vez de Linux por debajo
- **Elegido:** un núcleo propio de 32 bits escrito desde cero (C + NASM).
- **Descartado:** la primera versión (rama `chamows-prototipo`, PR #1) era Ubuntu + KDE
  personalizado. Funcionaba, pero Nacho pidió "hazlo de cero sin usar Ubuntu por detrás"
  y eligió "núcleo propio de verdad" frente a un escritorio web.
- **Por qué:** es lo que se pidió. El coste es que muchas cosas (red, disco, drivers)
  hay que escribirlas a mano y llegarán más tarde.

## 2. 64 bits, y 32 bits para la web, desde el mismo código
- **Elegido (v0.4):** el código compila dos versiones. La de 64 bits (x86-64, modo largo)
  es la principal y tiene USB; la de 32 bits (i386) existe porque el emulador del
  navegador (v86) solo hace 32 bits. Lo específico de cada CPU vive en
  `kernel/arch/<arch>/` (arranque, interrupciones, registros); el resto es común.
- **Descartado:** solo 64 bits con la web congelada (Nacho prefirió mantener la web al día)
  y quedarse en 32 bits (Nacho pidió 64).
- **Cómo arranca en 64 bits:** Multiboot 1 nos deja en 32 bits; `boot.asm` mapea los
  primeros 4 GB 1:1 con páginas de 2 MB, activa el modo largo y salta a C. Así QEMU
  `-kernel` sigue sirviendo sin GRUB.
- **Detalle:** en 64 bits todo se compila sin "red zone", porque las interrupciones usan
  la misma pila que el núcleo y las apps.
- **Coste:** la API de apps cambió a v2 (`format` pasa a ser variádica, porque en 64 bits
  un puntero no cabe en un `int`). Las apps se compilan dos veces.

## 3. Gráficos por el adaptador Bochs (BGA)
- **Elegido:** pedir 1024×768×32 al adaptador BGA (puertos 0x1CE/0x1CF) y dibujar en su
  framebuffer lineal; 800×600 si no acepta.
- **Descartado:** modo texto VGA (no parece Windows) y llamadas VBE de la BIOS (exige
  volver a modo real).
- **Por qué:** QEMU `-vga std`, v86 y Bochs lo traen, y es una sola escritura de puertos.

## 4. Compositor por software con "doble búfer" y zonas sucias
- **Elegido:** cada ventana tiene su propio búfer; el escritorio se recompone solo en el
  rectángulo que cambió y se copia a pantalla. El cursor se pinta aparte.
- **Descartado:** redibujar toda la pantalla en cada cambio (lento en v86).

## 5. Apps como programas ELF separados
- **Elegido:** cada app es un ELF de 32 bits independiente de posición (PIE). El núcleo lo
  copia, aplica las reubicaciones `R_386_RELATIVE` y llama a `app_main(api)`. Las apps
  solo ven la tabla `cw_api` (ventanas, dibujo, memoria, reloj).
- **Descartado:** compilar las apps dentro del núcleo (entonces "instalar" sería un truco)
  y un intérprete de scripts (más lento y menos parecido a un .exe).
- **Por qué:** "instalar" de verdad copia un programa y lo ejecuta, como en Windows.

## 6. La Tienda instala desde el "disco del sistema"
- **Elegido:** las apps vienen empaquetadas en un repositorio (`CHREPO01`) dentro de la
  imagen del sistema; instalar copia el ELF a memoria (`C:\Apps`). Varias instalaciones
  pueden ir a la vez.
- **Descartado (por ahora):** descargarlas de internet (no hay driver de red).
- **Desde v0.4:** si hay un pendrive FAT32, la app también se copia a
  `CHAMOWS/APPS64/<ID>.ELF` (o `APPS32`) y al arrancar se vuelve a instalar desde ahí.
  Cada versión usa su carpeta porque sus programas son distintos.

## 7. Sin protección entre apps y núcleo (anillo 0)
- **Elegido:** las apps corren en el mismo espacio y nivel que el núcleo.
- **Descartado:** anillo 3 con paginación y llamadas al sistema.
- **Por qué:** mucho más trabajo; para un prototipo con apps propias es aceptable. Una app
  con un fallo puede tumbar el sistema (sale la pantalla azul de Chamows).

## 8. Números sin coma flotante
- **Elegido:** la Calculadora usa coma fija con 6 decimales y división de 64 bits propia
  (`sdk/cwlib.c`).
- **Descartado:** activar la FPU/SSE (hay que guardar su estado en cada interrupción).

## 9. Fuentes generadas con Pillow
- **Elegido:** DejaVu Sans / Sans Mono rasterizadas a mapas de 8 bits (Latin-1) al compilar.
- **Por qué:** texto suavizado sin motor de fuentes en el núcleo. Solo Latin-1: caracteres
  como "√" o "⌫" no existen, por eso la Calculadora dice "Raíz" y "Borrar".

## 10. Ratón en el navegador
- **Elegido:** la página fija el cursor de Chamows en (0,0) y luego envía la diferencia hasta
  donde está tu puntero, así ambos coinciden sin "capturar" el ratón.
- **Descartado:** bloqueo del puntero (pide permiso y obliga a pulsar Esc para salir).
- **Arreglado en el camino:** el driver del ratón perdía todos los paquetes en v86 porque
  un ACK tardío (0xFA) desalineaba los 3 bytes de cada paquete; ahora se resincroniza.

## 11. AWS
- La versión con núcleo propio no se puede usar en una VM normal de AWS: EC2 no da
  pantalla gráfica. Para "en la nube" la vía es la página web con v86.

## 12. USB con xHCI, sin interrupciones
- **Elegido:** solo el controlador xHCI (USB 3, también maneja USB 1.1 y 2.0), consultado
  desde el bucle del escritorio cada 10 ms en vez de usar interrupciones.
- **Descartado:** UHCI/OHCI/EHCI (controladores antiguos; QEMU, UTM y los PC actuales
  usan xHCI) e interrupciones MSI (más código para la misma respuesta en un escritorio).
- **Límites:** sin hubs USB, sin conectar en caliente (los dispositivos se detectan al
  arrancar), y el controlador tiene que estar por debajo de 4 GB.

## 13. Teclado, ratón y tableta USB
- **Teclado:** informe "boot" de 8 bytes. Cada tecla se traduce al código PS/2 y entra por
  el mismo traductor de teclado español, así ñ, tildes y AltGr funcionan igual. La
  repetición de teclas (500 ms y luego 30 por segundo) la hace Chamows, como Windows.
- **Ratón y tableta:** se lee el descriptor de informes HID para encontrar X, Y y botones.
  Una tableta (QEMU, UTM) da posiciones absolutas: el cursor va exactamente donde está
  el tuyo, sin capturar el ratón.

## 14. Pendrives: SCSI sobre "bulk-only" y FAT32
- **Elegido:** el protocolo que usan casi todos los pendrives (clase 8, SCSI, BOT) y FAT32,
  con o sin tabla de particiones. Lectura y escritura de 4 KB en 4 KB.
- **Descartado:** exFAT y NTFS (más complejos; exFAT tiene patentes hasta hace poco),
  nombres largos (solo se leen como nombre corto 8.3).
- **Comprobado:** `fsck.fat` no encuentra errores tras instalar y desinstalar apps.

## 15. Programas .exe: cargador PE y mini Win32 propios

**Elegido:** un cargador de archivos PE de 64 bits (`os/kernel/win32/win32.c`)
que copia el programa a memoria, lo reubica con su tabla `.reloc`, enlaza cada
función importada por nombre a una versión propia (kernel32, user32, gdi32,
msvcrt: unas 200) y lo arranca. Cada programa tiene su propia pila y un TEB
falso en `gs` (el código de arranque de MinGW lo lee). Los programas ceden la
CPU al esperar un mensaje, dormir o mostrar un MessageBox (multitarea
cooperativa). Las funciones que no existen devuelven 0 y se anotan en el log.

**Rechazado:**
- *Portar Wine o ReactOS*: millones de líneas y dependen de Linux o de su
  propio núcleo; no cabe en un sistema hecho desde cero.
- *Solo empaquetar las apps de Chamows como .exe*: no ejecuta nada de Windows.
- *Multitarea con desalojo (hilos de verdad)*: necesita guardar el estado SSE en
  cada interrupción y separar memoria; se deja para cuando haya modo usuario.

**Consecuencias:** funcionan programas pequeños de consola y de ventanas
(GDI, temporizadores, doble búfer con BitBlt). No funcionan programas grandes,
.exe de 32 bits, DirectX, hilos, archivos ni red. Un programa que entra en un
bucle sin pedir mensajes congela el escritorio. Los .exe solo van en la versión
de 64 bits; la de 32 bits avisa. Se activa SSE al arrancar porque el código de
Windows lo usa, y `win32.c` se compila con SSE.

## 16. Juegos

**Elegido:** Burbujas (app nativa) y Esquiva (un .exe de Windows de verdad,
compilado con MinGW en el CI), ambos originales. Esquiva sirve también de
prueba del mini Win32.

**Rechazado:** copiar juegos conocidos.

## 17. Aspecto "Windows 12"

**Elegido:** barra de tareas flotante en tres piezas (Widgets, apps, reloj),
paneles de "vidrio" hechos con una copia difuminada del fondo de pantalla más
un tinte, esquinas de 10-16 px, panel de Widgets y modo oscuro por defecto con
interruptor. Diseño y logo propios, sin marcas de Microsoft.

**Rechazado:** difuminar en vivo lo que hay detrás (las ventanas): habría que
difuminar en cada fotograma, demasiado lento sin tarjeta gráfica.

**Consecuencias:** el vidrio solo deja ver el fondo, no las ventanas que tenga
detrás (igual que el efecto "Mica"). Las apps siguen con su contenido claro.

## 18. Navegador (pendiente)

Se hará en la siguiente fase, en el mismo PR: tarjeta de red e1000, TCP/IP,
DNS, https con una biblioteca TLS pequeña y un motor de HTML+CSS sin JavaScript.

## 19. Explorador de archivos

**Elegido:** app integrada en el núcleo (`os/kernel/explorer.c`) con tres
lugares: Este equipo, Apps (C:) y Pendrive (D:). Abre carpetas, ejecuta .exe,
muestra archivos de texto, crea carpetas (NUEVA, NUEVA2...) y elimina archivos.

**Rechazado por ahora:** renombrar, copiar/pegar y borrar carpetas con
contenido; necesitan cuadros de texto y operaciones de FAT32 que aún no hay.

## 20. Telegram

Telegram Desktop para Windows es un programa enorme (Qt, cientos de funciones
de Windows, hilos, red cifrada, OpenGL). El mini Win32 de Chamows no puede
ejecutarlo, y tampoco Telegram Web, que necesita un navegador con JavaScript
completo. Lo que sí se puede hacer cuando haya red: descargar e instalar .exe
pequeños desde internet, y una app propia de mensajes.
