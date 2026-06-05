#!/usr/bin/env python3
import os
import sys
import argparse

# Explicit layout matrix pairs
LINK_MAPS = {
    "fedora": {
        "links": {"disk.fedora": "fedora-40-cloud-base.qcow2", "initrd.fedora": "fedora-40-initrd", "vmlinuz.fedora": "fedora-40-vmlinuz"}
    },
    "cirros": {
        "links": {"disk.cirros": "cirros-0.6.2-x86_64-disk.img", "initrd.cirros": "cirros-0.6.2-x86_64-initramfs", "vmlinuz.cirros": "cirros-0.6.2-x86_64-kernel"}
    },
    "ubuntu": {
        "links": { "initrd.ubuntu": "ubuntu-noble-initrd-minimal", "vmlinuz.ubuntu": "ubuntu-noble-vmlinuz-minimal" }
    }
}

def clean_and_link(dist_name):
    meta = LINK_MAPS[dist_name]["links"]
    print(f"[Linker] Aligning symlink matrix entries for: '{dist_name}'...")
    
    for link_node, target_src in meta.items():
        # Check if src binary exists before linking
        if not os.path.exists(target_src):
            print(f" -> \033[93m[Warning] Target binary '{target_src}' missing. Skipping shortcut link node creation.\033[0m")
            continue

        # Atomic unlink update protection rules
        if os.path.islink(link_node) or os.path.exists(link_node):
            os.remove(link_node)
            
        print(f" -> Linking: {link_node} -> {target_src}")
        os.symlink(target_src, link_node)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    parser = argparse.ArgumentParser(description="Multi-Distro PCIe Symlink Management Layer Engine")
    parser.add_argument(
        "target", 
        choices=["fedora", "cirros", "ubuntu", "all"], 
        help="Specify target OS to map, or choose 'all' to update all profiles."
    )
    args = parser.parse_args()

    if args.target == "all":
        for target_key in LINK_MAPS.keys():
            clean_and_link(target_key)
    else:
        clean_and_link(args.target)

    print("\033[92m[Linker] Workspace shortcut structures updated successfully!\033[0m")

if __name__ == "__main__":
    main()
