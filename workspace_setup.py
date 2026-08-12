#!/usr/bin/env python3

from getpass import getpass
import glob
import grp
import json
import os
import re
import shutil
import subprocess
import sys

def print_workspace_manifest(blueprint):
    """Parse and print the structural components declared in the blueprint."""
    print("This utility checks and aligns the following components:")

    repos = blueprint.get("base_requirements", {}).get("repos", [])
    if repos:
        print("[📦 Mandatory Repositories]:")
        for repo in repos:
            dir = repo.get("dir") or repo["path"].split("/")[-1]
            branch_info = f" (branch: {repo['branch']})" if repo.get("branch") else ""
            print(f"  • {dir} -> {repo['path']}{branch_info}")

    scopes = blueprint.get("scopes", [])
    if scopes:
        print("[🌲 Optional Scopes & Features]:")
        for scope in scopes:
            print(f"  • {scope['name']} ({scope['id']})")
            if scope.get("packages"):
                print(f"    Packages: {', '.join(scope['packages'])}")
            if scope.get("repos"):
                scope_repos = [r["dir"] if r.get("dir") else r["path"].split("/")[-1] for r in scope["repos"]]
                print(f"    Repos: {', '.join(scope_repos)}")

def install_system_packages(package_list):
    """Update repositories and install a list of system packages."""
    if not package_list:
        return True
    try:
        print(f"[+] Processing system installation for: {', '.join(package_list)}")
        subprocess.run(["sudo", "apt", "update"], check=True)
        env = os.environ.copy()
        env["DEBIAN_FRONTEND"] = "noninteractive"
        subprocess.run(["sudo", "-E", "apt", "install", "-y"] + package_list, env=env, check=True)
        return True
    except subprocess.CalledProcessError as e:
        print(f"[⚠️] Native package manager reported a failure: {e}")
        return False

try:
    from InquirerPy import inquirer
    from InquirerPy.base.control import Choice
except ModuleNotFoundError:

    if install_system_packages(["python3-inquirerpy"]):
        print("[✓ Success] python3-inquirerpy installed.\n")
        os.execv(sys.executable, [sys.executable] + sys.argv)
    else:
        print("[⚠️] Please execute manually: sudo apt install python3-inquirerpy")
        sys.exit(1)

def load_json(path):
    if not os.path.exists(path):
        return None

    with open(path, "r") as f:
        try:
            return json.load(f)
        except json.JSONDecodeError as e:
            print(f"[⚠️] ERROR: Failed to parse '{path}'")
            print(f"[⚠️] {path}:{e.lineno}:{e.colno}: {e.msg}")
            sys.exit(1)

