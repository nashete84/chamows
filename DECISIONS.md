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

## 21. ScummVM original (scummvm.exe de Windows)
- **Elegido:** ejecutar el scummvm.exe oficial de 64 bits (con SDL2 dentro) ampliando el mini Win32, sin sonido. Lo pidió Nacho en la tarjeta de decisión.
- **Rechazado:** portar ScummVM como app nativa de Chamows (más rápido de lograr, pero no es "abrir el .exe").
- **Hilos:** hilos cooperativos (fibras) por proceso, cada uno con su pila y su TEB falso vía GS_BASE. CreateThread, eventos, mutex, semáforos y esperas cambian de hilo cuando nadie puede avanzar. Rechazado: hilos con desalojo real, porque el núcleo no tiene planificador preemptivo y sería un cambio muy grande.
- **free() seguro:** el montón marca cada bloque con un número mágico; un free() de un puntero ajeno se registra y se ignora en vez de colgar el sistema.
- **Estado:** fase 1 hecha. Las 686 funciones que importa se resuelven o se sustituyen, arranca su main y SDL llega a iniciarse; termina con "Could not initialize SDL: No displays available!" porque faltan las funciones de pantalla (EnumDisplayMonitors, GetMonitorInfoW, EnumDisplaySettingsW…). Fases siguientes: archivos (fase 2) y ventana/gráficos de SDL (fase 3).
- **Fase 2 (archivos):** C:\ y D:\ son los dos la raíz del pendrive. Cada archivo se lee entero a memoria al abrirlo y se guarda al cerrarlo; así el driver FAT sigue siendo sencillo. Rechazado: lectura por sectores con caché, que hace falta solo para archivos de cientos de MB. El FAT ahora lee nombres largos (los de Windows y macOS); los archivos nuevos siguen necesitando nombres 8.3. Las carpetas especiales (AppData, Documentos) van a C:\APPDATA. Renombrar es copiar y borrar.
- **Fase 3 (pantalla de SDL):** un monitor con un modo (la pantalla de Chamows, 32 bits), ventanas Unicode, propiedades de ventana, y "DIB sections" para que SDL dibuje con su renderer por software. Rechazado: OpenGL (sería escribir un driver 3D). Como ScummVM elige OpenGL por defecto, Chamows le añade `--gfx-mode=surfacesdl` si no se pasa otro modo; es un ajuste solo para SCUMMVM.EXE.
- **Teclado:** Chamows solo avisa de pulsaciones, así que cada tecla se envía como bajar + carácter + subir, con su código de tecla de PC (SDL lo necesita). Las teclas F1-F12 aún no llegan a los programas.
- **Nombres largos al escribir:** el FAT crea alias 8.3 (ARCHIV~1.TXT) con entradas de nombre largo y suma de control; fsck.fat lo da por bueno. C:\Windows y AppData apuntan a C:\CHAMOWS para no llenar la raíz del pendrive.
- **Funciones que faltan:** cada función de Windows que no existe se anota en el registro la primera vez que el programa la llama, con su nombre; así se ve qué falta sin depurador.
- **Fase 4 (Monkey Island):** probado con The Secret of Monkey Island (disquetes, 1990): ScummVM lo detecta, la intro y la primera escena se ven y se juega con el ratón. Dos arreglos: `fgetpos`/`fsetpos` (MinGW los usa para moverse dentro de un archivo) y música apagada (`--music-driver=null`), porque sin tarjeta de sonido la música no avanzaba y el juego esperaba a que acabase. Los datos del juego no se guardan en el repositorio.

## 22. Red (paso 1 hacia Telegram Desktop)
- **Objetivo final de Nacho:** instalar y usar Telegram Desktop. Camino: red → Winsock → cargar DLL → hilos que corren a la vez → más Win32 para Qt → probar el zip portable.
- **Tarjeta:** Intel e1000 (la que QEMU, VirtualBox y UTM ponen por defecto). Rechazado: virtio-net (más rápida, pero no está en todos los emuladores) y Realtek RTL8139 (más antigua).
- **Sin interrupciones:** la tarjeta se consulta en cada vuelta del escritorio, igual que el USB. Más sencillo; la latencia es la de una vuelta (unos ms).
- **Pila propia y pequeña:** Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS y TCP de cliente. Rechazado: portar lwIP (más completa, pero es mucho código ajeno que entender y adaptar). Sin fragmentos IP, sin IPv6, sin servidor TCP todavía.
- **TCP:** reenvío "vuelta atrás" (go-back-N) con espera que se dobla hasta 8 s, ventana de 64 KB, los datos fuera de orden se descartan y se pide de nuevo. Suficiente para HTTP y para Telegram; no es rápido en redes con pérdidas.
- **DNS:** el que da DHCP; `red dns <ip>[:puerto]` lo cambia a mano.
- **Probado en QEMU:** DHCP da 10.0.2.15, `ping 10.0.2.2` responde, `http` descarga una página y un archivo de 1,3 MB completo de un servidor del ordenador anfitrión, `nslookup` resuelve con un servidor DNS de prueba. El DNS público no se pudo probar porque la red de pruebas lo bloquea.

