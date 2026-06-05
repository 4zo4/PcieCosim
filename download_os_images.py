#!/usr/bin/env python3
import os
import sys
import argparse
import datetime
import urllib.request

# Default configuration option
SELECT = "cirros"

CIRROS_BASE_URL = "https://cirros-cloud.net/"

UBUNTU_BASE_URL = "https://cloud-images.ubuntu.com/minimal/releases/noble/release/"
UBUNTU_DISK_URL = UBUNTU_BASE_URL
UBUNTU_KERNEL_URL = UBUNTU_BASE_URL + "unpacked/"

FEDORA_BASE_URL = "https://elmirror.cl/fedora/linux/releases/"
FEDORA_TARGET_RELEASE = "40/"
FEDORA_TARGET_DISK_PATH = "Cloud/x86_64/images/"
FEDORA_TARGET_PXE_PATH = "Everything/x86_64/os/images/pxeboot/"
FEDORA_DISK_URL = FEDORA_BASE_URL + FEDORA_TARGET_RELEASE + FEDORA_TARGET_DISK_PATH
FEDORA_PXE_URL = FEDORA_BASE_URL + FEDORA_TARGET_RELEASE + FEDORA_TARGET_PXE_PATH

# Ubuntu Assets Mapping Matrix
UBUNTU_ASSETS = {
    "disk": {
        "remote_name": "ubuntu-24.04-minimal-cloudimg-amd64.img",
        "unique_name": "ubuntu-noble-disk.img",
        "generic_link": "disk.ubuntu",
        "url": UBUNTU_DISK_URL
    },
    "kernel": {
        "remote_name": "ubuntu-24.04-minimal-cloudimg-amd64-vmlinuz-generic",
        "unique_name": "ubuntu-noble-vmlinuz-minimal",
        "generic_link": "vmlinuz.ubuntu",
        "url": UBUNTU_KERNEL_URL
    }
}

# Fedora Assets Mapping Matrix
FEDORA_ASSETS = {
    "disk": {
        "remote_name": "Fedora-Cloud-Base-Generic.x86_64-40-1.14.qcow2",
        "unique_name": "fedora-40-cloud-base.qcow2",
        "generic_link": "disk.fedora",
        "url": FEDORA_DISK_URL
    },
    "kernel": {
        "remote_name": "vmlinuz",
        "unique_name": "fedora-40-vmlinuz",
        "generic_link": "vmlinuz.fedora",
        "url": FEDORA_PXE_URL
    },
    "initrd": {
        "remote_name": "initrd.img",
        "unique_name": "fedora-40-initrd",
        "generic_link": "initrd.fedora",
        "url": FEDORA_PXE_URL
    }
}

# CirrOS Assets Mapping Matrix
CIRROS_ASSETS = {
    "disk": {
        "remote_name": "cirros-0.6.2-x86_64-disk.img",
        "unique_name": "cirros-0.6.2-x86_64-disk.img",
        "generic_link": "disk.cirros",
        "url": CIRROS_BASE_URL
    },
    "kernel": {
        "remote_name": "cirros-0.6.2-x86_64-kernel",
        "unique_name": "cirros-0.6.2-x86_64-kernel",
        "generic_link": "vmlinuz.cirros",
        "url": CIRROS_BASE_URL
    },
    "initrd": {
        "remote_name": "cirros-0.6.2-x86_64-initramfs",
        "unique_name": "cirros-0.6.2-x86_64-initramfs",
        "generic_link": "initrd.cirros",
        "url": CIRROS_BASE_URL
    }
}

# Target folder placement mapping
PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
IMAGE_DIR = os.path.join(PROJECT_ROOT, "third_party", "os", "images", "linux")

def parse_arguments():
    """Defines and processes script options and targeted asset filters."""
    parser = argparse.ArgumentParser(
        description="Distribution-Agnostic Asset Manager and Symlink Linker tool."
    )
    parser.add_argument(
        "dist",
        choices=["ubuntu", "fedora", "cirros"],
        default=SELECT,
        help="Target Linux distribution context selector profile."
    )
    parser.add_argument(
        "assets",
        nargs="*",
        choices=["disk", "kernel", "initrd"],
        help="Target specific assets to update. If omitted, manages ALL assets for the dist."
    )
    return parser.parse_args()

