#!/bin/bash
# Run by Packer at the end of the build. Removes anything that must be unique
# per machine (keys, passwords, machine id) so every instance makes its own.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "ejecuta con sudo" >&2; exit 1; }

apt-get clean
rm -rf /var/lib/apt/lists/*

# Chamows secrets: recreated by chamows-firstboot.service.
rm -rf /etc/chamows/tls /etc/chamows/htpasswd /etc/chamows/initial-password
rm -f /var/lib/chamows/firstboot.done /root/chamows-password.txt
rm -f /home/chamows/.config/tigervnc/passwd
passwd -l chamows >/dev/null 2>&1 || true

# Per-machine keys from package installs.
rm -f /etc/ssl/private/ssl-cert-snakeoil.key /etc/ssl/certs/ssl-cert-snakeoil.pem
rm -f /etc/xrdp/rsakeys.ini
rm -f /etc/ssh/ssh_host_*

# SSH keys Packer used to log in during the build.
rm -f /home/ubuntu/.ssh/authorized_keys /root/.ssh/authorized_keys

# Make cloud-init treat the next boot as a brand-new instance.
cloud-init clean --logs --machine-id 2>/dev/null || {
  cloud-init clean --logs
  truncate -s 0 /etc/machine-id
}

rm -f /root/.bash_history /home/ubuntu/.bash_history
sync
