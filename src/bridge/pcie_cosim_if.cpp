/**
 * @file pcie_cosim_if.cpp
 * @brief PCIe co-simulation interface for the Verilated PCIe RTL Simulation module.
 * This file contains the implementation of the TCP/UDP Server interface functions that
 * facilitate communication between the PCIe Bridge and the Verilated PCIe RTL Simulation.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#include <iostream>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iomanip>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>

#include "protocol.h"
#include "log.h"
#include "socket_channel.h"

#if defined(ENABLE_WAIT_LIMIT) && (ENABLE_WAIT_LIMIT == 1)
#define MAX_WAIT_TIME TIME_TO_MS(0,10,0)
#else
#define MAX_WAIT_TIME BLOCK
#endif

// Forward declarations for functions implemented in pcie_cosim.cpp
void doWrite(Protocol& pkt, Buf& dbuf);
void doRead(Protocol& pkt, Buf& dbuf);
int  initRtlSim();
void idleRtlSim();
void terminateRtlSim();

typedef struct channel_s {
    const char* label; // transport label for logging
    int socket[2]; // socket[0] is the listening socket, socket[1] is the accepted connection socket
} Channel_t;

#if defined(ENABLE_LOGS) && (ENABLE_LOGS == 1)
static inline void logChannel(const int level, Channel_t *channel, int x, const char* prefix)
{
    if (level > LOG_LEVEL)
        return;
    bool once = false;
    for (int i = 0; i < 2; ++i) {
        if (channel[i].socket[x] > 0) {
            if (!once) {
                printf("%s", prefix);
            }
            printf("%s%s socket %d", once ? " and " : "", channel[i].label, channel[i].socket[x]);
            once = true;
        }
    }
    if (once) {
        putchar('\n');
        fflush(stdout);
    }
}
#else // !ENABLE_LOGS
#define logChannel(level, channel, x, prefix) NOP()
#endif

bool isRead(PktType type)
{
    return type == PktType::MEM_RD || type == PktType::CFG_RD;
}

bool isRead(uint8_t type)
{
    return isRead(static_cast<PktType>(type));
}

bool isWrite(PktType type)
{
    return type == PktType::MEM_WR || type == PktType::CFG_WR;
}

bool isWrite(uint8_t type)
{
    return isWrite(static_cast<PktType>(type));
}

#if defined(ENABLE_MOCK) && (ENABLE_MOCK == 1)

#define initRtlSim() 0
#define terminateRtlSim() NOP()

static int pin[16];
static char mock_ram[sizeof(uint32_t) * (16 + 4)]; // 16 32-bit words for mock memory operations

void doRead(Protocol& pkt, Buf& dbuf)
{
    if (pkt.type == PktType::MEM_RD) {
        char* buf = dbuf.data();

        if (pkt.addr + pkt.count <= sizeof(mock_ram)) {
            memcpy(buf, &mock_ram[pkt.addr], pkt.count);
        } else {
            memset(buf, 0, pkt.count);
        }

        if (pkt.addr == 0x1) {
            buf[0] = 0;
            for (int i = 0; i < 8; i++) {
                if ((pin[i] % 3) == 0) buf[0] |= (1 << i);
            }
        } else if (pkt.addr == 0x5) {
            buf[0] = 0;
            for (int i = 0; i < 8; i++) {
                if ((pin[i + 8] % 3) == 0) buf[0] |= (1 << i);
            }
        }

        dbuf.len = pkt.count;
    }
}

void doWrite(Protocol& pkt, Buf& dbuf)
{
    if (pkt.type == PktType::MEM_WR) {
        char* buf = dbuf.data();

        if (pkt.addr + pkt.count <= sizeof(mock_ram))
            memcpy(&mock_ram[pkt.addr], buf, pkt.count);

        if (pkt.addr == 0x0) {
            for (int i = 0; i < 8; i++) {
                if (buf[0] & (1 << i)) pin[i]++;
            }
        } else if (pkt.addr == 0x4) {
            for (int i = 0; i < 8; i++) {
                if (buf[0] & (1 << i)) pin[i + 8]++;
            }
        }
    }
}
#endif

void handleSigint(int sig)
{
    (void)sig;
    const char msg[] = "[PCIe-SIM] received SIGINT. Waiting for NACK from PCIe-Bridge to terminate.\n";
    write(STDOUT_FILENO, msg, sizeof(msg)); 
}

// transOrder[i] gives the index of the transport that completed the handshake i-th (0 or 1)
static int transOrder[2] = { -1, -1 };
static int activeChannel[2] = {-1, -1};

void sendAsync(PktType type, const uint32_t *data, uint16_t count)
{
    Protocol pkt(type, 0, count);
    ssize_t size = (ssize_t)sizeof(Protocol);
    int err = 0;
    ssize_t sent = ::send(activeChannel[ASYNC_CHANNEL], &pkt, sizeof(Protocol), MSG_NOSIGNAL);
    if (sent < size) {
        err = -1;
    }
    size = (ssize_t)count;
    sent = ::send(activeChannel[ASYNC_CHANNEL], data, count, MSG_NOSIGNAL);
    if (sent < size) {
        if (err == -1) {
            size += (ssize_t)sizeof(Protocol);
        } 
        err = -1;
    }
    if (err) {
        LOG_ERROR("PCIe-SIM","send failed on socket %d %s channel %d: "
            "%s (errno %d), bytes sent %d instead %u",
            activeChannel[ASYNC_CHANNEL], channelLabel[ASYNC_CHANNEL], ASYNC_CHANNEL,
            strerror(errno), errno, sent, size);
        fprintf(stderr, "send failed on socket %d %s channel %d: %s (errno %d)\n",
            activeChannel[ASYNC_CHANNEL], channelLabel[ASYNC_CHANNEL], ASYNC_CHANNEL,
            strerror(errno), errno);
    } else {
        LOG_DEBUG("PCIe-SIM","on socket %d %s channel %d",
            activeChannel[ASYNC_CHANNEL], channelLabel[ASYNC_CHANNEL], ASYNC_CHANNEL);
        LOG_PKT(Dir::TX, pkt, (const char*)data);
    }
}

int doAccept(Channel_t *channel)
{
    struct pollfd fds[2] = {{ channel[0].socket[0] , POLLIN, 0}, { channel[1].socket[0], POLLIN, 0 }};
    int sock = -1;
    int next = 0;

    logChannel(LOG_LEVEL_INFO, channel, 0, "[PCIe-SIM] Waiting for connection on ");

    while (next < 2) {
        int ret = poll(fds, 2, MAX_WAIT_TIME);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("[PCIe-SIM] Poll error");
            exit(1);
        } else if (ret == 0) {
            LOG_ERROR("PCIe-SIM","ERROR: Timeout waiting for connections");
            return -1;
        }

        for (int i = 0; i < 2; i++) {
            if (fds[i].fd > 0 && (fds[i].revents & POLLIN)) {
                sock = accept(fds[i].fd, NULL, NULL);
                if (sock < 0) {
                    LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) on %s socket %d %s channel %d while accepting connection",
                              strerror(errno), errno, channel[i].label, fds[i].fd, channelLabel[i], i);
                    continue;
                }
                channel[i].socket[1] = sock;
                transOrder[next++] = i;
                LOG_DEBUG("PCIe-SIM","Accepted connection on %s socket %d %s channel %d",
                            channel[i].label, sock, channelLabel[i], i);
                fds[i].fd = -1;
                close(channel[i].socket[0]);
            }
        }
    }
    return 0;
}

static int activeSocket = -1;

int receive(Protocol& pkt, Buf& dbuf, Channel_t *channel)
{
    struct pollfd fds[2] = {{channel[0].socket[1], POLLIN, 0}, {channel[1].socket[1], POLLIN, 0}};
    const int alt[2] = { 1, 0 }; // alt[i] gives the index of the alternate transport for transport i

    while (true) {
        int ret = poll(fds, 2, BLOCK);
        if (ret <= 0) {
            if (ret < 0 && errno == EINTR)
                continue;
            perror("[PCIe-SIM] Poll error");
            return -1;
        }

        for (int i = 0; i < 2; i++) {
            if (fds[i].fd > 0 && (fds[i].revents & POLLIN)) {
                int len = doReceive(fds[i].fd, reinterpret_cast<char*>(&pkt), sizeof(Protocol));

                if (len < 0) {
                    close(fds[i].fd);
                    channel[i].socket[1] = -1;
                    if (activeSocket == fds[i].fd && channel[alt[i]].socket[1] > 0) {
                        activeSocket = channel[alt[i]].socket[1];
                    } else {
                        activeSocket = -1;
                    }
                    LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) closing %s socket %d %s channel %d",
                              strerror(errno), errno, channel[i].label, fds[i].fd, channelLabel[i], i);
                    fds[i].fd = -1;
                    pkt.type = U8(PktType::INVALID);
                    return 0;
                }

                if (isWrite(pkt.type) && pkt.count > 0) {
                    if (pkt.count > dbuf.vec.size()) {
                        LOG_ERROR("PCIe-SIM","ERROR: Packet count %u exceeds Rx buffer size %zu",
                                  pkt.count, dbuf.vec.size());
                        return -1;
                    }
                    len = doReceive(fds[i].fd, dbuf.data(), pkt.count);
                    if (len == pkt.count)
                        dbuf.len = len;
                    if (len < 0) {
                        close(fds[i].fd);
                        channel[i].socket[1] = -1;
                        if (activeSocket == fds[i].fd && channel[alt[i]].socket[1] > 0) {
                            activeSocket = channel[alt[i]].socket[1];
                        } else {
                            activeSocket = -1;
                        }
                        LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) closing %s socket %d %s channel %d",
                                  strerror(errno), errno, channel[i].label, fds[i].fd, channelLabel[i], i);
                        fds[i].fd = -1;
                        pkt.type = U8(PktType::INVALID);
                        return 0;
                    }
                    activeSocket = fds[i].fd;
                }
                return 0;
            }
        }
    }
}

/**
 * @brief Runs the PCIe simulation
 * @param cfg The configuration for the PCIe Sim communication channels
 */
