# Chamows: plan y registro de decisiones

## Plan (memo)

**Situación.** El repositorio `nashete84/chamows` estaba vacío. Nacho quiere un
sistema operativo con aspecto de Windows, llamado Chamows, que corra en una
máquina virtual de AWS y que, como primera función, permita instalar
aplicaciones.

**Complicación.** Escribir un sistema operativo desde cero (núcleo, drivers,
gráficos) son años de trabajo y no aporta nada a la meta. Además, una máquina
EC2 no tiene pantalla: hay que llegar al escritorio a distancia.

**Pregunta.** ¿Cómo conseguimos, en una iteración de prototipo, un escritorio
que se vea como Windows 11, accesible por navegador y por RDP, con una tienda
que instale apps de Linux y programas `.exe`?

**Respuesta.** Una distribución basada en Ubuntu 24.04 LTS, empaquetada como
imagen de AWS (AMI) con Packer:

1. **Escritorio** (esfuerzo medio: configuración, sin lógica delicada).
   KDE Plasma con el tema Fluent (estilo Windows 11), barra de tareas centrada,
   menú Inicio, fondo y marca Chamows.
2. **Acceso remoto** (esfuerzo alto: es la puerta de entrada a la máquina).
   Una sola sesión de escritorio en TigerVNC. Por navegador: nginx con HTTPS +
   contraseña delante de noVNC. Por RDP: xrdp reenviando a la misma sesión.
   Contraseña creada al lanzar la máquina, nunca dentro de la imagen.
3. **Tienda Chamows** (esfuerzo alto: muchas rutas y casos de error).
   App de escritorio en Python/Qt con catálogo curado (APT, Flathub y `.exe`
   vía Wine), búsqueda en todo Flathub y botón para instalar cualquier `.exe`.
4. **Imagen y lanzamiento** (esfuerzo medio: comandos de AWS conocidos).
   Plantilla de Packer y scripts `launch.sh` / `terminate.sh` con la AWS CLI.
5. **Pruebas** (esfuerzo medio): tests de la tienda, shellcheck,
   `packer validate`. El arranque real en AWS lo prueba Nacho con su cuenta.

## Decisiones

| # | Elegido | Consideré y descarté | Por qué |
|---|---------|----------------------|---------|
| 1 | Linux (Ubuntu 24.04) como base | SO propio desde cero; ReactOS | Desde cero no es viable. ReactOS es alfa y no corre bien en EC2. |
| 2 | Ubuntu 24.04 LTS (Plasma 5.27) | Ubuntu 26.04 (Plasma 6) | 24.04 tiene soporte hasta 2029 y sesión X11 estable, que TigerVNC necesita. Plasma 6 va retirando X11. Migrar es un paso futuro. |
| 3 | KDE Plasma + tema Fluent (commit fijado) | XFCE + Chicago95; Cinnamon; GNOME | Plasma es el que mejor imita Windows 11 (barra centrada, menú Inicio, ventanas). Fijar el commit evita que un cambio externo rompa la imagen. |
| 4 | Una sesión TigerVNC compartida por navegador y RDP | xrdp nativo + Guacamole | Una sola sesión: abres por navegador o RDP y ves lo mismo. Guacamole añade Java, Docker y base de datos para un prototipo. |
| 5 | nginx (HTTPS + contraseña) delante de noVNC | Exponer noVNC directo | La contraseña de VNC solo usa 8 caracteres. nginx añade una contraseña completa y cifrado en el navegador. |
| 6 | VNC escucha solo en localhost | Abrir 5901 | Nadie de fuera habla con VNC; solo nginx y xrdp. |
| 7 | Contraseña generada en `launch.sh` y pasada como user-data | Contraseña fija en la AMI | Una contraseña en la imagen sería la misma en todas las máquinas. Si no llega user-data, el primer arranque genera una aleatoria y la muestra en la consola de EC2. |
| 8 | Certificados TLS y claves de xrdp se regeneran en el primer arranque | Generarlos al construir | Si se generan en la AMI, todas las copias comparten la clave privada. |
| 9 | Grupo de seguridad solo abierto a tu IP (/32) | Abierto a todo internet | Reduce mucho la superficie de ataque, sobre todo por el límite de 8 caracteres de VNC en RDP. |
| 10 | Tienda en Python + PyQt5 | Web app; KDE Discover | PyQt5 viene empaquetado en Ubuntu, encaja con Plasma y es fácil de probar. Discover no instala `.exe`. |
| 11 | Flatpak en modo usuario (`--user`) | Flatpak de sistema | No necesita permisos de administrador ni diálogos de contraseña. |
| 12 | APT solo vía un ayudante root con lista permitida (catálogo) | sudo completo para la tienda | La tienda solo puede instalar los paquetes APT del catálogo. Un fallo en la UI no da root. |
| 13 | `.exe` del catálogo con SHA-256 verificado | Descargar sin verificar | Si el archivo cambia o lo manipulan, la instalación se detiene. |
| 14 | Firefox desde el repositorio APT de Mozilla, con huella de la clave comprobada | Firefox snap | Snap dentro de una sesión VNC sin logind completo da problemas. |
| 15 | Español (es_ES) e idioma de teclado `es` por defecto | Inglés | Nacho escribe en español. |
| 16 | Compositor de ventanas apagado | Efectos activados | En VNC no hay GPU; los efectos lo hacen lento. |
| 17 | Bloqueo de pantalla desactivado | Bloqueo activado | El acceso ya pide contraseña; un segundo bloqueo dentro de VNC confunde. |
| 18 | Instancia por defecto `t3.large`, región `eu-west-1`, disco 30 GB | `t3.medium`; `eu-south-2` | Plasma + Wine necesitan ~4-8 GB de RAM. `eu-west-1` tiene todo el catálogo de AMIs de Ubuntu; la región es una variable. |
| 19 | Plasma arranca con `dbus-run-session` y `systemdBoot=false` | Arranque de Plasma con servicios de systemd de usuario | El escritorio vive dentro de un servicio de sistema, no de un inicio de sesión gráfico; el arranque clásico evita que Plasma use un bus D-Bus distinto al de la sesión. |
| 20 | Paquetes dudosos como opcionales (`tigervnc-tools`, packs de idioma, `kde-spectacle`) | Lista fija | Si un nombre no existe en Ubuntu 24.04, la instalación sigue en lugar de fallar entera. |

## Límites conocidos del prototipo

- La contraseña de VNC (y por tanto la de RDP) solo usa los primeros 8 caracteres.
- El certificado HTTPS es autofirmado: el navegador mostrará un aviso.
- Wine no ejecuta todos los programas de Windows.
- Desinstalar un `.exe` abre el desinstalador de Wine; no es automático.
- La imagen no se ha arrancado en AWS desde este entorno (sin credenciales).
- No hay sonido: VNC no transmite audio.
- La contraseña de lanzamiento queda también en los datos de usuario de la
  instancia (visible para quien tenga permisos de EC2 en tu cuenta). Cámbiala
  con `sudo chamows-set-password` si eso te preocupa.
- Si tu IP pública cambia, vuelve a ejecutar `launch.sh` o añade la nueva IP
  al grupo de seguridad `chamows-sg`.
