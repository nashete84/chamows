#!/bin/bash
# Runs once on the first boot of each new machine. Anything secret or unique
# per machine is created here, never inside the AMI.
set -euo pipefail
umask 077

install -d -m 0755 /etc/chamows /var/lib/chamows
install -d -m 0700 /etc/chamows/tls

# HTTPS certificate for the browser login (self-signed).
openssl req -x509 -newkey rsa:3072 -nodes -days 825 -subj "/CN=chamows" \
  -keyout /etc/chamows/tls/key.pem -out /etc/chamows/tls/cert.pem 2>/dev/null
chmod 0600 /etc/chamows/tls/key.pem

# RDP: new TLS certificate and RSA keys for xrdp.
make-ssl-cert generate-default-snakeoil --force-overwrite
xrdp-keygen xrdp /etc/xrdp/rsakeys.ini >/dev/null
chown root:xrdp /etc/xrdp/rsakeys.ini 2>/dev/null || true
chmod 0640 /etc/xrdp/rsakeys.ini

# Password: from launch.sh (cloud-init writes it), or a random one.
if [ -s /etc/chamows/initial-password ]; then
  /usr/lib/chamows/chamows-set-password < /etc/chamows/initial-password
  shred -u /etc/chamows/initial-password 2>/dev/null || rm -f /etc/chamows/initial-password
  echo "Chamows: contraseña aplicada desde los datos de lanzamiento."
else
  PW=$(openssl rand -base64 18 | tr -dc 'A-Za-z0-9' | cut -c1-16)
  printf '%s\n' "$PW" | /usr/lib/chamows/chamows-set-password
  printf '%s\n' "$PW" > /root/chamows-password.txt
  # Shown in the EC2 console output ("Obtener registro del sistema").
  echo "CHAMOWS-PASSWORD: $PW" > /dev/console
fi

touch /var/lib/chamows/firstboot.done
