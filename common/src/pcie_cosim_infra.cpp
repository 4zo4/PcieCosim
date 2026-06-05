/**
 * @file pcie_cosim_infra.cpp
 * @brief Infrastructure functions implementation for PCIe co-simulation.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#include <iostream>
#include <cstdarg>
#include <cstdint>

#include "protocol.h"
#include "log.h"

static uint64_t boot_ts = 0;

void set_timestamp(void)
{
#ifndef WINDOWS
    if (boot_ts != 0)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = ((uint64_t)ts.tv_sec * 1000000ULL + (ts.tv_nsec / 1000ULL));
    setenv("BASE_TIME", std::to_string(now).c_str(), 1);
    boot_ts = now;
#endif
}

uint64_t get_timestamp(void)
{
#ifndef WINDOWS
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = ((uint64_t)ts.tv_sec * 1000000ULL + (ts.tv_nsec / 1000ULL));

    if (boot_ts == 0) {
        const char* base_time = getenv("BASE_TIME");
        if (base_time)
            boot_ts = std::stoull(base_time);
        else
            boot_ts = now;
    }
    return (now - boot_ts);
#else
    return boot_ts;
#endif
}

void addPrefix(const std::string& tag)
{
#ifndef WINDOWS
    uint64_t ts = get_timestamp();
    uint32_t us = (uint32_t)(ts % 1000000), tsec = (uint32_t)(ts / 1000000);
    uint32_t sec = tsec % 60, min = (tsec / 60) % 60, hour = (tsec / 3600) % 24, days = tsec / 86400;
    printf("\033[2m[%03u:%02u:%02u:%02u.%06u]\033[0m [%s] ", days, hour, min, sec, us, tag.c_str());
#else
    printf("[%s] ", tag.c_str());
#endif
}

#if defined(ENABLE_LOGS) && (ENABLE_LOGS == 1)
void log_msg(int level, const char *prefix, const char *func, const int line, const char* format, ...)
{
    if (level <= LOG_LEVEL) {
        addPrefix(prefix);
        printf("%s:%d: ", func, line);
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        putchar('\n');
        fflush(stdout);
    }
}

static void dumpBuf(const void *dbuf, const size_t count)
{
    for (size_t i = 0; i < count/sizeof(uint32_t); i++) {
        printf("%08x", *(static_cast<const uint32_t*>(dbuf) + i));
        if ((i + 1) < count/sizeof(uint32_t)) {
            if ((i + 1) % sizeof(uint32_t) == 0)
                putchar('\n');
            else
                putchar(' ');
        }
    }
}

static const char* dirLabel[] = {"*", "RX", "TX"};
void log_pkt(Dir direction, const Protocol& pkt, const std::string& tag, const char *dbuf)
{
    addPrefix(dirLabel[static_cast<uint8_t>(direction)]);
    printf("%s Type: %s (%u) | Addr: 0x%lx | Count: %u | Req Id: %d | Tag: %u | Status: %u", tag.c_str(),
            pktTypeToStr(pkt.type), pkt.type, (unsigned long)pkt.addr, pkt.count, pkt.req_id, pkt.tag, pkt.status);
    if (direction == Dir::RX && pkt.type == PktType::MEM_RD) {
        putchar('\n');
        fflush(stdout);
        return;
    }
    if (dbuf && pkt.count > 0) {
        printf(" | Data:\n");
        dumpBuf(dbuf, pkt.count);
    }
    putchar('\n');
    fflush(stdout);
}

const char* pktTypeToStr(PktType type)
{
    switch (type) {
        case PktType::INVALID: return "INVALID";
        case PktType::ACK:     return "ACK";
        case PktType::NACK:    return "NACK";
        case PktType::STATUS:  return "STATUS";
        case PktType::MEM_RD:  return "MEM_RD";
        case PktType::MEM_WR:  return "MEM_WR";
        case PktType::CFG_RD:  return "CFG_RD";
        case PktType::CFG_WR:  return "CFG_WR";
        case PktType::MSG:     return "MSG";
        case PktType::NO_RSP:  return "NO_RSP";
    }
    return "UNKNOWN";
}

const char* pktTypeToStr(uint8_t type) {
    return pktTypeToStr(static_cast<PktType>(type));
}

#endif