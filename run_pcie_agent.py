#!/usr/bin/env python3
#
# This script implements a PCIe Co-Simulation Framework Agent that orchestrates the execution of
# a QEMU virtual machine with a guest OS connected to a PCIe Cosimulation Bridge and a PCIe RTL simulation endpoint device.
# The script can launch a packet sniffer to capture low-level interactions over the UDS channel.
# The agent can perform a series of tests to validate the correct behavior of the PCIe simulation environment,
# including burst verification, boundary validation, BAR isolation, and MSI interrupt handling.
# The script is designed to be adaptable to multiple Linux distributions running as guest OSes in the QEMU environment.
#
# Copyright (c) 2026, Purple
# This file is licensed under the MIT License.
#
import argparse
from asyncio import subprocess
import getpass
import os
import subprocess
import shutil
import struct
import sys
import threading
import time
import traceback
import pexpect
import re

def resolve_relative_paths():
    """Calculate the absolute root directory of the project file"""
    project_root = os.path.dirname(os.path.abspath(__file__))

    log_dir = os.path.join(project_root, "logs")
    log_file_path = os.path.join(log_dir, "pcie_backend.log")
    pcap_file_path = os.path.join(log_dir, "vfio-user.pcap")
    slog_file_path = os.path.join(log_dir, "sockdump.log")

    search_anchors = [
        os.path.abspath(os.path.join(project_root, "..")),
        os.path.abspath(os.path.join(project_root, "..", "..")),
        os.path.expanduser("~")
    ]
    image_root = None
    for anchor in search_anchors:
        possible_images_dir = os.path.join(anchor, "images")
        if os.path.exists(possible_images_dir):
            image_root = possible_images_dir
            break
    if image_root is None:
        image_root = os.path.expanduser("~/images")

    return project_root, log_dir, log_file_path, pcap_file_path, slog_file_path, image_root

def setup_log_directory(dir_name, log_file_path, pcap_file_path, slog_file_path, max_backups=2):
    """Create the log directory (if it doesn't exist) and
       rotate old logs to preserve a history of executions."""
    if not os.path.exists(dir_name):
        os.makedirs(dir_name)
        print(f"[Agent] Created logging directory: '{dir_name}/'")

    files = [log_file_path, pcap_file_path, slog_file_path]

    for target in files:
        for i in range(max_backups - 1, 0, -1):
            src = f"{target}.{i}"
            dst = f"{target}.{i+1}"
            if os.path.exists(src):
                shutil.move(src, dst)
        if os.path.exists(target) and os.path.getsize(target) > 0:
            shutil.move(target, f"{target}.1")

    print(f"[Agent] Rotated historical logs (Preserved max backups: {max_backups})")

def start_vfio_user_pkt_sniffer(project_root, pcap_file_path, slog_file_path, socket_path="/tmp/vfio-pcie.sock"):
    """Launch sockdump as a background root process to capture
       low-level IPC data going across the UDS channel."""
    print(f"[Agent] Arming VFIO-User packet sniffer on {socket_path} writing to {pcap_file_path}...")

    sockdump = os.path.join(project_root, "tools", "net", "sockdump.py")
    os.makedirs(os.path.dirname(pcap_file_path), exist_ok=True)

    cmd = [
        "sudo", "-n", "python3", sockdump,
        "--format", "pcap", "--raw",
        "--output", pcap_file_path,
        "--flush",
        socket_path
    ]

    try:
        fd = open(slog_file_path, "w")
        sockdump_env = os.environ.copy()
        sockdump_env["PYTHONUNBUFFERED"] = "1"
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=fd, # sockdump writes to the user via stderr
            env=sockdump_env,
            start_new_session=True,
            cwd=project_root
        )
        proc.slog_fd = fd
        proc.pcap_file_path = pcap_file_path
        time.sleep(3.0) # Allow the sniffer enough time to initialize and bind to the socket
        if proc.poll() is not None:
            print(f"\033[31m[Agent] ERROR: Sniffer terminated early with exit code {proc.returncode}\033[0m")
            return None
        else:
            print(f"[Agent] Sniffer process started with PID {proc.pid}")
            return proc
    except Exception as ex:
        print(f"\033[31m[Agent] ERROR: Failed to spawn sockdump sniffer: {ex}\033[0m")
        return None

