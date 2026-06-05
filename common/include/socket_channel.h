/**
 * @file socket_channel.h
 * @brief Socket channel declarations for PCIe co-simulation.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#pragma once

#define MAIN_CHANNEL 0
#define ASYNC_CHANNEL 1

#define BLOCK -1
#define NONBLOCK 0

class Protocol;

typedef struct socket_cfg_s {
    int port; // For TCP: port number; For UDS: unused
    const char* addr; // For TCP: IP address or hostname; For UDS: socket file path
} SocketCfg_t;

enum class Mode : uint8_t { NONE, UDS, TCP };
class Socket {
public:
    Socket();
    Mode mode;
    bool peek;
    bool connected; // is protocol connected
    int  fd; // socket file descriptor
    const char* label; // transport label for logging
};

class SocketChannel
{
public:
    SocketChannel();
    int connect(const SocketCfg_t *cfg);
    void disconnect();
    Protocol receive(char *dbuf = nullptr);
    Protocol asyncListen(char *dbuf = nullptr);
    void send(const Protocol& pkt) const;
    void send(const Protocol& pkt, const void* data) const;
    int setTimeout(int channel, int timeout_ms);
    int channel; // active channel index
private:
    Socket socket[2];

    int connect(int channel, const char* sock);
    int connect(int channel, const char* address, int port);
    int handshake(int channel);
};

int doReceive(int fd, char* dbuf, const size_t len, int flags = 0);
extern const char* transLabel[]; // transport labels for logging
extern const char* channelLabel[]; // channel labels for logging

extern SocketChannel *socketChannel;

#define TIME_TO_MS(min, sec, ms) (((min) * 60000UL) + ((sec) * 1000UL) + (ms))
