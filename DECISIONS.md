# Decisiones de Chamows

## 1. Núcleo propio en vez de Linux por debajo
- **Elegido:** un núcleo propio de 32 bits escrito desde cero (C + NASM).
- **Descartado:** la primera versión (rama `chamows-prototipo`, PR #1) era Ubuntu + KDE
  personalizado. Funcionaba, pero Nacho pidió "hazlo de cero sin usar Ubuntu por detrás"
  y eligió "núcleo propio de verdad" frente a un escritorio web.
- **Por qué:** es lo que se pidió. El coste es que muchas cosas (red, disco, drivers)
  hay que escribirlas a mano y llegarán más tarde.

## 2. 32 bits x86 y Multiboot
- **Elegido:** i386 en modo protegido, arrancado con Multiboot 1 (`-kernel` de QEMU).
- **Descartado:** 64 bits (necesita paginación y un cargador más complejo; v86 no lo emula)
  y ARM (el Mac M1 lo correría nativo, pero no se puede probar en el navegador).
- **Por qué:** así se prueba igual en el navegador (v86) y en el Mac (QEMU).

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
- **Descartado (por ahora):** descargarlas de internet (no hay driver de red) y guardarlas
  en disco (no hay driver ATA ni sistema de archivos).
- **Consecuencia:** al reiniciar, las apps instaladas se pierden. Es lo siguiente a resolver.

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