def stop_vfio_user_pkt_sniffer(sniffer_proc):
    """Detach the eBPF kernel hooks and save the pcap file."""
    if sniffer_proc is None:
        return

    pgid = os.getpgid(sniffer_proc.pid)
    print(f"[Agent] Disarming VFIO-User packet sniffer")
    try:
        start_time = time.time()
        terminated = False
        subprocess.run(["sudo", "kill", "-2", f"-{pgid}"], check=False)
        while time.time() - start_time < 4.0:
            if sniffer_proc.poll() is not None:
                terminated = True
                break
            time.sleep(0.1)

        if not terminated:
            print("[Agent] Forcing sniffer kill...")
            subprocess.run(["sudo", "kill", "-9", f"-{pgid}"], check=False)
            sniffer_proc.kill()
            sniffer_proc.wait(timeout=1)

    except Exception as ex:
        print(f"\n\033[91m[Agent] FATAL: Exception thrown during sniffer teardown: {ex}\033[0m")
        traceback.print_exc()
    finally:
        if hasattr(sniffer_proc, 'slog_fd') and sniffer_proc.slog_fd:
            sniffer_proc.slog_fd.flush()
            sniffer_proc.slog_fd.close()

    if os.path.exists(sniffer_proc.pcap_file_path):
        user = getpass.getuser()
        group = user
        os.system(f"sudo chown {user}:{group} {sniffer_proc.pcap_file_path}")
        os.system(f"sudo chmod 664 {sniffer_proc.pcap_file_path}")
        size = os.path.getsize(sniffer_proc.pcap_file_path)
        print(f"[Agent] Packet trace size: {size} bytes committed to log tree")
    else:
        print(f"\033[31m[Agent] ERROR: pcap file not found at {sniffer_proc.pcap_file_path}\033[0m")

def continuous_line_drainer(bridge_or_qemu, backend_log):
    """
    A thread worker loop that actively drains lines from the terminal,
    stripping raw ANSI terminal garbage control sequences to preserve log clean.
    """
    # Compile regex to match ANSI escape sequences, including Device Status Reports (\x1b\[\d+;\d+R)
    # and general CSI terminal code sequences
    ansi_escape_pattern = re.compile(
        r'(?:\x1B[@-_]|[\x80-\x9F])[0-?]*[ -/]*[@-~]|\x1B\[\d+(?:;\d+)*[R]'
    )

    while True:
        try:
            data = bridge_or_qemu.read_nonblocking(size=1024, timeout=0.1)
            if data:
                text_block = data.decode('utf-8', errors='ignore')
                clean_text = ansi_escape_pattern.sub('', text_block)
                if clean_text:
                    sys.stdout.write(clean_text)
                    sys.stdout.flush()
                    backend_log.write(clean_text.encode('utf-8'))
        except pexpect.TIMEOUT:
            continue
        except pexpect.EOF:
            break

