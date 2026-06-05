#!/usr/bin/env bash
set -euo pipefail

LINUX_TOP_DIR="${LINUX_TOP_DIR:-/home/purple/kernel/linux}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE}")" && pwd)"

show_usage() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  clean    Perform a full 'make clean' before compiling (Optional)"
    echo "  help, -h Display this help menu"
    echo ""
    echo "Environment Variables:"
    echo "  LINUX_TOP_DIR Path to Linux source tree"
    echo "                Current value: ${LINUX_TOP_DIR}"
    echo "Example:"
    echo "  LINUX_TOP_DIR=/path/to/linux $(basename "$0") clean"
    exit 0
}

DO_CLEAN=false
if [[ $# -gt 0 ]]; then
    case "$1" in
        clean)
            DO_CLEAN=true
            ;;
        help|-h|--help)
            show_usage
            ;;
        *)
            echo -e "\033[91m[Build] ERROR: Unknown option: $1\033[0m"
            show_usage
            ;;
    esac
fi

if [ ! -d "${LINUX_TOP_DIR}" ]; then
    echo -e "\033[91m[Build] FATAL ERROR: Path does not exist: LINUX_TOP_DIR=${LINUX_TOP_DIR}\033[0m"
    echo "Please set the correct path or export the LINUX_TOP_DIR variable."
    exit 1
fi

cd "${LINUX_TOP_DIR}"

if [ ! -f "Makefile" ] || [ ! -f "arch/x86/Kconfig" ]; then
    echo -e "\033[91m[Build] FATAL ERROR: Path is not a valid Linux Kernel source tree: ${LINUX_TOP_DIR}\033[0m"
    exit 1
fi

if [ "$DO_CLEAN" = true ]; then
    echo "[Build] Running optional repository clearance (make clean)..."
    make clean
fi

echo "[Build] Generating architecture baseline defconfig..."
make defconfig

echo "[Build] Injecting custom PCIe Co-Simulation configuration changes..."
./scripts/config --enable CONFIG_PCI \
                 --enable CONFIG_UIO \
                 --enable CONFIG_BTRFS_FS \
                 --enable CONFIG_EXT4_FS \
                 --enable CONFIG_EXT4_USE_FOR_EXT2 \
                 --enable CONFIG_DEVTMPFS \
                 --enable CONFIG_DEVTMPFS_MOUNT \
                 --enable CONFIG_EFI \
                 --enable CONFIG_EFI_STUB \
                 --enable CONFIG_EFIVARS \
                 --enable CONFIG_PCI_ENDPOINT \
                 --enable CONFIG_PCI_ENDPOINT_CONFIGFS \
                 --enable CONFIG_PCI_EPF_TEST \
                 --enable CONFIG_PCI_ENDPOINT_TEST \
                 --disable CONFIG_PSTORE \
                 --disable CONFIG_AGP \
                 --disable CONFIG_DRM \
                 --disable CONFIG_FB \
                 --disable CONFIG_BLK_DEV_DM

echo "[Build] Synchronizing dependency tables (make olddefconfig)..."
make olddefconfig

echo "[Build] Compiling bzImage image..."
make -j$(nproc) bzImage

TARGET_DIR="${SCRIPT_DIR}/third_party/os/images/linux"

echo "[Build] Deploying bzImage to pcie_cosim asset tree..."
cp arch/x86/boot/bzImage "${TARGET_DIR}/vmlinuz.v6.8_pcie_cosim"