void runPcieCosim(const SocketCfg_t *cfg)
{
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    signal(SIGINT, handleSigint);

    [[maybe_unused]] auto createUDSlistener = [](const char* sock) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

        if (fd < 0) {
            perror("[PCIe-SIM] UDS socket failed");
            exit(1);
        }

        unlink(sock);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) binding UDS socket %s (%d)",
                      strerror(errno), errno, sock, fd);
            exit(1);
        }

        if (listen(fd, 4) < 0) {
            LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) listening on UDS socket %s (%d)",
                      strerror(errno), errno, sock, fd);
            exit(1);
        }

        return fd;
    };
    [[maybe_unused]] auto createTCPlistener = [](int port, const char* address) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            perror("[PCIe-SIM] socket failed");
            exit(1);
        }

        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("[PCIe-SIM] setsockopt SO_REUSEADDR failed");
            exit(1);
        }
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            perror("[PCIe-SIM] setsockopt SO_REUSEPORT failed");
            exit(1);
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) binding TCP socket %d on port %d",
                      strerror(errno), errno, fd, port);
            exit(1);
        }
        if (listen(fd, 4) < 0) {
            LOG_ERROR("PCIe-SIM","ERROR: %s (errno %d) listening on TCP socket %d port %d",
                      strerror(errno), errno, fd, port);
            exit(1);
        }
        return fd;
    };

    Channel_t channel[] = {
#if defined(ENABLE_TCP) && (ENABLE_TCP == 1)
        { transLabel[2], {createTCPlistener(cfg[0].port, cfg[0].addr), -1 }},
        { transLabel[2], {createTCPlistener(cfg[1].port, cfg[1].addr), -1 }},
#else // UDS
        { transLabel[1], {createUDSlistener(cfg[0].addr), -1 }},
        { transLabel[1], {createUDSlistener(cfg[1].addr), -1 }},
#endif
    };