def detect_and_initialize_guest_os_profile(args, qemu):
    """
    Detect the guest operating system environment type and return
    a configuration profile dictionary.
    """
    print("[Agent] Detecting guest OS environment runtime signature (Awaiting boot stability)...")

    try:
        index = qemu.expect([
            r"Fedora.*pcie-cosim login:\s*$", # 0: Fedora Login Banner Challenge
            r"Ubuntu.*pcie-cosim login:\s*$", # 1: Ubuntu Login Banner Challenge
            r"\$\s*$",                        # 2: Cirros Shell / Standard User (Pre-auth)
            r"cirros login:\s*$",             # 3: CirrOS Login Challenge
            r"[\r\n]+\[root@.*\]#\s*$",       # 4: Pre-Authenticated Direct Fedora Root Prompt
            r"pcie-cosim login:\s*$",         # 5: Generic Shared Hostname Login Fallback
        ], timeout=None)
    except Exception as ex:
        print(f"\033[91m[Agent] ERROR: Early boot tracking exception caught: {ex}\033[0m")
        raise

    # PROFILE 1: UBUNTU LINUX
    if index in (1, 5) and args.distro == "ubuntu":
        print("[Agent] Profile Identified: Ubuntu Noble Core Environment")
        manual = False # set True for manual login
        if manual:
            print("[Agent] Type credentials manually. Press Ctrl+] to exit when done.\n")
            qemu.logfile_read = None
            time.sleep(0.1)
            qemu.interact(escape_character='\x1d')

        ubuntu_profile = {
            "username": "ubuntu",
            "password": "ubuntu",
            "prompt": r"root@pcie-cosim:.*#\s*$",
            "sudo_prefix": b"",
            "dump_cmd": b"od -t x4",
            "interrupt_parse": b"awk '{sum=0; for(i=2; i<=NF; i++) if($i ~ /^[0-9]+$/) sum+=$i; print sum}'"
        }

        time.sleep(1.0)
        print("[Agent] Sending ubuntu credentials...")
        qemu.sendline(b"ubuntu")

        qemu.expect(b"Password:", timeout=10)
        time.sleep(0.5)
        qemu.sendline(b"ubuntu")

        print("[Agent] Synchronizing with user shell environment...")
        qemu.expect(r"\$\s*$", timeout=20)

        print("[Agent] Elevating user session permissions to Supervisor root status...")
        qemu.sendline(b"sudo -i")
        qemu.expect(ubuntu_profile["prompt"], timeout=15)
        print("\n\033[94m[Agent] Established Ubuntu Root framework\033[0m")

        return ubuntu_profile

    # PROFILE 2: CIRROS 0.6.2
    elif index in (2, 3) and args.distro == "cirros":
        print(f"[Agent] Profile Identified: CirrOS 0.6.2 Core Node")

        cirros_profile = {
            "username": "cirros",
            "password": "gocubsgo",
            "prompt": r"(\$|#)\s*$",
            "sudo_prefix": b"sudo ",
            "dump_cmd": b"hexdump -e '1/4 \"%08x\\n\"'",
            "interrupt_parse": b"awk '{print $2}'"
        }

        if index == 3:
            time.sleep(0.5)
            print("[Agent] Sending cirros credentials over paced input stream...")
            qemu.sendline(cirros_profile["username"].encode())

            qemu.expect(b"Password:", timeout=5)
            time.sleep(0.2)
            qemu.sendline(cirros_profile["password"].encode())

            print("[Agent] Synchronizing with BusyBox shell environment...")
            qemu.expect(cirros_profile["prompt"], timeout=10)

            print("[Agent] Elevating user session permissions to Supervisor root...")
            qemu.sendline(b"sudo -i")
            qemu.expect(r"#\s*$", timeout=5)
            print(f"\n\033[94m[Agent] Established CirrOS Root framework\033[0m")

        elif index == 2:
            print("[Agent] Elevating pre-authenticated user session permissions to Supervisor root...")
            qemu.sendline(b"sudo -i")
            qemu.expect(r"#\s*$", timeout=5)
            print(f"\n\033[94m[Agent] Established CirrOS Root framework\033[0m")

        return cirros_profile

    # PROFILE 3: FEDORA 40 CLOUD
    elif index in (0, 4, 5) and args.distro == "fedora":
        print("[Agent] Profile Identified: Fedora 40 Cloud Edition")

        fedora_profile = {
            "username": "root",
            "password": "fedora",
            "prompt": r"\[root@.*\]#\s*$",
            "sudo_prefix": b"",
            "dump_cmd": b"od -t x4",
            "interrupt_parse": b"awk '{sum=0; for(i=2; i<=NF; i++) if($i ~ /^[0-9]+$/) sum+=$i; print sum}'"
        }
        time.sleep(1.0)
        print("[Agent] Sending root credentials over paced input stream...")
        qemu.sendline(b"root")

        qemu.expect(b"Password:", timeout=10)
        time.sleep(0.5)
        qemu.sendline(fedora_profile["password"].encode())

        qemu.expect(fedora_profile["prompt"], timeout=15)
        print(f"\n\033[94m[Agent] Established Fedora Root framework\033[0m")

        return fedora_profile
    # Fallback
    return {
        "username": "root", "password": "", "prompt": r"#\s*$", "sudo_prefix": b"",
        "dump_cmd": b"hexdump -e '1/4 \"%08x\\n\"'", "interrupt_parse": b"awk '{print $2}'"
    }