def is_package_installed(package_name):
    try:
        result = subprocess.run(
            ["dpkg-query", "-W", "-f=${Status}", package_name],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        status = result.stdout.strip()
        if "install ok installed" in status:
            return True

        return False
    except FileNotFoundError:
        print("[⚠️] Error: 'dpkg-query' not found")
        return False

def get_qemu_dirs():
    """Scan the system paths and return a list of directories containing QEMU binaries."""
    qemu_dirs = []
    if os.path.exists("/usr/bin/qemu-system-x86_64"):
        qemu_dirs.append("/usr/bin")
    if os.path.exists("/usr/local/bin/qemu-system-x86_64"):
        qemu_dirs.append("/usr/local/bin")
    return qemu_dirs

def remove_qemu_distro(repo_path):
    """Remove QEMU distro from the host."""
    build_dir = os.path.join(repo_path, "build")
    install_log = os.path.join(build_dir, "meson-logs", "install-log.txt")

    if os.path.exists(install_log):
        try:
            subprocess.run(["sudo", "ninja", "uninstall"], cwd=build_dir, check=True)
        except subprocess.CalledProcessError as e:
            print("[⚠️] Warning: QEMU ninja uninstaller failed: {e}")

    binaries = [
        "qemu-system-i386",
        "qemu-system-x86_64",
        "qemu-system-arm",
        "qemu-system-aarch64",
        "qemu-system-riscv32",
        "qemu-system-riscv64",
        "qemu-ga",
        "qemu-img",
        "qemu-io",
        "qemu-nbd",
        "qemu-storage-daemon",
        "qemu-edid",
        "qemu-pr-helper",
        "qemu-vmsr-helper"
    ]
    qemu_dirs = get_qemu_dirs()
    for dir in qemu_dirs:
        for item in binaries:
            if os.path.exists(os.path.join(dir, item)):
                try:
                    subprocess.run(["sudo", "rm", "-f", os.path.join(dir, item)], check=True)
                except subprocess.CalledProcessError:
                    pass
    files = [
        "/usr/libexec/qemu-bridge-helper",
        "/usr/include/qemu-plugin.h",
        "/usr/share/applications/qemu.desktop"
    ]

    for file in files:
        if os.path.exists(file):
            try:
                subprocess.run(["sudo", "rm", "-f", file], check=True)
            except subprocess.CalledProcessError:
                pass

    directories = [
        "/usr/share/qemu/firmware",
        "/usr/share/qemu/dtb",
        "/usr/share/qemu/keymaps",
        "/usr/share/qemu"
    ]

    for dir in directories:
        if os.path.exists(dir):
            try:
                subprocess.run(["sudo", "rm", "-rf", dir], check=True)
            except subprocess.CalledProcessError:
                pass

    icons = glob.glob("/usr/share/icons/hicolor/*/apps/qemu.*")
    for icon in icons:
        try:
            subprocess.run(["sudo", "rm", "-f", icon], check=True)
        except subprocess.CalledProcessError:
            pass
    print("[✓ Success] QEMU distro removed from host system")

def get_qemu_system_x86_64_version():
    """Retrieve the installed qemu-system-x86_64 version."""
    try:
        qemu_ver = subprocess.run(
            ["qemu-system-x86_64", "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True
        )
        first_line = qemu_ver.stdout.splitlines()[0]
        version_match = re.search(r"version\s+([0-9]+\.[0-9]+\.[0-9]+)", first_line)

        return version_match.group(1) if version_match else None
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None

def is_qemu_vfio_user_supported(tag = None):
    """Verify if the local qemu-system-x86_64 supports vfio-user-pci and matches the required version tag."""
    if not shutil.which("qemu-system-x86_64"):
        print("[⚠️] qemu-system-x86_64 not found")
        return False
    try:
        qemu_device_help = subprocess.Popen(
            ["qemu-system-x86_64", "-device", "help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )

        stdout_stream, _ = qemu_device_help.communicate()

        if "vfio-user-pci" not in stdout_stream:
            print("[⚠️] vfio-user-pci not supported in qemu-system-x86_64")
            return False

        if tag:
            qemu_ver = get_qemu_system_x86_64_version()
            if not qemu_ver:
                return False

            expected_ver = tag.lstrip('v')
            if qemu_ver != expected_ver:
                print(f"[⚠️] QEMU vfio-user-pci version mismatch! Expected: {expected_ver} | Found: {qemu_ver}")
            return qemu_ver == expected_ver

        return True
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False

def remove_verilator_distro(repo_path):
    """Remove Verilator distro from the host."""
    build_dir = os.path.join(repo_path, "build")

    if os.path.exists(build_dir):
        install_manifest = os.path.join(build_dir, "install_manifest.txt")
        if os.path.exists(install_manifest):
            try:
                with open(install_manifest, "r") as f:
                    files = [line.strip() for line in f if line.strip()]
                if files:
                    subprocess.run(
                        ["sudo", "rm", "-f"] + files,
                        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                    )
            except subprocess.CalledProcessError as e:
                print(f"[⚠️] Warning: Verilator manifest uninstaller failed: {e}")
                try:
                    subprocess.run(
                        ["sudo", "ninja", "uninstall"],
                        cwd=build_dir, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                    )
                except subprocess.CalledProcessError as e:
                    print(f"[⚠️] Warning: Verilator ninja uninstaller failed: {e}")

    binaries = glob.glob("/usr/local/bin/verilator*")
    if binaries:
        try:
            subprocess.run(
                ["sudo", "rm", "-f"] + binaries,
                shell=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
        except subprocess.CalledProcessError:
            pass

    directories = [
        "/usr/local/include/fstcpp",
        "/usr/local/include/vltstd",
        "/usr/local/examples"
    ]
    for dir in directories:
        if os.path.exists(dir):
            try:
                subprocess.run(["sudo", "rm", "-rf", dir], check=True)
            except subprocess.CalledProcessError:
                pass

    files = [
        "/usr/local/verilator-config.cmake",
        "/usr/local/verilator-config-version.cmake",
    ]
    for file in files:
        if os.path.exists(file):
            try:
                subprocess.run(["sudo", "rm", "-f", file], check=True)
            except subprocess.CalledProcessError:
                pass

    includes = glob.glob("/usr/local/include/verilated*")
    if includes:
        try:
            subprocess.run(
                ["sudo", "rm", "-f"] + includes,
                shell=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
        except subprocess.CalledProcessError:
            pass

    print("[✓ Success] Verilator distro removed from host system")

def check_upstream_update(repo_path, tag):
    """Query remote to notify users if a newer version exists."""
    if not tag:
        return

    try:
        res = subprocess.run(
            ["git", "ls-remote", "--tags", "origin"],
            cwd=repo_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True
        )
        tags = set()
        semver_regex = re.compile(r"^v\d+(?:\.\d+){0,2}$")
        for line in res.stdout.splitlines():
            if "refs/tags/" in line:
                tag_name = line.split("refs/tags/")[-1].split("^{}")[0].strip()
                if semver_regex.match(tag_name):
                    tags.add(tag_name)
        if tags:
            def semver_key(t):
                parts = [int(x) for x in t.lstrip('v').split('.')]
                while len(parts) < 3:
                    parts.append(0)
                return parts

            sorted_tags = sorted(tags, key=semver_key)
            latest_tag = sorted_tags[-1]

            if semver_key(latest_tag) > semver_key(tag):
                repo_name = repo_path.split("/")[-1]
                print(f"[🔔 {repo_name} upstream update available] Configured: {tag} | Latest: {latest_tag}")
    except Exception:
        pass

def configure_vfio_user_dissector(project_root_dir):
    """Deploy the vfio_user.lua Wireshark packet dissector plugin."""
    source_dir = os.path.join(project_root_dir, "tools", "net")
    source_lua = os.path.join(source_dir, "vfio_user.lua")

    target_plugin_dir = os.path.expanduser("~/.config/wireshark/plugins")
    target_lua = os.path.join(target_plugin_dir, "vfio_user.lua")

    if os.path.exists(target_lua):
        return

    if not os.path.exists(source_lua):
        print(f"[⚠️] Warning: Source plugin file missing from repository asset tree at: {source_lua}")
        print("[⚠️] Skipping vfio-user dissector deployment.")
        return

    try:
        os.makedirs(target_plugin_dir, exist_ok=True)
        shutil.copy2(source_lua, target_lua)
        print(f"[✓ Success] Wireshark 'vfio-user' packet dissector activated at {target_lua}!")
    except Exception as e:
        print(f"[⚠️] Failed to mirror plugin binary script asset: {e}")

def verify_project_symlinks(project_root_dir, tools_dir, soc_dir, os_dir, selected_ids):
    """Generate the third_party relative project directory symbolic links."""
    links_to_verify = [
        {
            "target": os.path.join(soc_dir, "openPCIE"),
            "symlink": os.path.join(project_root_dir, "third_party", "hw", "openPCIE"),
            "label": "openPCIE -> third_party/hw/openPCIE"
        },
        {
            "target": os.path.join(tools_dir, "libvfio-user"),
            "symlink": os.path.join(project_root_dir, "third_party", "lib", "libvfio-user"),
            "label": "libvfio-user -> third_party/lib/libvfio-user"
        }
    ]
    if "linux" in selected_ids:
        links_to_verify.append({
            "target": os.path.join(os_dir, "linux"),
            "symlink": os.path.join(project_root_dir, "third_party", "os", "linux"),
            "label": "linux -> third_party/os/linux"
        })

    for link in links_to_verify:
        target_path = link["target"]
        symlink_path = link["symlink"]

        os.makedirs(os.path.dirname(symlink_path), exist_ok=True)

        if os.path.islink(symlink_path):
            print(f"[✓ Skipping] Symbolic link for {link['label']} already set")
        else:
            if os.path.exists(target_path):
                print(f"[!] Setting up symbolic link for: {link['label']}")
                os.symlink(target_path, symlink_path)
            else:
                print(f"[⚠️] Warning: Cannot generate symbolic link. Target directory missing at: {target_path}")

def apply_patch(repo_path, branch, patch_path):
    """Safely and atomically applies a target patch file to a repository tree."""
    if not patch_path:
        return True

    if not os.path.exists(patch_path):
        print(f"[⚠️] Warning: Patch file missing on host filesystem: {patch_path}")
        return False

    patch_filename = os.path.basename(patch_path)

    try:
        dry_run = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=repo_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )

        if dry_run.returncode == 0:
            subprocess.run(["git", "apply", patch_path], cwd=repo_path, check=True)
            subprocess.run(["git", "add", "."], cwd=repo_path, check=True)
            subprocess.run(
                ["git", "commit", "-m", f"feat: add {patch_filename}"],
                cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            print(f"[✓ Patch] '{patch_filename}' merged and committed to branch {branch}")
            return True
        else:
            print(f"[⚠️] Warning: Dropped patch conflict: {dry_run.stderr.decode().strip()}")
            return False

    except subprocess.CalledProcessError as e:
        print(f"[⚠️] Failed to execute patch orchestration task: {e}")
        return False

def align_repo_state(repo_path, tag=None, branch=None, patch_path=None):
    """Align git workspace targets using low-level plumbing checks, avoiding blind checkouts."""
    if not tag and not branch:
        return
    try:
        status_check = subprocess.run(["git", "status", "--porcelain"], cwd=repo_path, stdout=subprocess.PIPE, text=True)
        is_dirty = bool(status_check.stdout.strip())

        def run_checkout(branch):
            if is_dirty:
                subprocess.run(["git", "stash"], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["git", "checkout", branch], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if is_dirty:
                subprocess.run(["git", "stash", "pop"], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        if branch and not tag:
            current_branch = subprocess.run(["git", "branch", "--show-current"], cwd=repo_path, stdout=subprocess.PIPE, text=True).stdout.strip()
            if current_branch == branch:
                print(f"[✓ Git] '{repo_path}' aligned on branch '{branch}'")
                return
            print(f"[🌲 Git] Switching '{repo_path}' to branch '{branch}'")
            run_checkout(branch)
            return

        if tag and not branch:
            subprocess.run(
                ["git", "fetch", "--depth=1", "origin", f"refs/tags/{tag}:refs/tags/{tag}"],
                cwd=repo_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True
            )

            try:
                target_tag_hash = subprocess.run(["git", "rev-parse", f"refs/tags/{tag}^{{commit}}"], cwd=repo_path, stdout=subprocess.PIPE, text=True, check=True).stdout.strip()
                current_head_hash = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo_path, stdout=subprocess.PIPE, text=True, check=True).stdout.strip()

                if current_head_hash == target_tag_hash:
                    print(f"[✓ Git] '{repo_path}' aligned on tag '{tag}'")
                    return
            except subprocess.CalledProcessError:
                pass

            print(f"[🌲 Git] Switching '{repo_path}' to tag '{tag}'...")
            run_checkout(tag)
            return

        if tag and branch:
            subprocess.run(
                ["git", "fetch", "--depth=1", "origin", f"refs/tags/{tag}:refs/tags/{tag}"],
                cwd=repo_path,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                text=True
            )
            branch_check = subprocess.run(["git", "rev-parse", "--verify", branch], cwd=repo_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            if branch_check.returncode == 0:
                current_branch = subprocess.run(["git", "branch", "--show-current"], cwd=repo_path, stdout=subprocess.PIPE, text=True).stdout.strip()

                if current_branch == branch:
                    print(f"[✓ Git] '{repo_path}' aligned on branch '{branch}' at tag '{tag}'")
                    return
                else:
                    print(f"[🌲 Git] Switching '{repo_path}' to branch '{branch}'")
                    run_checkout(branch)
                    return

            print(f"[🌲 Git] Initializing '{repo_path}' branch '{branch}' from tag reference '{tag}'...")
            if is_dirty:
                subprocess.run(["git", "stash"], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            try:
                subprocess.run(["git", "checkout", "-b", branch, f"refs/tags/{tag}"], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except subprocess.CalledProcessError as e:
                print(f"[⚠️] Error: Failed to checkout '{branch}': {e}")
                if is_dirty:
                    subprocess.run(["git", "stash", "pop"], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return

            if patch_path:
                print(f"[🌲 Git] Patching '{repo_path}' branch '{branch}' from patch {patch_path}'...")
                apply_patch(repo_path, branch, patch_path)

            if is_dirty:
                subprocess.run(["git", "stash", "pop"], cwd=repo_path, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return

    except subprocess.CalledProcessError as e:
        print(f"[⚠️] Warning: Failed git alignment pipeline orchestration task: {e}")

def build_repo(repo_name, repo_path, tag = None):
    """Execute target compilation recipes based on the repository identity, skipping if already installed."""
    if repo_name in ["linux", "openPCIE"]:
        return

    if repo_name == "libvfio-user":
        if os.path.exists(os.path.join(repo_path, "build", "lib", "libvfio-user.so")):
            print(f"[✓ Skipping] 'libvfio-user' already built in: {repo_path}/build")
            return

    elif repo_name == "verilator":
        check_upstream_update(repo_path, tag)
        verilator_bin = subprocess.run(["which", "verilator"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if verilator_bin.returncode == 0:
            verilator_ver = subprocess.run(["verilator", "--version"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if tag in verilator_ver.stdout:
                print(f"[✓ Skipping] Verilator version {tag} already built and installed")
                return
            else:
                print(f"[!] Current verilator version {verilator_ver.stdout.strip()}")
                print(f"[!] Initiating rebuild to align with version '{tag}'...")
                remove_verilator_distro(repo_path)
        else:
            print("[!] Verilator binary not found. Initiating build recipe...")

    elif repo_name == "qemu":
        check_upstream_update(repo_path, tag)
        if is_qemu_vfio_user_supported(tag):
            print(f"[✓ Skipping] 'qemu-system-x86_64' version {tag} with 'vfio-user-pci' support already built and installed")
            return
        else:
            if get_qemu_dirs():
                remove_qemu_distro(repo_path)
            print("[!] QEMU status: Initiating build recipe...")
    else:
        return

    print(f"[🔧 Build] Starting source compilation for: {repo_name}")
    build_dir = os.path.join(repo_path, "build")

    try:
        if repo_name == "libvfio-user":
            if os.path.exists(build_dir):
                shutil.rmtree(build_dir)
            print("→ Running: 'libvfio-user' meson setup build && meson compile -C build")
            subprocess.run(["meson", "setup", "build"], cwd=repo_path, check=True)
            subprocess.run(["meson", "compile", "-C", "build"], cwd=repo_path, check=True)

        elif repo_name == "verilator":
            env = os.environ.copy()
            if "VERILATOR_ROOT" in env:
                del env["VERILATOR_ROOT"]
            if os.path.exists(build_dir):
                subprocess.run(["sudo", "rm", "-rf", build_dir], check=True)
            os.makedirs(build_dir)

            print("→ Running Verilator configure pipeline...")
            config_cmd = [
                "cmake",
                "-G", "Ninja", "-Wno-dev",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_INSTALL_PREFIX=/usr/local",
                ".."
            ]
            try:
                subprocess.run(config_cmd, cwd=build_dir, check=True)
            except subprocess.CalledProcessError:
                print("[!] Verilator configuration failed. Clearing old build structures to try clean fallback...")
                if os.path.exists(build_dir):
                    subprocess.run(["sudo", "rm", "-rf", build_dir], check=True)
                os.makedirs(build_dir)
                subprocess.run(config_cmd, cwd=build_dir, check=True)

            cpu_jobs = str(os.cpu_count() or 2)
            print(f"→ Compiling Verilator with Ninja using {cpu_jobs} execution jobs...")

            try:
                subprocess.run(["ninja", "-j", cpu_jobs], cwd=build_dir, check=True)
                subprocess.run(["sudo", "cmake", "--install", "."], cwd=build_dir, check=True)
                subprocess.run(["hash", "-r"], shell=True)

                verilator_ver = subprocess.run(["verilator", "--version"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                if tag in verilator_ver.stdout:
                    print(f"[✓] Verified Verilator version {tag} installation")
                else:
                    print(f"[⚠️] Warning: Verilator version mismatch! Expected: {tag} | Found: {verilator_ver.stdout.strip()}")
            except subprocess.CalledProcessError as e:
                print(f"[⚠️] Error: Failed to compile Verilator: {e}")

        elif repo_name == "qemu":

            if os.path.exists(build_dir):
                subprocess.run(["sudo", "rm", "-rf", build_dir], check=True)
            os.makedirs(build_dir)

            print("→ Running QEMU configure pipeline...")
            config_cmd = [
                "../configure",
                "--prefix=/usr",
                "--enable-kvm",
                "--enable-slirp",
                "--disable-user",
                "--extra-cflags=-g -O2",
                "--target-list="
                "i386-softmmu,x86_64-softmmu,"      # x86_32 & x86_64 bare-metal emulation
                "arm-softmmu,aarch64-softmmu,"      # arm32 & arm64 bare-metal emulation
                "riscv32-softmmu,riscv64-softmmu"   # riscv32 & riscv64 bare-metal emulation
            ]

            try:
                subprocess.run(config_cmd, cwd=build_dir, check=True)
            except subprocess.CalledProcessError:
                print("[!] QEMU configuration failed. Clearing old build structures to try clean fallback...")
                if os.path.exists(build_dir):
                    subprocess.run(["sudo", "rm", "-rf", build_dir], check=True)
                os.makedirs(build_dir)
                subprocess.run(config_cmd, cwd=build_dir, check=True)

            cpu_jobs = str(os.cpu_count() or 2)
            print(f"→ Compiling QEMU with Ninja using {cpu_jobs} execution jobs...")

            try:
                subprocess.run(["ninja", "-j", cpu_jobs], cwd=build_dir, check=True)
                subprocess.run(["sudo", "-E", "ninja", "install"], cwd=build_dir, check=True)
                subprocess.run(["hash", "-r"], shell=True)

                qemu_ver = get_qemu_system_x86_64_version()
                expected_ver = tag.lstrip('v')
                if qemu_ver == expected_ver:
                    print(f"[✓] Verified QEMU version {expected_ver} installation")
                else:
                    print(f"[⚠️] Warning: QEMU version mismatch! Expected: {expected_ver} | Found: {qemu_ver}")
            except subprocess.CalledProcessError as e:
                print(f"[⚠️] Error: Failed to compile QEMU: {e}")

        print(f"[✓ Success] Finished compiling {repo_name}!")
    except subprocess.CalledProcessError as e:
        print(f"[🗙 Error] Compilation failed for {repo_name}: {e}")

def configure_wireshark_permissions():
    """Applies non-root capture capabilities to dumpcap for Debian/Ubuntu environments."""
    print("Applying Wireshark Non-Root Capture Privileges...")
    local_user = os.environ.get("USER") or os.environ.get("LOGNAME") or getpass.getuser()
    try:
        try:
            wireshark_group = grp.getgrnam("wireshark")
            user_in_wireshark = local_user in wireshark_group.gr_mem
        except KeyError:
            wireshark_group = None
            user_in_wireshark = False
        try:
            sudo_group = grp.getgrnam("sudo")
            user_in_sudo = local_user in sudo_group.gr_mem
        except KeyError:
            user_in_sudo = False

        user_in_group = user_in_wireshark or user_in_sudo
        capabilities_set = False
        if os.path.exists("/usr/bin/dumpcap"):
            cap_check = subprocess.run(
                ["getcap", "/usr/bin/dumpcap"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if ("cap_net_admin" in cap_check.stdout
                and "cap_net_raw" in cap_check.stdout
            ):
                capabilities_set = True

        if user_in_group and capabilities_set:
            print("[✓ Skipping] Wireshark already configured", flush=True)
            return

        if not wireshark_group:
            print("[→] Creating 'wireshark' system group...")
            subprocess.run(["sudo", "groupadd", "-f", "wireshark"], check=True)
        if not user_in_wireshark:
            print(f"[→] Registering current user to 'wireshark' group...")
            subprocess.run(["sudo", "usermod", "-aG", "wireshark", local_user], check=True)

        if os.path.exists("/usr/bin/dumpcap"):
            print("[→] Applying CAP_NET_RAW and CAP_NET_ADMIN capabilities to dumpcap...")
            subprocess.run(["sudo", "setcap", "CAP_NET_RAW+eip CAP_NET_ADMIN+eip", "/usr/bin/dumpcap"], check=True)
            print("[→] Ensuring executable access on dumpcap...")
            subprocess.run(["sudo", "chmod", "+x", "/usr/bin/dumpcap"], check=True)
            print("[✓ Success] Wireshark capture permissions configured! Please log out and back in to apply group changes.")
        else:
            print("[⚠️] Warning: /usr/bin/dumpcap not found. Wireshark package might have failed to install.")
    except subprocess.CalledProcessError as e:
        print(f"[⚠️] Failed to configure Wireshark capabilities: {e}")

def configure_x11_resources():
    """Append the Xresources merge string to the user's .bashrc file."""
    bashrc_path = os.path.expanduser("~/.bashrc")
    target_line = "xrdb -merge ~/.Xresources\n"

    if os.path.exists(bashrc_path):
        with open(bashrc_path, "r") as f:
            content = f.readlines()
        if any("xrdb -merge ~/.Xresources" in line for line in content):
            print("[✓ Skipping] '.bashrc' already contains Xresources initialization configuration")
            return

    with open(bashrc_path, "a") as f:
        f.write(f"{target_line}")
    print("[✓ Success] X11 startup rules appended to configuration profile")

def main():
    project_root_dir = os.path.dirname(os.path.abspath(__file__))

    blueprint = load_json("prerequisites_config.json")
    if not blueprint:
        print("[⚠️] Error: 'prerequisites_config.json' missing from root directory.")
        sys.exit(1)

    print_workspace_manifest(blueprint)

    path_defaults = blueprint.get("default_paths", {})
    default_tools_dir = path_defaults.get("clone_tools_dir", "~/tools")
    default_os_dir = path_defaults.get("clone_os_dir", "~/kernel")
    default_soc_dir = path_defaults.get("clone_soc_dir", "~/soc")

    resolved_tools_dir = os.path.abspath(os.path.expanduser(default_tools_dir))
    resolved_os_dir = os.path.abspath(os.path.expanduser(default_os_dir))
    resolved_soc_dir = os.path.abspath(os.path.expanduser(default_soc_dir))

    previous_state = load_json("configs.json")
    past_scopes = []
    past_tools_dir = default_tools_dir
    past_os_dir = default_os_dir
    past_soc_dir = default_soc_dir

    if previous_state:
        print("[+] Existing configurations from 'configs.json' are marked for the default setup")
        past_scopes = previous_state.get("meta", {}).get("selected_scopes", [])
        past_tools_dir = previous_state.get("meta", {}).get("clone_tools_directory", default_tools_dir)
        past_os_dir = previous_state.get("meta", {}).get("clone_os_directory", default_os_dir)
        past_soc_dir = previous_state.get("meta", {}).get("clone_soc_directory", default_soc_dir)

        resolved_tools_dir = os.path.abspath(os.path.expanduser(past_tools_dir))
        resolved_os_dir = os.path.abspath(os.path.expanduser(past_os_dir))
        resolved_soc_dir = os.path.abspath(os.path.expanduser(past_soc_dir))

    scopes = blueprint.get("scopes", [])
    menu_choices = [Choice(name=s["name"], value=s, enabled=(s["id"] in past_scopes)) for s in scopes]

    selected_scopes = inquirer.checkbox(
        message="Select installation options:\nUse SPACE to select, ARROW KEYS to navigate, and ENTER to confirm selection",
        choices=menu_choices,
    ).execute()

    selected_ids = [scope["id"] for scope in selected_scopes]

    all_active_repos = blueprint.get("base_requirements", {}).get("repos", [])
    for scope in selected_scopes:
        all_active_repos += scope.get("repos", [])

    if any(r.get("type") == "tools" for r in all_active_repos):
        tools_input = inquirer.text(
            message="Specify absolute path to tools repositories:",
            default=past_tools_dir,
        ).execute()
        resolved_tools_dir = os.path.abspath(os.path.expanduser(tools_input.strip() if tools_input.strip() else default_tools_dir))

    if any(r.get("type") == "soc" for r in all_active_repos):
        soc_input = inquirer.text(
            message="Specify absolute path to SoC repositories:",
            default=past_soc_dir,
        ).execute()
        resolved_soc_dir = os.path.abspath(os.path.expanduser(soc_input.strip() if soc_input.strip() else default_soc_dir))

    if "linux" in selected_ids:
        os_input = inquirer.text(
            message="Specify absolute path to operating system repositories:",
            default=past_os_dir,
        ).execute()
        resolved_os_dir = os.path.abspath(os.path.expanduser(os_input.strip() if os_input.strip() else default_os_dir))

    final_packages = set(blueprint.get("base_requirements", {}).get("packages", []))
    final_repos = {}
    repo_ids = []

    def add_repos(repo_list):
        for r_entry in repo_list:
            r_path = r_entry["path"]
            r_id = r_entry["id"]
            r_type = r_entry["type"]
            r_dir = r_entry.get("dir")
            r_tag = r_entry.get("tag")
            r_branch = r_entry.get("branch")
            r_depth = r_entry.get("clone_depth")
            r_url = r_entry.get("upstream_url")
            r_patch = r_entry.get("patch")
            r_sparse = r_entry.get("sparse")

            if r_id and r_id not in selected_ids:
                repo_ids.append(r_id)

            if r_type == "os":
                r_repos_dir = resolved_os_dir
            elif r_type == "soc":
                r_repos_dir = resolved_soc_dir
            elif r_type == "tools":
                r_repos_dir = resolved_tools_dir
            else:
                r_repos_dir = None

            final_repos[r_path] = {
                "id": r_id,
                "dir": r_dir,
                "rdir": r_repos_dir,
                "tag": r_tag,
                "branch": r_branch,
                "depth": r_depth,
                "url": r_url,
                "patch": r_patch,
                "sparse": r_sparse
            }
    add_repos(blueprint.get("base_requirements", {}).get("repos", []))

    for scope in selected_scopes:
        final_packages.update(scope.get("packages", []))
        add_repos(scope.get("repos", []))

    compiled_config = {
        "meta": {
            "clone_tools_directory": resolved_tools_dir,
            "clone_os_directory": resolved_os_dir,
            "clone_soc_directory": resolved_soc_dir,
            "repos": sorted(repo_ids),
            "selected_scopes": selected_ids,
        },
        "packages": sorted(list(final_packages)),
        "repos": [
            {
                "path": r_path,
                "id": r_info["id"],
                "dir": r_info["dir"],
                "rdir": r_info["rdir"],
                "tag": r_info["tag"],
                "branch": r_info["branch"],
                "depth": r_info["depth"],
                "url": r_info["url"],
                "patch": r_info["patch"],
                "sparse": r_info["sparse"]
            }
            for r_path, r_info in sorted(final_repos.items())
        ],
    }

    with open("configs.json", "w") as f:
        json.dump(compiled_config, f, indent=2)

    print("Verifying Local Package System State...")
    packages_to_install = [pkg for pkg in compiled_config["packages"] if not is_package_installed(pkg)]

    if packages_to_install:
        print(f"[+] Missing dependencies detected: {', '.join(packages_to_install)}")
        if not install_system_packages(packages_to_install):
            print("[⚠️] Native Package alignment halted due to execution errors")
        else:
            print("[+] Compilation dependency tools are available")

    print("Processing Repositories & Building From Source...")

    for repo_entry in compiled_config["repos"]:
        repo = repo_entry["path"]
        repo_id = repo_entry["id"]
        repo_dir = repo_entry["dir"]
        repo_root_dir = repo_entry["rdir"]

        if repo_id and repo_id not in (repo_ids + selected_ids):
            continue

        repo_name = repo_dir if repo_dir else repo.split("/")[-1]
        os.makedirs(repo_root_dir, exist_ok=True)
        repo_path = os.path.join(repo_root_dir, repo_name)
        clone_url = repo_entry.get("url") or f"https://github.com/{repo}.git"

        if os.path.exists(repo_path) and os.path.isdir(os.path.join(repo_path, ".git")):
            print(f"[→] Repository '{clone_url}' already downloaded at: {repo_path}")
        else:
            print(f"[→] Downloading: {clone_url} -> {repo_path}")

            clone_cmd = ["git", "clone"]
            repo_depth = repo_entry.get("depth")
            repo_sparse = repo_entry.get("sparse")

            if (repo_sparse):
                clone_cmd += ["--depth", "1", "--filter=blob:none", "--sparse"]
            if repo_depth:
                clone_cmd += ["--depth", str(repo_depth)]

            clone_cmd += [clone_url, repo_path]
            try:
                subprocess.run(clone_cmd, check=True)
            except subprocess.CalledProcessError:
                print(f"[⚠️] Failed to fetch repository: {repo}")
                continue
            if (repo_sparse):
                subprocess.run(["git", "sparse-checkout", "set"] + repo_sparse, cwd=repo_path, check=True)

        tag = repo_entry.get("tag")
        branch = repo_entry.get("branch")
        patch_path = repo_entry.get("patch")
        if patch_path:
            patch_path = os.path.abspath(os.path.join(project_root_dir, patch_path))
            if not os.path.exists(patch_path):
                print(f"[⚠️] Warning: Patch file '{patch_path}' does not exist. Skipping patch application.")
                patch_path = None
        align_repo_state(repo_path, tag=tag, branch=branch, patch_path=patch_path)
        build_repo(repo_name, repo_path, tag=tag)

    verify_project_symlinks(project_root_dir, resolved_tools_dir, resolved_soc_dir, resolved_os_dir, selected_ids)

    if "networking" in selected_ids:
        configure_wireshark_permissions()
        configure_vfio_user_dissector(project_root_dir)

    if "networking" in selected_ids or "debug" in selected_ids:
        configure_x11_resources()

    print("[+] Setup completed")

if __name__ == "__main__":
    main()
