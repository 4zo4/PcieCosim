/**
 * @file socket_channel.cpp
 * @brief Socket channel implementation for PCIe co-simulation.
 *
 * This file contains the implementation for managing socket connections in the PCIe co-simulation environment.
 * It provides functions for creating and managing Unix Domain Sockets and TCP sockets for the PCIe Bridge.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#include <cstring>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

#include "protocol.h"
#include "log.h"
#include "socket_channel.h"

const char* transLabel[] = { "NONE", "UDS", "TCP"}; // transport labels for logging
const char* channelLabel[] = { "Main", "Async"}; // channel labels for logging

int doReceive(int fd, char* dbuf, const size_t len, int flags)
{
    size_t total = 0;
    do {
        int bytesRead = ::recv(fd, dbuf + total, len - total, flags);

        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (total > 0)
                    return (int)total;
                else
                    return -3; // Timeout
            }
            if (errno == EINTR)
                continue;
            return -1;
        }

        // Peer shut down
        if (bytesRead == 0)
            return -2;

        total += bytesRead;
    } while (total < len);

    return (int)total;
}

Socket::Socket()
{
    this->mode = Mode::NONE;
    this->peek = false;
    this->connected = false;
    this->fd = -1;
    this->label = transLabel[static_cast<uint8_t>(this->mode)];
}

SocketChannel::SocketChannel()
{
    socket[0] = Socket();
    socket[1] = Socket();
    this->channel = -1;
}

int SocketChannel::connect(int channel, const char* sock)
{
    struct sockaddr_un addr = {};

    this->socket[channel].fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

    if (this->socket[channel].fd == -1) {
        perror("socket creation failed");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);

    if (::connect(this->socket[channel].fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        this->socket[channel].mode = Mode::UDS;
        this->socket[channel].label = transLabel[static_cast<uint8_t>(Mode::UDS)];
        return 0;
    }

    ::close(this->socket[channel].fd);
    LOG_ERROR("PCIe-Bridge","failed connecting to %s (%d)",
            sock, this->socket[channel].fd);
    this->socket[channel].fd = -1;
    return -1;
}

int SocketChannel::connect(int channel, const char* address, int port)
{
    struct sockaddr_in addr = {};

    this->socket[channel].fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (this->socket[channel].fd == -1) {
        perror("socket creation failed");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, address, &addr.sin_addr);

    int opt = 1;
    if (setsockopt(this->socket[channel].fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[PCIe-Bridge] setsockopt SO_REUSEADDR failed");
        return -1;
    }
    if (setsockopt(this->socket[channel].fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("[PCIe-Bridge] setsockopt SO_REUSEPORT failed");
        return -1;
    }
    setsockopt(this->socket[channel].fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    if (::connect(this->socket[channel].fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        this->socket[channel].mode = Mode::TCP;
        this->socket[channel].label = transLabel[static_cast<uint8_t>(Mode::TCP)];
        return 0;
    }

    ::close(this->socket[channel].fd);
    LOG_ERROR("PCIe-Bridge","failed connecting to %s:%d (%d)",
            address, port, this->socket[channel].fd);
    this->socket[channel].fd = -1;
    return -1;
}

int SocketChannel::connect(const SocketCfg_t *cfg)
{
    int ret = 0, i = 0;

    for (; i < 2; i++) {
        if (cfg[i].port == -1) {
            LOG_DEBUG("PCIe-Bridge","<- UDS Connect %s", cfg[i].addr);
            ret = this->connect(i, cfg[i].addr);
            if (ret == -1)
                break;
            LOG_DEBUG("PCIe-Bridge","-> UDS Connected on socket %d",
                    this->socket[i].fd);
        } else if (cfg[i].port > 0 && cfg[i].port < 65536) {
            LOG_DEBUG("PCIe-Bridge","<- TCP Connect %s port %d",
                      cfg[i].addr, cfg[i].port);
            ret = this->connect(i, cfg[i].addr, cfg[i].port);
            if (ret == -1)
                break;
            LOG_DEBUG("PCIe-Bridge","-> TCP Connected on socket %d",
                    this->socket[i].fd);
        } else {
            LOG_ERROR("PCIe-Bridge","Invalid port %d for socket %d",
                    cfg[i].port, this->socket[i].fd);
            return -1;
        }
    }

    if (ret != 0) {
        LOG_ERROR("PCIe-Bridge","%s Connection on socket %d failed, aborting",
            this->socket[i].label, this->socket[i].fd);
        return -1;
    }

    for (i = 0; i < 2; i++) {
        ret = handshake(i);
        if (ret != 0) {
            LOG_ERROR("PCIe-Bridge","Handshake failed on %s socket %d, aborting",
                this->socket[i].label, this->socket[i].fd);
            return -1;
        }
    }

    this->channel = MAIN_CHANNEL;
    return 0;
}

int SocketChannel::handshake(int channel)
{
    this->channel = channel;

    Protocol pkt = receive();

    if (pkt.type == PktType::ACK) {
        LOG_DEBUG("PCIe-Bridge","-> Handshake over %s received",
            this->socket[channel].label);
        send(Protocol(PktType::ACK, 0, 0, pkt.tag));
        LOG_DEBUG("PCIe-Bridge","<- Handshake over %s acked",
            this->socket[channel].label);
        this->socket[channel].connected = true;
        LOG_INFO("PCIe-Bridge","%s Connection on socket %d %s channel %d synced",
            this->socket[channel].label, this->socket[channel].fd, channelLabel[channel], channel);
    } else {
        send(Protocol(PktType::NACK, 0, 0, pkt.tag));
        LOG_ERROR("PCIe-Bridge","%s Connection on socket %d channel %d failed",
            this->socket[channel].label, this->socket[channel].fd, channelLabel[channel], channel);
        return -1;
    }
    return 0;
}

Protocol SocketChannel::receive(char *dbuf)
{
    Protocol pkt = {};
    int bytesRead = doReceive(this->socket[channel].fd, reinterpret_cast<char*>(&pkt), sizeof(Protocol));

    if (bytesRead < 0 || static_cast<size_t>(bytesRead) != sizeof(Protocol)) {
        pkt.type = U8(PktType::INVALID);
        LOG_ERROR("PCIe-Bridge","%s Connection on socket %d %s channel %d %s",
                this->socket[channel].label, this->socket[channel].fd, channelLabel[channel], channel,
                (bytesRead == -1 ? "error" : (bytesRead == -2 ? "shut down" :
                (bytesRead == -3 ? "timeout" : "closed"))));
        fprintf(stderr, "receive:%d: failed on socket %d channel %s %d: %s (errno %d)\n", __LINE__,
                this->socket[channel].fd, channelLabel[channel], channel, strerror(errno), errno);
        return pkt;
    } else {
        LOG_PKT(Dir::RX, pkt);
    }

    if (dbuf && pkt.count > 0) {
        bytesRead = doReceive(this->socket[channel].fd, dbuf, pkt.count);
        if (bytesRead < 0 || static_cast<size_t>(bytesRead) != pkt.count) {
            pkt.type = U8(PktType::INVALID);
            LOG_ERROR("PCIe-Bridge","%s Connection on socket %d %s channel %d %s while receiving data",
                    this->socket[channel].label, this->socket[channel].fd, channel,
                    (bytesRead == -1 ? "error" : (bytesRead == -2 ? "shut down" :
                    (bytesRead == -3 ? "timeout" : "closed"))));
            fprintf(stderr, "receive:%d: failed on socket %d %s channel %d: %s (errno %d)\n", __LINE__,
                    this->socket[channel].fd, channelLabel[channel], channel, strerror(errno), errno);
        }
    }
    return pkt;
}

void SocketChannel::send(const Protocol& pkt, const void* data) const
{
    LOG_PKT(Dir::TX, pkt);

    size_t iovcnt = 1;
    struct iovec iov[2];
    iov[0].iov_base = const_cast<Protocol*>(&pkt);
    iov[0].iov_len = sizeof(Protocol);

    if (pkt.count > 0 && data) {
        iov[1].iov_base = const_cast<void*>(data);
        iov[1].iov_len = pkt.count;
        iovcnt = 2;
    }
    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = iovcnt,
    };

    ssize_t len = pkt.count > 0 && data ? sizeof(Protocol) + pkt.count : sizeof(Protocol);
    ssize_t sent = ::sendmsg(this->socket[channel].fd, &msg, MSG_NOSIGNAL);
    if (sent < len) {
        LOG_ERROR("PCIe-Bridge","send failed on %s socket %d %s channel %d: "
                "%s (errno %d), bytes sent %d instead %u",
                this->socket[channel].label, this->socket[channel].fd,
                channelLabel[channel], channel, strerror(errno), errno, sent, len);
    }
}

void SocketChannel::send(const Protocol& pkt) const
{
    LOG_PKT(Dir::TX, pkt);

    struct iovec iov = {
        .iov_base = const_cast<Protocol*>(&pkt),
        .iov_len = sizeof(Protocol),
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    ssize_t sent = ::sendmsg(this->socket[channel].fd, &msg, MSG_NOSIGNAL);
    if (sent < (ssize_t)(sizeof(Protocol))) {
        LOG_ERROR("PCIe-Bridge","send failed on %s socket %d %s channel %d: "
                "%s (errno %d), bytes sent %d instead %u",
                this->socket[channel].label, this->socket[channel].fd,
                channelLabel[channel], channel, strerror(errno), errno, sent, sizeof(Protocol));
    }
}

int SocketChannel::setTimeout(int channel, int timeout_ms)
{
    if (channel < 0 || channel > 1) {
        LOG_ERROR("PCIe-Bridge","Cannot set timeout: invalid channel %d selected", channel);
        return -1;
    }

    int fd = this->socket[channel].fd;
    if (fd < 0) {
        LOG_ERROR("PCIe-Bridge","Cannot set timeout: invalid socket "
                "file descriptor %d for channel %s %d", fd, channelLabel[channel], channel);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms == BLOCK ? 0 : timeout_ms / 1000;
    tv.tv_usec = timeout_ms == BLOCK ? 0 : (timeout_ms % 1000) * 1000;

    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_ERROR("PCIe-Bridge","setsockopt SO_RCVTIMEO failed on socket %d "
                "channel %d: %s (errno %d)", fd, channel, strerror(errno), errno);
        fprintf(stderr, "setTimeout:%d: failed on socket %d %s channel %d: %s (errno %d)\n", __LINE__,
                this->socket[channel].fd, channelLabel[channel], channel, strerror(errno), errno);
        return -1;
    }

    LOG_DEBUG("PCIe-Bridge","Timeout set to %d ms on %s socket %d %s channel %d",
              timeout_ms == BLOCK ? 0 : timeout_ms, this->socket[this->channel].label,
              fd, channelLabel[channel], channel);
    return 0;
}

bool isAsync(PktType type)
{
    return type == PktType::STATUS || type == PktType::MSG;
}

bool isAsync(uint8_t type)
{
    return isAsync(static_cast<PktType>(type));
}

Protocol SocketChannel::asyncListen(char *dbuf)
{
    struct pollfd fds[1] = { this->socket[ASYNC_CHANNEL].fd, POLLIN, 0};
    Protocol pkt(PktType::INVALID);

    while (true) {
        int ret = poll(fds, 1, BLOCK);
        if (ret <= 0) {
            if (ret < 0 && errno == EINTR)
                continue;
            perror("[PCIe-Bridge] Poll error");
            return pkt;
        }
        if (fds[0].revents & (POLLHUP | POLLERR)) {
            LOG_DEBUG("PCIe-Bridge","Async channel closed");
            return pkt;
        }
        if (fds[0].fd > 0 && (fds[0].revents & POLLIN)) {
            this->channel = ASYNC_CHANNEL;
            pkt = this->receive(dbuf);
            if (!isAsync(pkt.type)) {
                LOG_ERROR("PCIe-Bridge","%s received %s packet on socket %d channel %s %d",
                    this->socket[ASYNC_CHANNEL].label, pktTypeToStr(pkt.type),
                    channelLabel[ASYNC_CHANNEL], ASYNC_CHANNEL);
                pkt.type = U8(PktType::INVALID);
            }
            return pkt;
        }
    }
}

void SocketChannel::disconnect()
{
    this->socket[MAIN_CHANNEL].connected = false;
    this->socket[ASYNC_CHANNEL].connected = false;
    shutdown(this->socket[MAIN_CHANNEL].fd, SHUT_WR);
    shutdown(this->socket[ASYNC_CHANNEL].fd, SHUT_WR);
}