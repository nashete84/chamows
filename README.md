# Chamows

Chamows es un sistema operativo con aspecto de Windows 11 que corre en una
máquina virtual de AWS. Por dentro es Ubuntu 24.04 con KDE Plasma. Entras a su
escritorio desde el navegador o con Escritorio remoto (RDP), y trae la
**Tienda Chamows** para instalar aplicaciones.

Estado: **prototipo 0.1**.

## Qué trae

- Escritorio estilo Windows 11: barra de tareas centrada, menú Inicio, botones
  de ventana a la derecha, tema claro Fluent.
- Acceso por navegador (`https://IP`) y por RDP (`IP:3389`) a la **misma** sesión.
- Tienda Chamows:
  - Catálogo curado: Chrome, VS Code, LibreOffice, VLC, GIMP, Spotify,
    Discord, Telegram, Zoom, OBS, Notepad++ y 7-Zip (estas dos son de Windows).
  - Búsqueda en todo Flathub (miles de apps).
  - Instalar cualquier `.exe` o `.msi` con Wine.
- Firefox, explorador de archivos, terminal y editor de texto preinstalados.
- Interfaz en español.

## Cómo usarlo

Necesitas una cuenta de AWS, la [AWS CLI](https://aws.amazon.com/cli/)
configurada (`aws configure`) y [Packer](https://developer.hashicorp.com/packer/install).
En Windows, ejecuta los scripts desde WSL o Git Bash.

### 1. Construir la imagen (una vez, ~25 minutos)

```bash
cd packer
packer init .
packer build .                       # región eu-west-1 por defecto
# packer build -var region=us-east-1 .
```

Al terminar tendrás una AMI llamada `chamows-0.1-FECHA` en tu cuenta.

### 2. Lanzar una máquina

```bash
./scripts/launch.sh                  # usa la AMI de Chamows más nueva
# ./scripts/launch.sh --region us-east-1 --type t3.xlarge
```

El script muestra la dirección, el usuario (`chamows`) y una contraseña nueva.
Solo tu IP actual puede conectarse.

### 3. Entrar

- **Navegador:** abre `https://IP`, acepta el aviso del certificado, escribe
  usuario y contraseña, y luego otra vez la contraseña en la pantalla de VNC.
- **RDP:** conecta a `IP:3389` con Escritorio remoto, elige la sesión
  *Chamows* y escribe la contraseña.

### 4. Borrar la máquina (para dejar de pagar)

```bash
./scripts/terminate.sh i-XXXXXXXX --region eu-west-1
```

### Cambiar la contraseña

Dentro de Chamows, en una terminal:

```bash
sudo chamows-set-password
```

### Instalar sin Packer

En una instancia Ubuntu 24.04 ya creada (al menos 4 GB de RAM):

```bash
git clone https://github.com/nashete84/chamows && cd chamows
sudo ./scripts/install.sh --now
```

La contraseña aparece al final.

### Probar en tu Mac, sin AWS

Con [Multipass](https://multipass.run) creas una máquina Ubuntu dentro del Mac:

```bash
brew install multipass
multipass launch 24.04 --name chamows --cpus 2 --memory 6G --disk 30G
multipass shell chamows
# dentro de la máquina:
git clone https://github.com/nashete84/chamows && cd chamows
sudo ./scripts/install.sh --now
```

Después, en el Mac, `multipass info chamows` te da la IP; abre `https://ESA-IP`.

En Macs con chip Apple (M1 a M4) la máquina es ARM: no hay Wine, así que no
se instalan `.exe`, y algunas apps de Flathub (Chrome, Spotify, Zoom, Discord)
no existen para ARM. Todo lo demás funciona igual.

## Estructura

| Carpeta | Contenido |
|---------|-----------|
| `scripts/install.sh` | Convierte un Ubuntu 24.04 en Chamows |
| `scripts/launch.sh`, `terminate.sh` | Crear y borrar máquinas en AWS |
| `packer/` | Plantilla para construir la AMI |
| `rootfs/` | Archivos que se copian tal cual al sistema |
| `store/` | Tienda Chamows (Python + Qt), su catálogo y sus tests |
| `DECISIONS.md` | Plan, decisiones tomadas y límites conocidos |

## Tests

```bash
pip install PyQt5            # solo para abrir la tienda en local
python3 -m unittest discover -s store/tests
```

## Añadir una app al catálogo

Edita `store/catalog.json`. Cada app tiene `source`:

- `apt`: paquete de Ubuntu (`"package": "vlc"`).
- `flatpak`: id de Flathub (`"package": "com.spotify.Client"`).
- `wine`: instalador de Windows. Requiere `url` (https), `sha256` del archivo,
  `installed_path` (ruta dentro de `C:\`) y opcionalmente `args` para
  instalación silenciosa.

Los tests validan el catálogo.
