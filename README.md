# PCIe Cosim

PCIe co-simulation framework using Verilator and QEMU vfio-user-pci with openPCIE and AXI RAM endpoint RTL blocks.

## Project Architecture

- **`common/`**: Core infrastructure logic and logging primitives.
    - `include/`: Shared header templates for logging, packet protocols, and IPC socket channels.
    - `src/`: The socket channel definitions and system infra layers.
- **`src/bridge/`**: The host environment bridge mapping the `vfio-user` protocol layer onto the simulation.
- **`src/hdl/`**: Core synthesizable components (`axi_ram.v`) and top-level structural wrapper (`sim_top.sv`).
- **`src/sim/`**: Verilator hardware simulation wrapper orchestration logic and memory transaction drivers.
- **`logs/`**: Dynamic execution logs captured during live validation simulation sweeps.
- **`third_party/`**: External library frameworks and environment dependencies.
    - `hw/`: Workspace directory context targeting local `openPCIE` repository tracking node.
    - `lib/`: Workspace directory context targeting local `libvfio-user` repository compilation tree.
    - `os/images/linux/`: Local loop assets, file system initialization targets, and platform kernels.
- `build_vmlinuz.v6.8_pcie_cosim.sh`: Utility script adapting kernel compilation arrays with target co-simulation parameters.
- `download_os_images.py`: Automation script parsing cloud distribution layers down to the execution image tree.
- `run_pcie_agent.py`: System automation script wrapping QEMU guests alongside the target RTL Verilated simulation.

## Quick Start

### 1. Prerequisites

 - **Build Tools**: `gcc`, `ninja`, `make`
 - **Python**: `python3`
```text
    sudo apt install python3-pexpect
```
 - **Verilator**:
```text
    git clone https://github.com/verilator/verilator.git
    For install from git refer to Verilator User Guide at https://verilator.org/guide/latest/install.html
```
 - **QEMU**:
```text
    sudo apt install -y libjson-c-dev libpixman-1-dev libglib2.0-dev libslirp-dev
    sudo apt purge qemu-system-x86
    git clone https://github.com/qemu/qemu.git
    cd qemu/
    ./configure --target-list=x86_64-softmmu --enable-kvm --enable-debug --enable-slirp
    make -j$(nproc)
    cd build/
    sudo ninja install
```
 - **openPCIE**:
```text
    git clone https://github.com/chili-chips-ba/openPCIE.git
    cd /path/to/your/local/PcieCosim/third_party/hw
    ln -s /path/to/your/local/openPCIE openPCIE
    Example:
        ln -s /home/purple/soc/openPCIE openPCIE
```
 - **libvfio-user**:
```text
    git clone https://github.com/nutanix/libvfio-user.git
    cd /path/to/your/local/PcieCosim/third_party/lib
    ln -s /path/to/your/local/libvfio-userlibv fio-user
    Example:
        ln -s /home/purple/tools/libvfio-user libvfio-user
```
 - **linux kernel**:
```text
    git clone https://github.com/torvalds/linux.git
    cd linux
    git checkout v6.8
```
### 2. Get Linux Distribution

To download Linux Distribution images do
```text
    chmod +x /path/to/your/local/PcieCosim/download_os_images.py
    /path/to/your/local/PcieCosim/download_os_images.py
```
This installs CirrOS Linux distribution.

### 3. Build

To build Linux kernel image with PCIe Co-Simulation configuration changes do
```text
    chmod +x /path/to/your/local/PcieCosim/build_vmlinuz.v6.8_pcie_cosim.sh
    /path/to/your/local/PcieCosim/build_vmlinuz.v6.8_pcie_cosim.sh
```
this generates the customized target kernel and copies the image asset into `/path/to/your/local/PcieCosim/third_party/os/images/linux`

To build PCIe co-simulation do
```text
    /path/to/your/local/PcieCosim/make
        or
    /path/to/your/local/PcieCosim/make trace
```
this outputs target executable to `/path/to/your/local/PcieCosim/build/pcie_sim`

### 4. Run

Execute the automation wrapper to start PCIe co-simulation with QEMU, bridge daemon, and RTL PCIe AXI RAM endpoint simulation:
```text
    chmod +x /path/to/your/local/PcieCosim/run_pcie_agent.py
    /path/to/your/local/PcieCosim/run_pcie_agent.py
```

### 5. Misc


