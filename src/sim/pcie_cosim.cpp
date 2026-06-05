/**
 * @file pcie_cosim.cpp
 * @brief PCIe AXI RAM EP Verilated C++ Transactor.
 *
 * This file contains the Memory Write and Read handlers for the PCIe AXI RAM EP Verilated simulation.
 * It sets up the RTL simulation environment, including the Verilated model.
 * Note: Clock is driven manually from C++ to synchronize Verilog logic with PCIe protocol messages.
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
#include <iostream>

#include <verilated.h>          // Core Verilator definitions
#include "verilated_vcd_c.h"    // For .vcd waveform generation
#include "Vsim.h"               // Top-level class for sim_top.sv
#include "Vsim___024root.h"     // Access to root scope (required for public signals)

#include "protocol.h"
#include "log.h"

#ifndef ENABLE_MSI_TEST
#define ENABLE_MSI_TEST 0
#endif

#define BAR_SHIFT 56
#define PCIE_ADDR_MASK 0x00FFFFFFFFFFFFFFULL

#ifndef STR
#define STR2(x) #x
#define STR(x) STR2(x)
#endif

void sendAsync(PktType type, const uint32_t *data, uint16_t count);

static const int MAX_TRANSACTION_CYCLES = 100;
static Vsim* top = nullptr;
static VerilatedVcdC* tfp = nullptr;
static uint64_t vtime = 0;

static uint32_t mock_reg[0x1000/sizeof(uint32_t)]; // dummy 4KB BAR1 mem region

#if defined(ENABLE_TRACE) && (ENABLE_TRACE == 1)
static void TFP_OPEN()
{
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open(STR(WAVEFORM_NAME)".vcd");
}

#define TFP_DUMP do { tfp->dump(vtime); } while(0)
#define TFP_CLOSE tfp->close()
#else // !TRACE
#define TFP_OPEN() NOP()
#define TFP_DUMP NOP()
#define TFP_CLOSE NOP()
#endif
#define TFP_STEP(x) vtime += (x)

// Evaluate the Verilator model logic and dumps signal states to the trace file
static void topEval()
{
    top->eval();
    TFP_DUMP;
}

double sc_time_stamp()
{
    return vtime;
}

static void doReadMem(uint32_t addr, uint8_t tag, uint16_t req_id, uint32_t *data)
{
    auto* root = top->rootp;
    int n;

    uint32_t DW[3];
    DW[0] = 0x00000001; // MRd, Length 1
    DW[1] = (req_id << 16) | (tag << 8) | 0x0F;
    DW[2] = addr & 0xFFFFFFFC;

    LOG_INFO("PCIe-SIM","[RX] MRd | Addr: 0x%x Tag: 0x%02x, DW0=0x%x DW1=0x%x DW2=0x%x",
             addr, tag, DW[0], DW[1], DW[2]);

    // --- BEAT 0: Send to RTL Header in DW0 & DW1 ---
    root->s_axis_rx_tdata  = ((uint64_t)DW[1] << 32) | DW[0];
    root->s_axis_rx_tkeep  = 0xFF;
    root->beat_idx         = 0;
    root->s_axis_rx_tlast  = 0;
    root->s_axis_rx_tvalid = 1;
    top->eval();

    n = 0;
    while (!root->s_axis_rx_tready && n++ < MAX_TRANSACTION_CYCLES) {
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
        top->eval();
    }

    if (n >= MAX_TRANSACTION_CYCLES) {
        root->s_axis_rx_tvalid = 0;
        LOG_ERROR("PCIe-SIM","ERROR: Timeout waiting for s_axis_rx_tready (Beat 0)");
        return;
    }

    // --- BEAT 1: Send to RTL Data Address in DW2 ---
    root->s_axis_rx_tdata  = (uint64_t)DW[2];
    root->s_axis_rx_tkeep  = 0x0F;
    root->beat_idx         = 1;
    root->s_axis_rx_tlast  = 1;
    top->eval();

    n = 0;
    while (!root->s_axis_rx_tready && n++ < MAX_TRANSACTION_CYCLES) {
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
        top->eval();
    }
    if (n >= MAX_TRANSACTION_CYCLES) {
        root->s_axis_rx_tvalid = 0;
        LOG_ERROR("PCIe-SIM","ERROR: Timeout waiting for s_axis_rx_tready (Beat 1)");
        return;
    }
    // Let RTL Sim Consume Beat 1
    root->clk = 1; topEval(); TFP_STEP(50);
    root->clk = 0; topEval(); TFP_STEP(50);

    root->s_axis_rx_tvalid = 0;
    root->s_axis_rx_tlast  = 0;
    top->eval();

    // --- Wait for Completion (Reply from RTL) ---
    root->m_axis_tx_tready = 1;
    n = 0;
    // Wait for Beat 0 Completion (DW0 & DW1) from RTL
    while (!root->m_axis_tx_tvalid && n++ < MAX_TRANSACTION_CYCLES) {
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
        top->eval();
    }
    if (n >= MAX_TRANSACTION_CYCLES) {
        root->m_axis_tx_tready = 0;
        LOG_ERROR("PCIe-SIM","ERROR: Timeout waiting for CplD m_axis_tx_tvalid (Beat 0)");
        return;
    }
    if (root->m_axis_tx_tvalid) {
        // Consume Beat 0
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
        top->eval();

        // Wait for Beat 1 Completion (DW2 + Data) from RTL
        n = 0;
        while (!root->m_axis_tx_tvalid && n++ < MAX_TRANSACTION_CYCLES) {
            root->clk = 1; topEval(); TFP_STEP(50);
            root->clk = 0; topEval(); TFP_STEP(50);
            top->eval();
        }
        if (n >= MAX_TRANSACTION_CYCLES) {
            root->m_axis_tx_tready = 0;
            LOG_ERROR("PCIe-SIM","ERROR: Timeout waiting for CplD m_axis_tx_tvalid (Beat 1)");
            return;
        }
        if (root->m_axis_tx_tvalid) {
            uint32_t val = (uint32_t)(root->m_axis_tx_tdata >> 32);
            *data = val;
            LOG_INFO("PCIe-SIM", "Read Value: 0x%08x", val);
            root->clk = 1; topEval(); TFP_STEP(50);
            root->clk = 0; topEval(); TFP_STEP(50);
        }
    } else {
        LOG_ERROR("PCIe-SIM","ERROR: Timeout waiting for CplD Completion");
    }

    root->m_axis_tx_tready = 0;
    top->eval();
}

static void doReadMem(Protocol& pkt, Buf& dbuf)
{
    uint32_t addr = pkt.addr & PCIE_ADDR_MASK;
    uint16_t bar_id = pkt.addr >> BAR_SHIFT;
    uint16_t req_id = pkt.req_id;
    uint8_t  tag = pkt.tag;

    switch (bar_id) {
    case 0:
        for (int i = 0; i < pkt.count/sizeof(uint32_t); i++) {
            doReadMem(addr, tag++, req_id, dbuf.data<uint32_t>() + i);
            addr += sizeof(uint32_t);
        }
        dbuf.len = pkt.count;
        break;
    case 1:
        for (int i = 0; i < pkt.count/sizeof(uint32_t); i++) {
            *(dbuf.data<uint32_t>() + i) = mock_reg[i];
            addr += sizeof(uint32_t);
        }
        dbuf.len = pkt.count;
        break;
    }
}

static void doReadCfg(Protocol& pkt, Buf& dbuf)
{
    // TODO: Implementation for configuration space reads
}

void doRead(Protocol& pkt, Buf& dbuf)
{
    if (pkt.type == PktType::MEM_RD)
        doReadMem(pkt, dbuf);
    if (pkt.type == PktType::CFG_RD)
        doReadCfg(pkt, dbuf);
}

static void doWriteMem(uint32_t addr, uint8_t tag, uint16_t req_id, uint32_t data)
{
    auto* root = top->rootp;
    int n;

    // Prepare 3-DW Header + 1 DW Data
    uint32_t DW[4];
    DW[0] = (0x40 << 24) | (1); // MWr (3-DW), Length = 1 DW
    DW[1] = (req_id << 16) | (tag << 8) | 0x0F; // ReqID, Tag, BE
    DW[2] = addr & 0xFFFFFFFC;  // Target Address
    DW[3] = data;               // Payload Data

    LOG_INFO("PCIe-SIM","[RX] MWr | Addr: 0x%x Tag: 0x%02x, DW0=0x%x DW1=0x%x DW2=0x%x DW3=0x%x",
             addr, tag, DW[0], DW[1], DW[2], DW[3]);

    for (int i = 0; i < 2; i++) {
        // Beat 0: [DW1 | DW0]  (Fmt/Type/Tag)
        // Beat 1: [DW3 | DW2]  (Data/Addr)
        uint64_t data_64 = ((uint64_t)DW[i*2 + 1] << 32) | DW[i*2];

        root->s_axis_rx_tdata  = data_64;
        root->s_axis_rx_tkeep  = 0xFF;
        root->s_axis_rx_tvalid = 1;
        root->s_axis_rx_tlast  = (i == 1);
        root->beat_idx         = i;
        top->eval();

        n = 0;
        while (!root->s_axis_rx_tready && n++ < MAX_TRANSACTION_CYCLES) {
            root->clk = 1; topEval(); TFP_STEP(50);
            root->clk = 0; topEval(); TFP_STEP(50);
            top->eval();
        }

        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
        top->eval();

        if (n >= MAX_TRANSACTION_CYCLES) {
            root->s_axis_rx_tvalid = 0;
            LOG_ERROR("PCIe-SIM","ERROR: Timeout at Cpl Beat %d", i);
            return;
        }
    }

    root->s_axis_rx_tvalid = 0;
    root->s_axis_rx_tlast  = 0;
    top->eval();

    // Give RTL time to commit to RAM and clear Handshake
    for (int i = 0; i < 10; i++) {
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
        top->eval();
    }
}

static void doWriteMem(Protocol& pkt, Buf& dbuf)
{
    uint32_t addr = pkt.addr & PCIE_ADDR_MASK;
    uint16_t bar_id = pkt.addr >> BAR_SHIFT;
    uint16_t req_id = pkt.req_id;
    uint8_t  tag = pkt.tag;

    switch (bar_id) {
    case 0:
        for (int i = 0; i < pkt.count/sizeof(uint32_t); i++) {
#if ENABLE_MSI_TEST
            uint32_t data = *dbuf.data<uint32_t>(i*sizeof(uint32_t));
            if (addr == 0 && data == 0xACDCBABE) {
                uint32_t vector_idx = 0;
                LOG_INFO("PCIe-SIM","Sending Irq vector index %u", vector_idx);
                sendAsync(PktType::MSG, &vector_idx, sizeof(uint32_t));
                break;
            }
#endif
            doWriteMem(addr, tag++, req_id, *dbuf.data<uint32_t>(i*sizeof(uint32_t)));
            addr += sizeof(uint32_t);
        }
        break;
    case 1:
        for (int i = 0; i < pkt.count/sizeof(uint32_t); i++) {
            mock_reg[i] = *dbuf.data<uint32_t>(i*sizeof(uint32_t));
            addr += sizeof(uint32_t);
        }
        break;
    }
}

static void doWriteCfg(Protocol& pkt, Buf& dbuf)
{
    // TODO: Implementation for configuration space writes
}

void doWrite(Protocol& pkt, Buf& dbuf)
{
    if (pkt.type == PktType::MEM_WR)
        doWriteMem(pkt, dbuf);
    if (pkt.type == PktType::CFG_WR)
        doWriteCfg(pkt, dbuf);
}

static void launchRtl()
{
    auto* root = top->rootp;

    addPrefix("PCIe-SIM");
    std::cout << "Starting RTL Reset Sequence..." << std::endl;

    // Reset (Active Low)
    top->rst_n = 0;
    for (int i = 0; i < 10; i++) {
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
    }

    top->rst_n = 1; // Set RTL out of reset

    // Let internal pipelines settle
    for (int i = 0; i < 10; i++) {
        root->clk = 1; topEval(); TFP_STEP(50);
        root->clk = 0; topEval(); TFP_STEP(50);
    }

    addPrefix("PCIe-SIM");
    std::cout << "RTL Sim ready for TLPs. LTSSM: 0x"
              << std::hex << (int)root->ltssm_state << std::endl;
}

int initRtlSim()
{
    if (!top) {
        top = new Vsim;
        TFP_OPEN();
        launchRtl();
        return 0;
    }
    return -1;
}

void terminateRtlSim()
{
    auto* root = top->rootp;

    addPrefix("PCIe-SIM");
    std::cout << "Terminating RTL Sim..." << std::endl;

    root->terminate = 1;
    root->clk = 1; topEval(); TFP_STEP(50);

    top->final();
    TFP_CLOSE;
    delete top;
}

void idleRtlSim()
{
    auto* root = top->rootp;
    root->clk = 1; topEval(); TFP_STEP(50);
    root->clk = 0; topEval(); TFP_STEP(50);
    usleep(100);
}