/**
 * @file protocol.h
 * @brief Protocol declarations for PCIe co-simulation.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#pragma once

#include <stdexcept>
#include <stdint.h>
#include <string>
#include <vector>

enum class PktType : uint8_t
{
    INVALID,
    ANY = INVALID, // wildcard for matching any packet type
    ACK,
    NACK,
    STATUS,
    MEM_RD,
    MEM_WR,
    CFG_RD,
    CFG_WR,
    MSG,          // Message packet type (e.g., MSI IRQ, MSI-X IRQ, etc.)
    NO_RSP = 0xFF // special type indicating no response expected
};

// IRQ types reflects the types of interrupts that vifio-user can handle (see vfu_dev_irq_type in libvfio-user.h),
// including legacy INTx, MSI, MSI-X, error interrupts, and request interrupts.
enum class IrqType : uint8_t
{
    DEV_INT_NONE = 0,
    DEV_INTX_IRQ,
    DEV_MSI_IRQ,
    DEV_MSIX_IRQ,
    DEV_ERR_IRQ,
    DEV_REQ_IRQ,
    DEV_NUM_IRQS
};

// Message Packet (MSG) data for IRQ packets
struct MsgData
{
    uint32_t vector_idx;
    uint8_t  irq_type; // IrqType
    uint8_t  pad[3];
} __attribute__((packed));

typedef struct {
    uint32_t magic;     // magic number (e.g., 0xACDCBABE)
    uint16_t vector_id; // 16-bit vector ID
    uint8_t  int_type;  // 8-bit interrupt type (e.g., INTx, MSI, MSI-X)
    uint8_t  pad;       // Padding to 64-bit boundary
} __attribute__((aligned(8))) trigger_irq_pkt_t;

static_assert(sizeof(trigger_irq_pkt_t) == 8, "trigger_irq_pkt_t struct size mismatch");

inline bool operator==(uint8_t lhs, PktType rhs) {
    return lhs == static_cast<uint8_t>(rhs);
}

inline bool operator==(PktType lhs, uint8_t rhs) {
    return static_cast<uint8_t>(lhs) == rhs;
}

inline bool operator!=(uint8_t lhs, PktType rhs) {
    return lhs != static_cast<uint8_t>(rhs);
}

inline bool operator!=(PktType lhs, uint8_t rhs) {
    return static_cast<uint8_t>(lhs) != rhs;
}

template<typename T>
constexpr auto U8 (T e) { return static_cast<uint8_t>(e); }

struct Protocol
{
    Protocol() = default;
    Protocol(PktType type, uint64_t addr = 0, uint16_t count = 0, uint8_t tag = 0, uint16_t req_id = 0, uint8_t status = 0)
    {
        this->addr = addr;
        this->req_id = req_id;
        this->count = count;
        this->type = U8(type);
        this->tag = tag;
        this->status = status;
    }

    uint64_t addr;
    uint16_t req_id;
    uint16_t count;
    uint8_t  type;
    uint8_t  tag;
    uint8_t  status;
    uint8_t  pad;
    uint8_t  data[];
} __attribute__((packed));

#define PROTO_HDR_SIZE sizeof(Protocol)
static_assert(PROTO_HDR_SIZE == 16, "Protocol struct padding error");

template <typename T>
struct AlignedAllocator {
    using value_type = T;
    T* allocate(std::size_t n) {
        if (auto p = static_cast<T*>(std::aligned_alloc(8, n * sizeof(T))))
            return p;
        throw std::bad_alloc();
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
};

#define BUF_SIZE 4096
class Buf {
public:
    Buf(size_t size = BUF_SIZE)
    : len(0), p(nullptr), size(size), vec(size) {}

    Buf(void* ptr, size_t size)
    : len(0), p(static_cast<char*>(ptr)), size(size), vec(0) {}

    char* data() { return p ? p : vec.data(); }
    const char* data() const { return p ? p : vec.data(); }
    template <typename T>
    T* data(size_t offset = 0) {
        return reinterpret_cast<T*>(data() + offset);
    }
    template <typename T>
    const T* data(size_t offset = 0) const {
        return reinterpret_cast<const T*>(data() + offset);
    }
    size_t len;
private:
    char* p;
    size_t size;
public:
    std::vector<char, AlignedAllocator<char>> vec;
};

#ifndef NOP
#define NOP() do {} while(0)
#endif
const char* pktTypeToStr(PktType type);
const char* pktTypeToStr(uint8_t type);
const char* irqTypeToStr(IrqType type);
const char* irqTypeToStr(uint8_t type);
void addPrefix(const std::string& tag);