## 23. Winsock (paso 2 hacia Telegram Desktop)
- **Qué hay:** socket, connect, send/recv, sendto/recvfrom, WSASend/WSARecv, shutdown, ioctlsocket (FIONBIO, FIONREAD), get/setsockopt, select, WSAAsyncSelect, gethostbyname, getaddrinfo/GetAddrInfoW y las conversiones de direcciones. Todo encima de la pila del paso 1.
- **Bloqueo cooperativo:** una llamada que espera (connect, recv, select) cede el turno con yield en bucle hasta que hay datos o se acaba el tiempo; el escritorio sigue vivo. Rechazado: hilos reales aquí (llegan en el paso 4).
- **WSAAsyncSelect:** los avisos (FD_CONNECT, FD_READ, FD_WRITE, FD_CLOSE) se envían como mensajes de ventana desde win32_poll. Qt y Telegram usan este modelo o select.
- **Fin de conexión:** se usa tcp_eof (el otro lado cerró) en vez del estado TCP, porque CLOSE_WAIT todavía parecía "vivo" y FD_CLOSE nunca llegaba.
- **No hay todavía:** listen/accept (servidor), IPv6, E/S solapada (overlapped/IOCP) y WSAIoctl. La búsqueda DNS detiene el escritorio un momento.
- **Probado en QEMU:** un .exe de prueba hizo connect con y sin bloqueo, select, gethostbyname y UDP; otro con WSAAsyncSelect recibió FD_CONNECT, FD_READ (289 bytes) y FD_CLOSE contra un servidor HTTP del anfitrión.

## 24. Cargar DLL (paso 3 hacia Telegram Desktop)
- **Dos tipos de DLL:** las de Windows (kernel32, user32, ws2_32, api-ms-win-*...) no son archivos: sus funciones son la capa Win32 de Chamows. Las demás (Qt, SDL...) se buscan en disco: junto al .exe, en la carpeta de SetDllDirectory, en la carpeta actual y en `C:\CHAMOWS\SYSTEM`. Rechazado: cargar las DLL de un Windows real (necesitan el núcleo de Windows por debajo).
- **Cómo se carga:** igual que el .exe (secciones, reubicación), luego sus imports cargan otras DLL en cadena. Exports por nombre (búsqueda binaria, con la pista del import), por número y reenviados (`DLL.Función`). Si una DLL que no es de Windows no está, sus funciones se buscan en la capa Win32 como antes, para no romper programas que ya funcionaban.
- **Orden de arranque:** DllMain(PROCESS_ATTACH) de las dependencias primero, luego el .exe. También THREAD_ATTACH/DETACH en cada hilo y PROCESS_DETACH al salir. FreeLibrary descarga la DLL cuando nadie la usa.
- **TLS por módulo:** cada .exe o DLL con sección .tls tiene su hueco en la tabla TLS del hilo (hasta 16). Las DLL cargadas después también lo reciben en los hilos que ya existen.
- **Recursos:** FindResource, LoadResource, SizeofResource y LoadString leen los recursos de cualquier módulo.
- **atexit por programa:** antes era global y un programa podía llamar código de otro que ya había terminado.
- **Aviso de herramientas:** GNU ld enlaza mal la TLS nativa de clang dentro de una DLL (marca como reubicable un desplazamiento que no lo es). No es un fallo de Chamows; con lld funciona.
- **No hay todavía:** excepciones de C++/SEH que atraviesan DLL (RtlLookupFunctionEntry y desenrollado), retraso de imports (delay-load) y DLL de 32 bits.
- **Probado en QEMU:** un .exe con tres DLL propias (cadena de imports, reenvío, ordinal, TLS en hilos, recurso de texto, LoadLibrary/GetProcAddress/FreeLibrary, DLL inexistente da error 126). Un programa con el SDL2.dll oficial 2.30.9 abre su ventana y dibuja 175 fotogramas en 6 s. ScummVM sigue abriendo su lanzador.