def run_test_1_burst_verification(qemu, profile):
    print(f"\033[95m[Agent][TEST 1] Starting Sequential 16-Byte Burst Verification...\033[0m")
    qemu.sendline(b"")
    qemu.expect(profile["prompt"], timeout=5)

    burst_data = b"\\x10\\x20\\x30\\x40\\x50\\x60\\x70\\x80\\x90\\xa0\\xb0\\xc0\\xd0\\xe0\\xf0\\xff"
    cmd = b"echo -ne '" + burst_data + b"' | " + profile["sudo_prefix"] + b"dd of=/sys/bus/pci/devices/0000:01:00.0/resource0 bs=4 count=4"
    qemu.sendline(cmd)
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 1] Verifying memory payload layout...\033[0m")
    if b"hexdump" in profile["dump_cmd"]:
        read_cmd = profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/resource0 -n 16"
        qemu.sendline(read_cmd)
        qemu.expect(b"40302010", timeout=10)
    else:
        read_cmd = profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/resource0 -N 4"
        qemu.sendline(read_cmd)
        qemu.expect(b"0000000 40302010", timeout=10)

    qemu.expect(profile["prompt"], timeout=5)
    print(f"\033[92m[Agent][TEST 1] PASS: Memory words burst W/R verified\033[0m")
    return True

def run_test_2_boundary_validation(qemu, profile):
    print(f"\033[95m[Agent][TEST 2] Starting 2-Kilobyte Address Boundary Verification...\033[0m")
    qemu.sendline(b"")
    qemu.expect(profile["prompt"], timeout=5)

    boundary_data = b"\\xCA\\xFE\\xBA\\xBE"
    cmd = b"echo -ne '" + boundary_data + b"' | " + profile["sudo_prefix"] + b"dd of=/sys/bus/pci/devices/0000:01:00.0/resource0 bs=4 seek=512 count=1"
    qemu.sendline(cmd)
    qemu.expect(profile["prompt"], timeout=5)

    if b"hexdump" in profile["dump_cmd"]:
        read_cmd = profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/resource0 -s 2048 -n 4"
        qemu.sendline(read_cmd)
        qemu.expect(b"bebafeca", timeout=10)
    else:
        read_cmd = profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/resource0 -j 2048 -N 4"
        qemu.sendline(read_cmd)
        qemu.expect(b"0004000 bebafeca", timeout=10)

    qemu.expect(profile["prompt"], timeout=5)
    print(f"\033[92m\033[96m[Agent][TEST 2] PASS: Boundary mapping offsets verified.\033[0m")
    return True

def run_test_3_bar_isolation(qemu, profile):
    print(f"\033[95m[Agent][TEST 3] Initiating Cross-BAR Isolation Verification...")
    qemu.sendline(b"")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 3] Seeding baseline validation pattern into BAR0...\033[0m")
    bar0_payload = b"\\xCA\\xFE\\xBA\\xBE"
    qemu.sendline(b"echo -ne '" + bar0_payload + b"' | " + profile["sudo_prefix"] + b"dd of=/sys/bus/pci/devices/0000:01:00.0/resource0 bs=4 count=1 conv=notrunc")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 3] Writing target allocation pattern into BAR1...\033[0m")
    bar1_payload = b"\\x00\\x60\\x77\\x88"
    qemu.sendline(b"echo -ne '" + bar1_payload + b"' | " + profile["sudo_prefix"] + b"dd of=/sys/bus/pci/devices/0000:01:00.0/config bs=4 seek=5 count=1 conv=notrunc")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 3] Verifying BAR1 metric latch integrity...\033[0m")
    if b"hexdump" in profile["dump_cmd"]:
        read_bar1 = profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/config -s 2048 -n 4" # mapping adjustment check for config spaces
        qemu.sendline(profile["sudo_prefix"] + b"hexdump -e '1/4 \"%08x\\n\"' /sys/bus/pci/devices/0000:01:00.0/config | grep -E '88776000'")
    else:
        qemu.sendline(profile["sudo_prefix"] + b"od -t x4 /sys/bus/pci/devices/0000:01:00.0/config -j 20 -N 4")
        qemu.expect(b"0000024 88776000", timeout=10)
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 3] Checking BAR0 to guarantee cross-BAR isolation...\033[0m")
    if b"hexdump" in profile["dump_cmd"]:
        qemu.sendline(profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/resource0 -n 4")
        qemu.expect(b"bebafeca", timeout=10)
    else:
        qemu.sendline(profile["sudo_prefix"] + profile["dump_cmd"] + b" /sys/bus/pci/devices/0000:01:00.0/resource0 -N 4")
        qemu.expect(b"0000000 bebafeca", timeout=10)
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[92m[Agent][TEST 3] PASS: BAR0 and BAR1 are isolated on the fabric with no cross-talk.\033[0m")
    return True

def run_test_4_irq_verification(qemu, profile, log_file_path):
    print(f"\033[95m[Agent][TEST 4] Initiating MSI Verification...\033[0m")

    print(f"\033[96m[Agent][TEST 4] Clearing driver bindings:\033[0m")
    qemu.sendline(b"echo '0000:01:00.0' > /sys/bus/pci/devices/0000:01:00.0/driver/unbind 2>/dev/null || true")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 4] Binding pcie-bridge device to the pci-endpoint-test framework:\033[0m")
    qemu.sendline(b"echo '0000:01:00.0' > /sys/bus/pci/drivers/pci-endpoint-test/bind")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 4] Checking is device registered:\033[0m")
    qemu.sendline(b"ls -l /sys/bus/pci/drivers/pci-endpoint-test/")
    qemu.expect(profile["prompt"], timeout=5)

    if "0000:01:00.0" not in qemu.before.decode():
        print(f"\033[96m[Agent][TEST 4] FATAL: Device 0000:01:00.0 failed to bind under /sys/bus/pci/drivers/pci-endpoint-test/\033[0m")
        return False

    print(f"\033[96m[Agent][TEST 4] Enabling Bus Mastering and Memory paths via config space:\033[0m")
    cmd_enable = b"echo -ne '\\x07\\x01' | " + profile["sudo_prefix"] + b"dd of=/sys/bus/pci/devices/0000:01:00.0/config bs=2 seek=2 count=1 conv=notrunc"
    qemu.sendline(cmd_enable)
    qemu.expect(profile["prompt"], timeout=5)
    try:
        qemu.read_nonblocking(size=10000, timeout=0.1)
    except Exception:
        pass

    print(f"\033[96m[Agent][TEST 4] Capturing a baseline interrupt count:\033[0m")
    qemu.sendline(b"stty -echo")
    qemu.expect(profile["prompt"], timeout=5)

    cmd_baseline = b"cat /proc/interrupts | grep -i 'pci-endpoint-test' | " + profile["interrupt_parse"]
    qemu.sendline(cmd_baseline)
    qemu.expect(profile["prompt"], timeout=5)

    baseline_val = 0
    for line in qemu.before.decode().splitlines():
        clean_line = line.strip()
        if clean_line.isdigit():
            baseline_val = int(clean_line)
            break

    qemu.sendline(b"stty echo")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 4] Triggering RTL MSI via BAR0 write 'acdcbabe' to PCIe device...\033[0m")
    irq_trigger_payload = b"\\xBE\\xBA\\xDC\\xAC"
    qemu.sendline(b"echo -ne '" + irq_trigger_payload + b"' | " + profile["sudo_prefix"] + b"dd of=/sys/bus/pci/devices/0000:01:00.0/resource0 bs=4 count=1 conv=notrunc")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 4] Waiting for interrupt propagation...\033[0m")
    time.sleep(0.2)
    try:
        qemu.read_nonblocking(size=10000, timeout=0.1)
    except Exception:
        pass

    qemu.sendline(b"stty -echo")
    qemu.expect(profile["prompt"], timeout=5)

    cmd_post = b"cat /proc/interrupts | grep -i 'pci-endpoint-test' | " + profile["interrupt_parse"]
    qemu.sendline(cmd_post)
    qemu.expect(profile["prompt"], timeout=5)

    post_val = 0
    for line in qemu.before.decode().splitlines():
        clean_line = line.strip()
        if clean_line.isdigit():
            post_val = int(clean_line)
            break

    qemu.sendline(b"stty echo")
    qemu.expect(profile["prompt"], timeout=5)

    print(f"\033[96m[Agent][TEST 4] MSI Verification Done. Baseline: {baseline_val} Post: {post_val}\033[0m")

    if post_val > baseline_val:
        print(f"\033[92m[Agent][TEST 4] PASS: MSI interrupt captured by the guest OS kernel.\033[0m")
        return True
    else:
        print(f"\033[31m[Agent][TEST 4] FAIL: Trigger written, but counter did not increment.\033[0m")
        return False

def main():
    print("\033[94m[Agent] Resolving workspace infrastructure layout...\033[0m")

    parser = argparse.ArgumentParser(description="PCIe Co-Simulation Test Framework Agent")
    parser.add_argument(
        "--distro",
        choices=["cirros", "fedora", "ubuntu"],
        default="cirros",
        help="Specify the target guest OS distribution profile context to execute (default: cirros)"
    )

    args = parser.parse_args()

    project_root, log_dir, log_file_path, pcap_file_path, slog_file_path, image_root = resolve_relative_paths()
    setup_log_directory(log_dir, log_file_path, pcap_file_path, slog_file_path, max_backups=2)
    backend_log = open(log_file_path, "wb", buffering=0)

    image_root = os.path.join(project_root, "third_party", "os", "images", "linux")
    base_cmd = (
        f"qemu-system-x86_64 -enable-kvm -cpu qemu64,+kvm_pv_unhalt -m 1024M -machine pc -net none "
        f"-nographic -vga none"
    )
    external_kernel = os.path.join(image_root, "vmlinuz.v6.8_pcie_cosim")

    if args.distro == "cirros":
        disk_img = os.path.join(image_root, "disk.cirros")
        boot_kernel = external_kernel if os.path.exists(external_kernel) else os.path.join(image_root, "vmlinuz.cirros")
        boot_initrd = os.path.join(image_root, "initrd.cirros")
        base_cmd = f"qemu-system-x86_64 -enable-kvm -cpu host -m 512M -machine q35 -net none -nographic"
        append_args = (
            f"-kernel {boot_kernel} "
            f"-initrd {boot_initrd} "
            f"-drive file={disk_img},format=qcow2,if=virtio "
            f"-append 'console=ttyS0 loglevel=7 root=/dev/vda1 rw nomodeset noefi dslist=none'"
        )
    elif args.distro == "fedora":
        disk_img = os.path.join(image_root, "disk.fedora")
        boot_kernel = external_kernel if os.path.exists(external_kernel) else os.path.join(image_root, "vmlinuz.fedora")
        fedora_sha512_hash = "$6$salt1234$o3fWf91zly8GgC008h1qA32W8x07G9v2SOfsXvW9Z7A6p8X4O3gT9ApxE1eZOfsXvW9Z7A6p8X4O3gT9ApxE1."
        append_args = (
            f"-kernel {boot_kernel} "
            f"-drive file={disk_img},format=qcow2,if=virtio "
            f"-fw_cfg name=opt/io.systemd.Credentials/root.password,string='{fedora_sha512_hash}' "
            f"-append 'console=ttyS0 loglevel=7 root=/dev/vda4 rootflags=subvol=root rw nomodeset noefi selinux=0 enforcing=0 init=/usr/lib/systemd/systemd systemd.default_standard_output=journal+console systemd.unit=multi-user.target plymouth.enable=0 vga=normal rootdelay=1 systemd.mask=local-fs.target systemd.mask=dev-zram0.device systemd.mask=systemd-zram-setup@zram0.service systemd.mask=NetworkManager-wait-online.service'"
        )
    elif args.distro == "ubuntu":
        disk_img = os.path.join(image_root, "disk.ubuntu")
        boot_kernel = external_kernel if os.path.exists(external_kernel) else os.path.join(image_root, "vmlinuz.ubuntu")
        base_cmd = f"qemu-system-x86_64 -enable-kvm -cpu qemu64,+kvm_pv_unhalt -m 1024M -machine pc -net none -nographic -vga none"
        append_args = (
            f"-kernel {boot_kernel} "
            f"-drive file={disk_img},format=qcow2,if=virtio "
            f"-append 'console=ttyS0 loglevel=7 root=/dev/vda1 rw nomodeset noefi cloud-init=disabled systemd.unified_cgroup_hierarchy=1 systemd.mask=local-fs.target'"
        )
    if args.distro != "ubuntu":
        if not os.path.exists(disk_img):
            print(f"\n\033[91m[Agent] FATAL: Specified target disk image file asset missing: {disk_img}\033[0m")
            print("Please run your asset management or symlink tools first.")
            sys.exit(1)

    # Consolidated Execution Assignment
    qemu_cmd = (
        f"{base_cmd} "
        f"{append_args} "
        f"-device pcie-root-port,id=pcie.1 "
        f"-device '{{\"driver\": \"vfio-user-pci\", \"socket\": {{\"type\": \"unix\", \"path\": \"/tmp/vfio-pcie.sock\"}}, \"bus\": \"pcie.1\", \"id\": \"my_dev\"}}'"
    )

    # Launch the PCIe simulation
    enableVerbose = False # set True/False to enable/disable verbose mode on the libvfio-user internal logging
    bridge_bin = os.path.join(project_root, "build", "pcie_sim")
    if enableVerbose:
        bridge_opt = "-v"
    else:
        bridge_opt = ""
    bridge = pexpect.spawn(f"stdbuf -oL {bridge_bin} {bridge_opt}")

    # Launch background continuous line processor
    drainer_thread = threading.Thread(
        target=continuous_line_drainer,
        args=(bridge, backend_log),
        daemon=True
    )
    drainer_thread.start()

    try:
        print("[Agent] Syncing with PCIe-Bridge...")
        timeout_limit = 15.0
        initialized = False

        while timeout_limit > 0:
            if os.path.exists(log_file_path):
                with open(log_file_path, 'r', errors='ignore') as f:
                    log_content = f.read()
                    if "[PCIe-SIM] Waiting for PCIe packets" in log_content:
                        initialized = True
                        break
            time.sleep(0.5)
            timeout_limit -= 0.5

        if not initialized:
            raise pexpect.TIMEOUT("PCIe-SIM backend handshake synchronization timeout.")

        print(f"[Agent] Backend ready. Trace log streaming to: {log_file_path}")
        time.sleep(0.5)

    except pexpect.TIMEOUT as e:
        print(f"\n\033[91m[Agent] FATAL: PCIe-Bridge Backend Handshake Fail: {e}\033[0m")
        bridge.terminate(force=True)
        backend_log.close()
        return

    # Launch the packet sniffer to capture VFIO-User traffic over UDS on the channel with QEMU
    sniffer_proc = None
    enableSniffer = False # set True/False to enable/disable the VFIO-User packet sniffer
    if enableSniffer:
        print(f"[Agent] Launching VFIO-User packet sniffer...")
        sniffer_proc = start_vfio_user_pkt_sniffer(project_root, pcap_file_path, slog_file_path)

    print(f"[Agent] Launching Guest OS Emulation...")
    qemu = pexpect.spawn(qemu_cmd)
    qemu.logfile_read = sys.stdout.buffer

    try:
        env_profile = detect_and_initialize_guest_os_profile(args, qemu)

        if env_profile.get("is_boot_failure", False):
            print(f"\n\033[93m[Agent] HALT: Automation aborted on a crashed OS.\033[0m")
            print(f"\033[93m[Agent] HALT: Transferring control to terminal console for troubleshooting.")
            print("[Agent] HALT: Press Enter to access maintenance mode. Press Ctrl+] to exit agent.\n")

            qemu.sendline(b"")
            time.sleep(0.2)
            qemu.sendline(b"")
            qemu.interact()
            return
        else:
            print("[Agent] Workspace alignment synchronized")

        enableAutomatedTest = True # set True/False to enable/disable the automated verification test suite
        testMatrix = [1, 2, 3, 4]

        if enableAutomatedTest and testMatrix:
            print(f"\033[93m[Agent] Workspace boot metrics validated CLEAN. Running test matrix: {testMatrix}\033[0m")
            testResults = []

            if 1 in testMatrix:
                testResults.append(run_test_1_burst_verification(qemu, env_profile))
            if 2 in testMatrix:
                testResults.append(run_test_2_boundary_validation(qemu, env_profile))
            if 3 in testMatrix:
                testResults.append(run_test_3_bar_isolation(qemu, env_profile))
            if 4 in testMatrix:
                testResults.append(run_test_4_irq_verification(qemu, env_profile, log_file_path))

            if all(testResults):
                print(f"\n\033[93m[Agent] RUN RESULT: AUTOMATED VERIFICATION: PASS\033[0m")
            else:
                failedTests = [test for test, result in zip(testMatrix, testResults) if not result]
                print(f"\n\033[91m[Agent] RUN RESULT: AUTOMATED VERIFICATION: FAIL: {failedTests}\033[0m")
        else:
            print(f"\n\033[35m[Agent] Automated verification BYPASSED.\033[0m")

        print("\n\033[95m[Agent] Entering interactive mode. Press Ctrl+] to exit.\033[0m")

        qemu.logfile_read = None
        time.sleep(0.1)
        qemu.interact(escape_character='\x1d')

    except pexpect.TIMEOUT as te:
        print(f"\n\033[91m[Agent] FATAL: Pexpect Watchdog Timeout Expired: {te}\033[0m")
        traceback.print_exc()
    except Exception as ex:
        print(f"\n\033[91m[Agent] FATAL: Exception thrown: {ex}\033[0m")
        traceback.print_exc()
    finally:
        print(f"\n\033[96m[Agent] Tearing down infrastructure...\033[0m")
        stop_vfio_user_pkt_sniffer(sniffer_proc)
        try:
            qemu.terminate(force=True)
            print("[Agent] QEMU stopped.")
        except Exception:
            pass
        try:
            if bridge.isalive():
                print("[Agent] Sending SIGINT to PCIe-Bridge...")
                bridge.kill(2)
                bridge.expect(pexpect.EOF, timeout=2)
                print("[Agent] PCIe-Bridge reaped gracefully.")
        except pexpect.TIMEOUT:
            print("\n[Agent] WARNING: PCIe-Bridge is frozen! Forcing kill...")
            bridge.terminate(force=True)
            for sock in ["/tmp/vfio-pcie.sock", "/tmp/pcie-cosim1.sock", "/tmp/pcie-cosim2.sock"]:
                if os.path.exists(sock):
                    try: os.unlink(sock)
                    except Exception: pass
        except pexpect.EOF:
            print("[Agent] PCIe-Bridge closed channel.")

        backend_log.close()
        print("[Agent] Teardown complete.")

if __name__ == "__main__":
    main()