def setup_workspace():
    """Ensure the target workspace directory footprint exists cleanly."""
    if not os.path.exists(IMAGE_DIR):
        print(f"[Assets] Creating workspace directory footprint at: {IMAGE_DIR}")
        os.makedirs(IMAGE_DIR, exist_ok=True)

import datetime

def download_file(remote_file, base_url, local_path):
    """Download the asset block dynamically with progress visualization telemetry and timing stamps."""
    url = base_url + remote_file

    # Capture and print the exact timestamp when the network transaction initiates
    start_time = datetime.datetime.now()
    print(f"[Assets] Downloading: {url} -> {local_path}")
    print(f"[Assets] Started at: {start_time.strftime('%Y-%m-%d %H:%M:%S')}")

    def progress_callback(block_num, block_size, total_size):
        read_so_far = block_num * block_size
        if total_size > 0:
            percent = min(100, (read_so_far * 100) // total_size)
            sys.stdout.write(f"\r[Assets] Progress: {percent}% ({read_so_far // 1024 // 1024}MB / {total_size // 1024 // 1024}MB)")
            sys.stdout.flush()
        else:
            sys.stdout.write(f"\r[Assets] Progress: Vector bytes read: {read_so_far}")
            sys.stdout.flush()

    urllib.request.urlretrieve(url, local_path, reporthook=progress_callback)

    # Capture the final timestamp and calculate the dynamic duration delta
    end_time = datetime.datetime.now()
    duration = end_time - start_time

    # Format the time measurements to drop seconds precision neatly
    duration_str = str(duration).split('.')[0] # Strips microseconds for clean tracking

    print("\n[Assets] Download transaction finalized successfully.")
    print(f"[Assets] Finished at: {end_time.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"[Assets] Total Download Duration: {duration_str}")


def update_symbolic_links(active_assets):
    """Manages relative symbolic shortcut links dynamically for the active targets."""
    original_cwd = os.getcwd()
    os.chdir(IMAGE_DIR)

    try:
        print("[Assets] Refreshing targeted symbolic links inside workspace...")

        # Clear out any old target specific symlinks for active assets to prevent contamination
        for name, meta in active_assets.items():
            link_path = meta["generic_link"]
            target_path = meta["unique_name"]

            if os.path.islink(link_path) or os.path.exists(link_path):
                os.remove(link_path)

            if os.path.exists(target_path):
                print(f" -> Linking: {link_path} -> {target_path}")
                os.symlink(target_path, link_path)
            else:
                print(f" -> \033[93m[Warning] Skipping link creation for '{link_path}'. Binary asset file not found.\033[0m")
    finally:
        os.chdir(original_cwd)

def main():
    args = parse_arguments()
    setup_workspace()

    # 1. Map target distribution workspace configs
    if args.dist == "ubuntu":
        dist_manifest = UBUNTU_ASSETS
    elif args.dist == "fedora":
        dist_manifest = FEDORA_ASSETS
    elif args.dist == "cirros":
        dist_manifest = CIRROS_ASSETS
    else:
        dist_manifest = CIRROS_ASSETS

    # 2. Apply asset filtering rules
    if not args.assets:
        targeted_keys = list(dist_manifest.keys())
        print(f"[Assets] No asset filters specified. Managing ALL files for profile: '{args.dist}'")
    else:
        targeted_keys = [key for key in args.assets if key in dist_manifest]
        ignored_keys = [key for key in args.assets if key not in dist_manifest]

        if ignored_keys:
            print(f"[Assets][Info] Safely ignoring filters {ignored_keys} (Not applicable for '{args.dist}').")
        print(f"[Assets] Targeting explicit asset filters: {targeted_keys} for profile: '{args.dist}'")

    # 3. Process Download Loop Matrix on active targets
    active_selection_pool = {}
    for key in targeted_keys:
        meta = dist_manifest[key]
        active_selection_pool[key] = meta
        local_unique_file = os.path.join(IMAGE_DIR, meta["unique_name"])

        if not os.path.exists(local_unique_file):
            download_file(meta["remote_name"], meta["url"], local_unique_file)
        else:
            print(f"[Assets] Cache target found for '{meta['unique_name']}'. Skipping download.")

    # 4. Handle atomic symlink mappings updates for active components
    update_symbolic_links(active_selection_pool)
    print("\033[92m[Assets] Workspace alignment complete. Ready for agent deployment!\033[0m")

if __name__ == "__main__":
    main()