## 25. Hilos con turnos (paso 4 hacia Telegram Desktop)
- **Qué cambia:** antes un hilo de un .exe solo soltaba la CPU al esperar (mensajes, Sleep, WaitFor...). Un bucle sin esperas congelaba el escritorio y un cerrojo de espera activa entre dos hilos se quedaba colgado. Ahora el reloj (cada 10 ms) quita la CPU a un hilo que lleva 20 ms seguidos.
- **Solo se pausa el código del programa:** si el hilo está dentro de código de Chamows (la capa Win32, el núcleo), no se toca; se pausa en el siguiente tic en que vuelva a su propio código. Así el núcleo no necesita cerrojos por todas partes y cada función Win32 se ejecuta de una pieza. Rechazado: varios núcleos de CPU a la vez (SMP), que exigiría proteger todo el núcleo; queda para más adelante si hace falta velocidad.
- **Cómo se pausa:** dentro de la interrupción del reloj, después del EOI, se guardan SSE/x87 (fxsave) en la pila del hilo y se cede el turno; al volver se restauran y la interrupción termina normalmente. Los registros generales ya los guarda la entrada de la interrupción.
- **Por hilo:** GetLastError y errno se guardan con cada hilo (antes eran globales y un hilo podía ver el error de otro).
- **Cerrojos de verdad:** secciones críticas (con recursión), SRW de lectura/escritura, variables de condición (CS y SRW), WaitOnAddress, InitOnce, FLS y SwitchToThread. Antes casi todos no hacían nada.
- **Probado en QEMU:** dos hilos en bucle sin llamadas a Windows avanzan los dos (47 y 49 millones de vueltas en 2 s) y Sleep(500) del hilo principal tarda 540 ms; las sumas con SSE salen exactas; un cerrojo de espera activa se libera; 4 hilos sumando con sección crítica dan 400000 exacto; productor/consumidor con SRW y variable de condición pasa 1000 elementos. Un programa con un bucle infinito se cierra con la X. SDL2.dll, las DLL de prueba y Monkey Island siguen funcionando.

## 26. Más Win32 para Qt 6 (paso 5 hacia Telegram Desktop)
- **Objetivo de prueba:** las DLL oficiales de Qt 6.12 para Windows (MSVC, 64 bits): `rcc.exe` y Qt Designer. Linguist no sirve de prueba: contiene un widget 3D que necesita Direct3D y su ventana queda en blanco.
- **Runtime de C de Microsoft (UCRT):** familia `__stdio_common_*` (printf y scanf), strtod, matemáticas, tablas atexit por módulo, `_Gettnames`, `rand_s`, `_aligned_malloc`. En archivos nuevos (`wcrt.inc`, `wmisc.inc`, `wuser.inc`, `wfont.inc`) para no hacer win32.c gigante.
- **Entorno de verdad:** una tabla de variables compartida por todo el programa, con valores por defecto de Windows (USERPROFILE, APPDATA, windir=C:\WINDOWS...). `QT_QPA_PLATFORM=windows:fontengine=freetype` va por defecto: Qt dibuja el texto con FreeType (incluido en Qt) en vez de con GDI/DirectWrite, que Chamows no tiene.
- **Fuentes:** Qt busca las fuentes con EnumFontFamiliesExW y su archivo en el registro. Chamows lee las tablas name/OS2/post de cada .ttf/.otf de `WINDOWS\FONTS` y contesta las dos cosas. El registro es solo de lectura y solo tiene esa clave. Rechazado: un registro completo en disco (no hace falta todavía).
- **Locale:** inglés de EE. UU.; la página de códigos ANSI (GetACP) es UTF-8 (65001), así los textos de 8 bits no se estropean.
- **Gráficos 3D:** DXGI, D3D11/12, DirectWrite, Direct2D y DirectComposition devuelven error (E_NOTIMPL) en vez de "éxito" sin nada detrás. Antes Qt creía tenerlos y fallaba más tarde.
- **Ventanas ocultas:** una ventana sin WS_VISIBLE no se ve ni sale en la barra de tareas hasta ShowWindow (Qt crea muchas ventanas invisibles).
- **WM_TIMER bajo demanda:** como en Windows, el mensaje de temporizador se crea al pedir mensajes y no se acumula en la cola.
- **Fallo del bucle de Qt (el importante):** Qt pregunta con MsgWaitForMultipleObjectsEx(espera 0) si llegó algo *nuevo*. Chamows decía que sí por un temporizador de 10 ms que vencía mientras PeekMessage cedía el turno, y Qt se quedaba dando vueltas sin entregar nunca sus eventos internos (el redibujado). Ahora, con espera 0 y sin MWMO_INPUTAVAILABLE, los temporizadores no cuentan como nuevos.
- **Archivos abiertos dos veces:** cada handle guarda el archivo en memoria y lo escribe al cerrar. Si otro handle abre el mismo archivo, primero se guarda lo pendiente. Designer escribe la plantilla en un archivo temporal y la lee con otro handle sin cerrar el primero.
- **Ventanas emergentes:** WS_POPUP sin barra de título (menús, listas desplegables, tooltips) se dibuja sin marco, en la posición exacta que pide el programa, siempre encima y sin botón en la barra de tareas. Las ventanas normales siguen sin moverse con SetWindowPos (Chamows las coloca).
- **Trazador de llamadas:** `W32_TRACE 1` en win32.c registra cada llamada a Windows de un .exe/DLL ("Nombre(a,b) desde dirección"). Apagado por defecto. Sirvió para encontrar el fallo del bucle.
- **No hay todavía:** nombres largos nuevos en FAT (Designer no puede crear `.designer`; avisa y sigue), dibujado con GDI de texto, Direct3D, portapapeles y arrastrar y soltar.
- **Probado en QEMU:** `rcc.exe --help` y `rcc.exe test.qrc -o out.cpp` (el contenido descomprimido es igual al original). Designer: abre con texto DejaVu, el diálogo New Form responde a clics, crea un formulario, el menú File se abre en su sitio con resaltado al pasar el ratón, File > New abre el diálogo y escribir "push" en el filtro deja solo Push Button. `make test` (64 y 32 bits), EX.EXE, SD.EXE y ScummVM siguen igual.

