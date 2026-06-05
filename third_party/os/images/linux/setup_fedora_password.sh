#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE}")" && pwd)"
cd "${SCRIPT_DIR}"

DISK_IMG="disk.fedora"

echo "[Shell] Updating Fedora account credentials and access layers..."

virt-sysprep -a "$DISK_IMG" \
  --hostname pcie-cosim \
  --enable customize \
  --run-command 'useradd -m -s /bin/bash fedora' \
  --run-command 'echo "fedora:fedora" | chpasswd' \
  --run-command 'echo "root:fedora" | chpasswd' \
  --run-command 'usermod -aG wheel fedora' \
  --run-command 'echo "fedora ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-pcie-cosim-user' \
  --run-command 'chmod 0440 /etc/sudoers.d/90-pcie-cosim-user' \
  --selinux-relabel

echo -e "\033[92m[Shell] Fedora account credentials compiled inside the ${DISK_IMG} image\033[0m"