/**
    Architecture Overview:
    -------------------------
    The PCIe co-simulation environment consists of two main processes:
    The PCIe Bridge (process A) and the Verilated PCIe Simulation (process B).
    The PCIe Bridge acts as a vfio-user server, facilitating communication between the QEMU
    and the PCIe Simulation. The communication channels between PCIe Bridge and the Verilated PCIe Sim
    can be established using either Unix Domain Sockets (UDS) or TCP sockets, depending on the configuration.
    The PCIe Simulation is TCP/UDS Server that listens for incoming packets from the PCIe Bridge,
    and PCIe Bridge is TCP/UDS Client. There are two communication channels established between
    the PCIe Bridge and the PCIe Simulation, which can be used for different types of traffic
    (e.g., one for memory operations and another for PCIe interrupts).
    PCIe MWr is posted operation, so the PCIe Simulation does not send an acknowledgment back to
    the PCIe Bridge after receiving a MWr request.
    For MRd, the PCIe Simulation sends an ACK along with the requested data back to the PCIe Bridge.
    The PCIe Bridge then forwards the data to QEMU, which completes the MRd transaction.

    QEMU + Linux (cirros) + vfio-user-pci
    |
    +--  vfio-user UDS proto --+ pcie_bridge (vfio-user Server) (process A) forks PCIe Sim
                                    ^
                                    | pcie mem (UDS/TCP) proto
                                    v
                                 verilated PCIe Sim (process B)
                                    ^
                                    |
                                    v
                                    OpenPCI EP + AXI mem
 */
    static Buf dbuf(1024); // PCIe TLP data buffer
    bool connected = false;
    auto exchange = [&](PktType type, PktType expected = PktType::NO_RSP,
                    uint8_t tag = 0, uint64_t addr = 0, uint16_t count = 0) -> int {
        Protocol p(type, addr, count, tag);

        if (activeSocket == -1)
            return -1;
        if (type != PktType::INVALID) {
            LOG_PKT(Dir::TX, p, "exchange", dbuf.data());
            ssize_t sent = ::send(activeSocket, &p, sizeof(Protocol), MSG_NOSIGNAL);
            if (sent < (ssize_t)sizeof(Protocol)) {
                std::cerr << "exchange:" << __LINE__ << ": bytes sent " << sent <<
                             " instead " << sizeof(Protocol) << std::endl;
                fprintf(stderr, "send failed on socket %d channel %d: %s (errno %d)\n",
                        activeSocket, channel[0].socket[1] == activeSocket ? 0 : 1, strerror(errno), errno);
                return -2;
            }
            if (dbuf.len == p.count && p.count > 0)
                ::send(activeSocket, dbuf.data(), p.count, 0);

            if (expected == PktType::NO_RSP)
                return 0;
        }

        logChannel(LOG_LEVEL_DEBUG, channel, 1, "[PCIe-SIM] exchange: waiting for packets on ");

        Protocol pkt = {}; dbuf.len = 0;
        int ret = receive(pkt, dbuf, channel);
        if (ret < 0) {
            return ret;
        }
        if (pkt.type != PktType::INVALID) {
            LOG_PKT(Dir::RX, pkt, "exchange", dbuf.data());

            if (isWrite(pkt.type))
                doWrite(pkt, dbuf);

            if (isRead(pkt.type)) {
                doRead(pkt, dbuf);
                p.type = U8(PktType::ACK);
                p.addr = pkt.addr;
                p.count = pkt.count;
                p.tag = pkt.tag;
                LOG_PKT(Dir::TX, p, "exchange", dbuf.data());
                ssize_t sent = ::send(activeSocket, &p, sizeof(Protocol), MSG_NOSIGNAL);
                if (sent < (ssize_t)sizeof(Protocol)) {
                    std::cerr << "exchange:" << __LINE__ << ": bytes sent " << sent <<
                                 " instead " << sizeof(Protocol) << std::endl;
                    fprintf(stderr, "send failed on socket %d channel %d: %s (errno %d)\n",
                            activeSocket, channel[0].socket[1] == activeSocket ? 0 : 1, strerror(errno), errno);
                    return -2;
                }
                if (dbuf.len == p.count && p.count > 0)
                    ::send(activeSocket, dbuf.data(), p.count, 0);
            } else if (pkt.type == PktType::NACK) {
                std::cout << "[PCIe-SIM] disconnected" << std::endl;
                connected = false;
                for (int i = 0; i < 2; i++) {
                    if (channel[i].socket[1] > 0) {
                        close(channel[i].socket[1]);
                        channel[i].socket[1] = -1;
                        if (cfg[i].port == -1) {
                            unlink(cfg[i].addr);
                        }
                    }
                }
                activeSocket = -1;
            }
        }
        return 0;
    };

    int ret = initRtlSim(); // Initialize RTL Sim to get ready for TLPs

    if (ret < 0) {
        LOG_ERROR("PCIe-SIM","Failed to initialize RTL Sim, aborting");
        exit(1);
    }

    LOG_DEBUG("PCIe-SIM","<- Signaling PCIe Bridge");

    kill(getppid(), SIGUSR1); // signal PCIe Bridge that we're ready to accept connections

    if (doAccept(channel) < 0) {
        LOG_ERROR("PCIe-SIM","Failed to accept connections, aborting");
        exit(1);
    }

    // Perform handshake on channels in the order they connected
    for (int i = 0; i < 2; i++) {
        int x = transOrder[i];
        int currentSocket = channel[x].socket[1];
        if (currentSocket > 0) {
            activeSocket = currentSocket;
            LOG_DEBUG("PCIe-SIM","Handshake on %s socket %d", channel[x].label, currentSocket);
            int ret = exchange(PktType::ACK, PktType::ACK, x + 1);
            if (ret != 0) {
                LOG_ERROR("PCIe-SIM","Handshake failed on %s socket %d (error %d), aborting",
                        channel[x].label, currentSocket, ret);
                goto waitPeerTerminate;
            }
            activeChannel[x] = currentSocket;
            LOG_DEBUG("PCIe-SIM","Handshake on %s socket %d done", channel[x].label, currentSocket);
        }
    }

    connected = true;

    addPrefix("PCIe-SIM");
    std::cout << "Waiting for PCIe packets" << std::endl;

waitPeerTerminate:

    while (activeSocket > 0) {
        exchange(PktType::ANY, PktType::ANY);
    }

    terminateRtlSim();

    addPrefix("PCIe-SIM");
    std::cerr << "Terminated" << std::endl;
}