## 27. Aspecto Windows 11 en Qt y primeros pasos de Telegram (paso 6)
- **Estilo:** `QT_STYLE_OVERRIDE=windows11` por defecto. Rechazado: decir que somos Windows 11 (build 22631); Qt elige ese estilo solo, pero también usa APIs WinRT de pantalla que Chamows no tiene y el programa se cierra. Chamows sigue diciendo Windows 10 22H2.
- **Iconos del estilo:** Qt dibuja flechas, marcas y botones con caracteres de la fuente "Segoe Fluent Icons", que no es libre. Chamows trae su propia fuente (`tools/make-iconfont.py` la genera con trazos simples; `iconfont.h` la lleva dentro del núcleo) con los mismos códigos y ese nombre de familia, y la escribe en `WINDOWS\FONTS\CHAMOWSI.TTF` si falta.
- **Color de acento:** Qt lo pide a WinRT (`UISettings.GetColorValue`). Hay un WinRT mínimo: HSTRING con el formato de Windows (C++/WinRT construye las cadenas él mismo), `RoGetActivationFactory` solo para `Windows.UI.ViewManagement.UISettings`, con el azul de Windows 11 (#0078D4) y tema claro. El resto de clases dan REGDB_E_CLASSNOTREG.
- **Menús translúcidos:** UpdateLayeredWindow(Indirect) copia los píxeles con alfa a la ventana emergente y el compositor los mezcla (esquinas redondeadas y sombra propias de Qt). Las emergentes avisan WM_MOVE al moverse, porque Qt toma de ahí su posición.
- **Telegram 7.3.3 (exe estático de 250 MB):**
  - Las importaciones de carga diferida de DLL de Windows se enlazan al cargar, con stubs para lo que falta. Si no, el ayudante del programa pedía GetProcAddress y una función ausente lo mataba (C06D007F).
  - FAT crea nombres largos de hasta 255 letras (antes 63).
  - Tuberías con nombre: se puede escuchar (QLocalServer) aunque nadie conecte; Telegram las usa para saber si ya hay otro abierto.
  - Esperas del grupo de hilos: WaitFor...Callbacks ya no espera a una espera que nunca se cumple.
  - Al terminar un programa se guardan sus archivos abiertos, como en Windows (así queda el registro de Telegram).
  - La fecha (GetSystemTimeAsFileTime) sale del reloj real; antes era fija en 2019.
  - Números aleatorios para OpenSSL: CryptGenRandom, BCryptGenRandom.
  - Unas 35 funciones de kernel32 más (afinidad, energía, rutas, sesiones...).
- **Dónde está Telegram:** arranca, decide que es la única copia, crea `tdata` y su registro, y se cae al crear las fuentes. Su Qt no trae FreeType en Windows; usa DirectWrite o GDI. Falta un dibujante de fuentes TrueType en Chamows (CreateFontIndirect, GetGlyphOutline, GetFontData, ExtTextOut).
- **Probado en QEMU:** Designer con estilo Windows 11 (botones redondeados, casillas azules con marca, flechas, menú File translúcido en su sitio con resaltado). Telegram llega a "Cannot initialize common font database data" y se cierra (c0000005). `make test` 64 y 32 bits, EX, SD, rcc y ScummVM siguen igual.

## 28. Letras TrueType con GDI (Telegram) y más Windows 11
- **Dibujante de letras:** stb_truetype (dominio público, un solo archivo) dentro del núcleo, en `win32/wtext.inc`. Rechazado: FreeType (mucho más grande y con dependencias) y escribir uno propio (más tiempo, mismo resultado). Las pocas funciones de C que necesita (floor, sqrt...) van al lado; pow/cos/acos solo las usa la parte SDF, que no se llama.
- **Funciones GDI de texto:** CreateFontIndirectW/CreateFontW (elige la fuente de la misma familia con el peso y la inclinación más parecidos; si no está, Segoe UI, DejaVu Sans, Arial...), GetTextMetricsW, GetOutlineTextMetricsW, GetTextFaceW, GetFontData (tablas del archivo), GetGlyphIndicesW, anchos ABC, GetGlyphOutlineW (medidas, mapas de grises GGO_GRAY*, mapa de 1 bit y contornos GGO_NATIVE) y ExtTextOutW/TextOutW/DrawTextW con alineación y transformación del DC (SetWorldTransform). AddFontMemResourceEx y AddFontResourceExW añaden fuentes del programa (Telegram trae las suyas). Las de memoria no salen al enumerar, como en Windows.
- **Contornos en 1/64 de píxel con coordenadas int:** las curvas se transforman (escala, giro) antes de rasterizar, así sirve cualquier matriz de Qt.
- **GetDC(NULL)** devuelve un DC de pantalla para medir texto (antes NULL, y Qt no podía añadir fuentes).
- **Fallos que salieron al seguir con Telegram:**
  - GetCurrentThreadId devolvía 4 en todos los hilos (una tabla vieja tapaba la buena). Qt creía que todos los hilos eran el mismo.
  - Las fechas de los archivos eran "ahora"; ahora salen de la entrada FAT (GetFileAttributesEx, FindFirstFile, GetFileInformationByHandle, GetFileTime nueva). QLockFile veía su bloqueo "del futuro" y no lo limpiaba.
  - SetFileInformationByHandle (borrar y renombrar con el archivo abierto) existe: Qt borra así. Sin ella, los `.rmlock` se acumulaban en cada arranque.
  - Nombres cortos 8.3: de `NOMBRE~9` se pasa a `NOMB~10`... como Windows (antes no se podía crear el décimo).
  - BCryptEnumContextFunctions dice "no hay" en vez de "éxito" con la lista vacía (Telegram se caía al leerla).
  - DeleteObject ignora lo que no es un objeto GDI de Chamows (Qt le pasa un DC al maximizar).
- **Ayuda para depurar:** si un programa se cae, el registro muestra quién llamó a quién (con las tablas de desenrollado del .exe). Si la caída es dentro de Chamows, muestra las direcciones de retorno de la pila.
- **Dónde está Telegram:** se ve el aviso "Last time Telegram Desktop was not closed properly" con su texto bien dibujado y su icono. Al pulsar Skip o al maximizar se cae dentro de su propio código ("QWidget::repaint: Recursive repaint detected" y luego c0000005). Queda para el siguiente paso.
- **Maximizar (Windows 11):** tres botones (minimizar, maximizar/restaurar, cerrar). Maximizar llena la pantalla sobre la barra de tareas, quita las esquinas redondeadas y avisa al programa con WM_MOVE y WM_SIZE (SIZE_MAXIMIZED). Doble clic en el título hace lo mismo. Solo para programas de Windows con borde de tamaño (WS_THICKFRAME o WS_MAXIMIZEBOX); en los demás el botón sale gris. ShowWindow(SW_MAXIMIZE/SW_RESTORE) e IsZoomed funcionan. Rechazado: cambiar el tamaño arrastrando el borde (más trabajo; se puede añadir luego).
- **Iconos de los programas:** el icono del .exe (recursos RT_GROUP_ICON/RT_ICON en formato DIB; las imágenes PNG se saltan) se usa en el título, la barra de tareas y los avisos. LoadIconW/LoadImageW leen iconos de los recursos, CreateIconIndirect y CreateBitmap hacen iconos desde mapas de bits (así los hace Qt) y WM_SETICON cambia el de la ventana.
- **Probado en QEMU:** Telegram muestra su aviso con texto e icono. Qt Designer se abre maximizado, se restaura con el botón y se maximiza con doble clic en el título, recolocando sus paneles. ScummVM sale con su icono. `make test` 64 y 32 bits, EX.EXE (código 3), SD.EXE (código 0), rcc (crea out.cpp) y ScummVM siguen igual.

## 29. Telegram llega a su ventana principal
- **Una cola de mensajes por hilo, como Windows:** cada ventana es del hilo que la creó y sus mensajes (PostMessage, entradas, WM_PAINT, WM_TIMER) solo los ve ese hilo. WM_QUIT también es por hilo. Antes había una sola cola por programa, y un hilo de trabajo de Qt se llevaba los temporizadores y el pintado de la ventana principal ("Timers cannot be stopped from another thread", "Recursive repaint detected"). SendMessage a una ventana de otro hilo espera a que ese hilo la atienda dentro de su GetMessage/PeekMessage. Rechazado: seguir con una sola cola y "adivinar" el hilo (no hay forma fiable).
- **Hilos suspendidos:** CreateThread/_beginthreadex respetan CREATE_SUSPENDED, y ResumeThread/SuspendThread funcionan (antes el hilo arrancaba antes de que Qt guardara sus datos).
- **El .exe también oye los hilos:** sus funciones TLS se llaman al empezar y terminar cada hilo (variables `thread_local`), no solo las de las DLL.
- **GetLogicalProcessorInformationEx:** un procesador, un núcleo, un nodo NUMA. El planificador de Microsoft (ConcRT) la pide primero sin búfer y se para si la respuesta no es "búfer pequeño".
- **WinRT DataWriter:** existe y todos sus métodos dicen "no implementado". Telegram lo crea para el panel de teclas multimedia y se cerraba si no estaba.
- **Punteros nulos:** mientras corren programas .exe, los primeros 64 KB de memoria no están en la tabla de páginas (los primeros 2 MB pasan a páginas de 4 KB), igual que en Windows. Antes un puntero nulo leía restos de la BIOS y el programa seguía con basura hasta caerse lejos del fallo. Esto destapó dos fallos de Chamows: el PEB no tenía parámetros del proceso ni lista de módulos (ahora sí: línea de órdenes, nombre del .exe, versión de Windows, lista vacía) y GetTokenInformation devolvía la misma forma para todo (ahora User, Groups, Owner, PrimaryGroup, Elevation e IntegrityLevel).
- **longjmp en funciones partidas:** el compilador parte funciones en trozos con información de desenrollado "encadenada". Un `jmp` de un trozo a otro de la misma función se tomaba como final de función (llamada de cola) y el desenrollado se iba al sitio equivocado. Ahora solo es final si salta a otra función. Era el fallo del dibujante de Qt (qgrayraster usa setjmp/longjmp).
- **Memoria:** el montón llega hasta 3 GB (antes 512 MB; Telegram solo ocupa unos 255 MB al cargarse) y un hilo que termina devuelve su pila (Telegram crea cientos). La versión de 32 bits sigue con 512 MB.
- **Registro de caídas:** una excepción sin manejar sale en el registro antes del filtro del programa (que puede cerrar sin decir nada), con el tipo C++ si lo es y quién llamó, también a través de bloques catch.
- **Dónde está Telegram:** pasa el aviso, muestra "Start Messaging" y la pantalla del código QR. Pone "Reconnect in N s": todavía no conecta con sus servidores. Detalles que faltan: dibuja su propia barra de título dentro de la de Chamows, y el borde de sombra sale negro.
- **Probado en QEMU:** lo anterior con Telegram. `make test` 64 y 32 bits, EX.EXE (código 3), SD.EXE (código 0), Qt Designer (5 arranques seguidos, igual que antes) y ScummVM siguen bien. Una vez, Designer se cerró por una llamada a la dirección 0 justo después de pedir una clase WinRT; no se repitió en 5 intentos más.
