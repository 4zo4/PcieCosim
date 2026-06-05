#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE}")" && pwd)"
cd "${SCRIPT_DIR}"

DISK_IMG="disk.ubuntu"

echo "[Shell] Creating Ubuntu user account and injecting password..."

virt-sysprep -a "$DISK_IMG" \
  --hostname pcie-cosim \
  --enable customize \
  --run-command 'useradd -m -s /bin/bash ubuntu' \
  --run-command 'echo "ubuntu:ubuntu" | chpasswd' \
  --run-command 'echo "root:ubuntu" | chpasswd' \
  --run-command 'usermod -aG sudo ubuntu' \
  --run-command 'echo "ubuntu ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-pcie-cosim-user' \
  --run-command 'chmod 0440 /etc/sudoers.d/90-pcie-cosim-user'

echo -e "\033[92m[Shell] Ubuntu account credentials compiled inside the ${DISK_IMG} image\033[0m"