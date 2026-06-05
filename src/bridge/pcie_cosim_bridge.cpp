/**
 * @file pcie_cosim_bridge.cpp
 * @brief PCIe co-simulation bridge implementation.
 * The PCIe Bridge (parent process) forks the PCIe Simulation (child process) and establishes
 * communication channels using either TCP or UDS sockets.
 * The PCIe Simulation is TCP/UDS Server that listens for incoming packets from the PCIe Bridge,
 * and PCIe Bridge is TCP/UDS Client for the PCIe Simulation.
 * The PCIe Bridge acts as a vfio-user server, facilitating communication between the QEMU and the PCIe EP Simulation.
 * When the PCIe Bridge receives packets from QEMU it forwards the PCIe packets to the PCIe Simulation, which completes the PCIe transaction.
 * For no-posted operations like MRd, the PCIe Simulation sends an ACK along with the requested data back to the PCIe Bridge,
 * which then forwards the data to QEMU.
 *
 * (QEMU vfio-user-pci) + Linux OS
 *  |
 *  +-- vfio-user UDS proto --> (vfio-user Server)<= PCIe Bridge =>(UDS/TCP Client) <== UDS/TCP ==> verilated PCIe Sim (UDS/TCP Server) EP
 *
 *                   +---------------------$-------------------+
 *                   | PCIe Cosim Bridge                       |
 * QEMU / vfio-user  | +-------------------+                   |
 *   Request ------> | | Main Thread       |                   |        $
 *                   | | (Blocking Client) | --[Main Socket]-> |     Verilated
 * QEMU / vfio-user  | +-------------------+                   | <=> PCIe SIM
 *   <- Return Data  |           | (Blocks for ACK/Data)       |   (Dual Server)
 *                   |           v                             |        $
 *                   | +-------------------+                   |
 *                   | | Async Listener    |                   |
 *   <--- Inject IRQ | | Thread (Poll)     | <-[Async Socket]- |
 *                   | +-------------------+                   |
 *                   +---------------------$-------------------+
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#include <iostream>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "protocol.h"
#include "log.h"
#include "socket_channel.h"

#define BAR_SHIFT 56
#define PCIE_TCP_PORT1 5001
#define PCIE_TCP_PORT2 5002
#define LOOPBACK_IP_ADDR "127.0.0.1"
#define PCIE_SOCK1 "/tmp/pcie-cosim1.sock"
#define PCIE_SOCK2 "/tmp/pcie-cosim2.sock"
#define PCIE_REQUESTER_ID 0x0100

#define MAX_WAIT_TIME TIME_TO_MS(0,10,0)

extern "C" {
    int vfu_irq_trigger(struct vfu_ctx *vfu_ctx, uint32_t subindex);
}
void set_timestamp(void);
void runPcieCosim(const SocketCfg_t *cfg);
static void startPcieCosim(const SocketCfg_t *cfg);
static volatile sig_atomic_t pcieCosimReady = 0;

static pid_t pcieCosimPid = 0;
SocketChannel *socketChannel = nullptr;

int pcie_bridge_disconnect(void)
{
    if (socketChannel) {
        socketChannel->send(Protocol(PktType::NACK));
        socketChannel->disconnect();
        usleep(1000); // let some time to terminate gracefully
        return 0;
    }
    return -1;
}

void pcie_bridge_async_listener(struct vfu_ctx *vfu_ctx)
{
    char dbuf[256];
    LOG_INFO("PCIe-Bridge","Async listener started");

    while (true) {
        Protocol pkt = socketChannel->asyncListen(dbuf);

        if (pkt.type == PktType::INVALID) {
            LOG_INFO("PCIe-Bridge","Disconnected");
            break;
        }

        if (pkt.type == PktType::MSG) {
            uint32_t vector_idx = *reinterpret_cast<uint32_t*>(dbuf);
            socketChannel->channel = MAIN_CHANNEL; // switch back to Main channel
            
            LOG_INFO("PCIe-Bridge","Delivering Irq vector index %u", vector_idx);
            
            int ret = vfu_irq_trigger(vfu_ctx, vector_idx);
            if (ret < 0) {
                LOG_ERROR("PCIe-Bridge","[PKT][ERROR] vfu_irq_trigger returned %d (errno: %d)", ret, errno);
            }
        } else {
            LOG_ERROR("PCIe-Bridge","[PKT][ERROR] received invalid pkt %s (%d)", pktTypeToStr(pkt.type), pkt.type);
        }
    }
}

static ssize_t pcie_bridge(uint16_t bar_id, char *buf, size_t count, loff_t offset, bool is_write)
{
    static uint8_t tran = 1;
    uint64_t pcie_addr = (uint64_t)offset | ((uint64_t)bar_id << BAR_SHIFT);

    const Protocol pkt(is_write ? PktType::MEM_WR : PktType::MEM_RD,
                        (uint64_t)pcie_addr, (uint16_t)count, tran++, PCIE_REQUESTER_ID);
    if (is_write) {
        LOG_INFO("PCIe-Bridge","[PKT] Mem Write Addr: 0x%lx Count: %u", offset, (unsigned)count);
        socketChannel->send(pkt, buf);
        return count;
    } else {
        LOG_INFO("PCIe-Bridge","[PKT] Mem Read Addr: 0x%lx Count: %u", offset, (unsigned)count);
        socketChannel->send(pkt);
        socketChannel->setTimeout(socketChannel->channel, MAX_WAIT_TIME);
        const Protocol resp = socketChannel->receive(buf);
        socketChannel->setTimeout(socketChannel->channel, BLOCK);

        if (resp.type == PktType::ACK &&
            resp.tag == pkt.tag && resp.count == count) {
            return count;
        } else {
            LOG_ERROR("PCIe-Bridge","[PKT][ERROR] type %d tag %d count %d",
                    resp.type, resp.tag, resp.count);
            return 0;
        }
    }
}

ssize_t pcie_bridge_bar0(struct vfu_ctx *vfu_ctx, char *buf, size_t count, loff_t offset, bool is_write)
{
    (void)vfu_ctx;
    return pcie_bridge(0, buf, count, offset, is_write);
}

ssize_t pcie_bridge_bar1(struct vfu_ctx *vfu_ctx, char *buf, size_t count, loff_t offset, bool is_write)
{
    (void)vfu_ctx;
    return pcie_bridge(1, buf, count, offset, is_write);
}

int pcie_bridge_init(void)
{
    if (!socketChannel) {
        set_timestamp();
        addPrefix("PCIe-Bridge");
        std::cout << "Starting PCIe-SIM" << std::endl;

        SocketCfg_t cfg[2] = {
#if defined(ENABLE_TCP) && (ENABLE_TCP == 1)
            [0] = {
                .port = PCIE_TCP_PORT1,
                .addr = LOOPBACK_IP_ADDR,
            },
            [1] = {
                .port = PCIE_TCP_PORT2,
                .addr = LOOPBACK_IP_ADDR,
            },
#else // UDS
            [0] = {
                .port = -1,
                .addr = PCIE_SOCK1,
            },
            [1] = {
                .port = -1,
                .addr = PCIE_SOCK2,
            },
#endif
        };

        startPcieCosim(cfg);

        socketChannel = new SocketChannel();
        int ret = socketChannel->connect(cfg);
        if (ret == 0) {
            addPrefix("PCIe-Bridge");
            std::cout << "Connected to PCIe-SIM" << std::endl;
            return 0;
        }
    }
    return -1;
}

void handle_sigusr(int sig) {
    (void)sig;
    pcieCosimReady = 1;
}

static void startPcieCosim(const SocketCfg_t *cfg)
{
    signal(SIGUSR1, handle_sigusr);

    pcieCosimPid = fork();

    if (pcieCosimPid == 0) {
        runPcieCosim(cfg);
        exit(0);
    } else {
        int timeout = 0;
        while (!pcieCosimReady) {
            int status;
            pid_t ret = waitpid(pcieCosimPid, &status, WNOHANG);
            if (ret == -1) {
                addPrefix("PCIe-Bridge");
                std::cerr << __func__ << " FATAL: Failed to start PCIe cosim" << std::endl;
                exit(1);
            }
            if (timeout >= 1000) {
                addPrefix("PCIe-Bridge");
                std::cerr << __func__ << " FATAL: Timeout waiting for PCIe cosim signal" << std::endl;
                kill(pcieCosimPid, SIGKILL);
                exit(1);
            }
            usleep(1000);
            timeout++;
        }
    }
}

