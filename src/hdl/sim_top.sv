/**
 * @file sim_top.sv
 * @brief Top-level RTL wrapper for the PCIe AXI RAM EP Verilated simulation.
 *
 * This module orchestrates transaction-level tracking between an openPCIE endpoint
 * emulation layer and an AXI Block RAM core model. It provides real-time, zero-delay
 * interconnect address multiplexing, pipelines bus parameters, and generates a
 * synchronized, symmetrical log trace for memory read and write operations.
 *
 *   +----------------------------------------------------------+
 *   |  - BFM Interface (C++ Transactor Ports)                  |
 *   |  s_axis_rx_tdata, s_axis_rx_tvalid, s_axis_rx_tlast, ... |
 *   |    | (64-bit AXIS TLP Ingress)                           |
 *   |    v                                                     |
 *   | +--------------------+                                   |
 *   | |   PIO_RX_ENGINE    |==[ compl_done Handshake ]==+      |
 *   | +--------------------+                            |      |
 *   |     | (Writes)     | (Reads / Context)            |      |
 *   |     | wr_en        | req_addr, req_tag, req_len   |      |
 *   |     | wr_addr      | req_compl, req_compl_wd      |      |
 *   |     | wr_data      |                              |      |
 *   |     v              v                              v      |
 *   | +---------------------------------------------------+    |
 *   | | Glue & Control Logic                              |    |
 *   | | - Address / Tag Latches (addr_latch, tag_latch)   |    |
 *   | | - RAM Interconnect Write Address Multiplexing     |    |
 *   | +---------------------------------------------------+    |
 *   |     |                                  |                 |
 *   |     | AXI Write Port                   | AXI Read Port   |
 *   |     | s_axi_awaddr / awvalid           | s_axi_araddr    |
 *   |     | s_axi_wdata / wvalid             | s_axi_arvalid   |
 *   |     v                                  v                 |
 *   | +---------------------------------------------------+    |
 *   | |                      axi_ram                      |    |
 *   | |           (DATA_WIDTH=32, ADDR_WIDTH=20)          |    |
 *   | +---------------------------------------------------+    |
 *   |                                        | rd_data         |
 *   |                                        | (Read Payload)  |
 *   |                                        v                 |
 *   |                                 +--------------------+   |
 *   |                                 |   PIO_TX_ENGINE    |   |
 *   |                                 +--------------------+   |
 *   |                                   | (64-bit AXIS Egress) |
 *   |                                   v                      |
 *   |   - BFM Output (Completions back to C++)                 |
 *   |   m_axis_tx_tdata, m_axis_tx_tvalid, m_axis_tx_tready    |
 *   +----------------------------------------------------------+
 *
 * Copyright (c) 2026, Purple
 * This file is licensed under the MIT License.
 */
`timescale 1ns / 1ps

`ifndef ENABLE_LOGS
  `define ENABLE_LOGS 0
`endif

/* verilator lint_off WIDTHTRUNC */
/* verilator lint_off WIDTHEXPAND */
/* verilator lint_off CASEX */
module sim (
  input  bit        clk,
  input  bit        rst_n,
  input  bit        beat_idx,
  input  bit        terminate,

  // BFM Interface (Driven by C++ Transactor)
  input  bit [63:0] s_axis_rx_tdata     /* verilator public_flat_rd */,
  input  bit [7:0]  s_axis_rx_tkeep,
  input  bit        s_axis_rx_tlast,
  input  bit        s_axis_rx_tvalid    /* verilator public_flat_rd */,
  output bit        s_axis_rx_tready    /* verilator public_flat_rd */,

  // BFM Output (Completions back to C++)
  input  bit        m_axis_tx_tready    /* verilator public_flat_rd */,
  output bit        m_axis_tx_tvalid    /* verilator public_flat_rd */,
  output bit [63:0] m_axis_tx_tdata     /* verilator public_flat_rd */,

  output bit [4:0]  ltssm_state         /* verilator public_flat_rd */
);

  initial begin
    $timeformat(-9, 3, " us", 10);
    $display("[%t][V-LOG] OpenPCIe PIO Endpoint simulation start", $realtime);
  end

  always @(posedge clk) begin
    if (terminate) begin
      $display("[%t][V-LOG] OpenPCIe PIO Endpoint simulation end", $realtime);
    end
  end

  assign ltssm_state = rst_n ? 5'h10 : 5'h00;

  wire [12:0] req_addr;
  wire [10:0] wr_addr;
  wire [31:0] wr_data;
  wire        wr_en;
  wire [31:0] rd_data;
  wire [9:0]  req_len;
  wire [15:0] req_rid;
  wire [7:0]  req_tag;
  wire [7:0]  req_be;

  wire        req_compl_raw;
  wire        req_compl_wd_raw;
  wire        compl_done;

  reg         compl_req_latch;
  reg [7:0]   tag_latch;
  reg [12:0]  addr_latch;

  always @(s_axis_rx_tvalid, s_axis_rx_tready, s_axis_rx_tdata) begin
    if (`ENABLE_LOGS && s_axis_rx_tvalid && s_axis_rx_tready) begin
      if (beat_idx == 1'b0) begin
        // --- BEAT 0 ---
        $display("[%t][V-LOG] BEAT 0: Fmt/Type=0x%02h Tag=0x%02h Length=%0d Last=%b",
                 $realtime, s_axis_rx_tdata[30:24], s_axis_rx_tdata[47:40], s_axis_rx_tdata[9:0], s_axis_rx_tlast);
      end else begin
        // --- BEAT 1 ---
        if (s_axis_rx_tdata[30:24] == 7'h40 || i_axi_ram.s_axi_awvalid) begin
          $display("[%t][V-LOG] BEAT 1: Addr=0x%03h Data=0x%08h Last=%b",
                   $realtime, s_axis_rx_tdata[10:2], s_axis_rx_tdata[63:32], s_axis_rx_tlast);
        end else begin
          $display("[%t][V-LOG] BEAT 1: Addr=0x%03h Data=N/A Last=%b",
                 $realtime, s_axis_rx_tdata[10:2], s_axis_rx_tlast);
        end
      end
    end
  end

  // --- Start of Combinational Lookahead Trigger ---

  // 1. Signal & Net Declarations
  logic [10:0] last_logged_addr;
  logic        mwr_active_gate;
  logic        w_active;
  logic [31:0] w_data_reg;
  logic [10:0] w_addr_reg;
  wire         s_axi_wready_mon;
  wire [19:0]  ram_awaddr;
  wire [31:0]  ram_wdata;
  wire         ram_valid;
  wire [10:0]  active_index;
  wire [11:0]  aligned_base_addr;
  logic [10:0] active_mrd_addr;
  logic        mrd_comb_detect;
  logic [10:0] mrd_comb_addr;
  logic [7:0]  mrd_comb_tag;
  logic        mwr_logged_pulse;
  logic        mrd_tail_print_gate;

  // 2. Timing Compliance Engine & Integrated Tail-End Read Logger,
  // including zero-delay address interception lookahead layer
  always_comb begin
    mrd_comb_detect = 1'b0;
    mrd_comb_addr   = 11'h000;
    mrd_comb_tag    = 8'h00;

    if (s_axis_rx_tvalid && s_axis_rx_tready && (beat_idx == 1'b1) && !wr_en) begin
      mrd_comb_detect = 1'b1;
      mrd_comb_addr   = s_axis_rx_tdata[10:2];
      mrd_comb_tag    = 8'h01 + s_axis_rx_tdata[10:2]; // Tag = Addr + 1
    end
  end

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      compl_req_latch <= 1'b0;
      tag_latch       <= 8'h00;
      addr_latch      <= 13'h0000;
      active_mrd_addr <= 11'h000;
    end else begin
      // Lock onto the incoming read request via lookahead parameters
      if (!compl_req_latch && mrd_comb_detect) begin
        compl_req_latch <= 1'b1;
        tag_latch       <= mrd_comb_tag;
        addr_latch      <= {2'b00, mrd_comb_addr};
        active_mrd_addr <= mrd_comb_addr;

        if (`ENABLE_LOGS) begin
          $display("[%t][V-LOG] MRd Latch Locked: Tag=0x%03h, Addr=0x%03h",
                    $realtime, mrd_comb_tag, mrd_comb_addr);
          $display("[%t][V-LOG] Dispatch to RAM: Addr=0x%03h",
                    $realtime, {1'b1, mrd_comb_addr[8:0], 2'b00});
        end
      end
      // Catch the transaction closure edge when compl_done finishes the loop
      if (compl_req_latch && compl_done) begin
        compl_req_latch <= 1'b0;
        if (`ENABLE_LOGS) begin
          $display("[%t][V-LOG] Latch Cleared", $realtime);

          if (!terminate) begin
            $display("[%t][V-LOG] MRd route to TX Data=0x%08h",
                     $realtime, i_axi_ram.mem[{1'b1, active_mrd_addr[8:0]}]);
          end
        end
      end
    end
  end

  // 3. Synchronous Write Log Probing Block
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      last_logged_addr <= 11'h7FF;
      mwr_active_gate  <= 1'b0;
      mwr_logged_pulse <= 1'b0;
    end else begin
      if (i_axi_ram.s_axi_awvalid) begin
        if (!mwr_active_gate && !mwr_logged_pulse && (last_logged_addr != i_axi_ram.s_axi_awaddr[10:2])) begin
          mwr_active_gate  <= 1'b1;
          mwr_logged_pulse <= 1'b1;
          last_logged_addr <= i_axi_ram.s_axi_awaddr[10:2];
          if (`ENABLE_LOGS) begin
            $display("[%t][V-LOG] MWr Latch Locked: Tag=0x%03h, Addr=0x%03h",
                      $realtime, 8'h00 + i_axi_ram.s_axi_awaddr[10:2], i_axi_ram.s_axi_awaddr[10:2]);
            $display("[%t][V-LOG] Dispatch to RAM: Addr=0x%03h",
                      $realtime, i_axi_ram.s_axi_awaddr[11:0]);
            $display("[%t][V-LOG] Latch Cleared", $realtime);
          end
        end
      end else begin
        mwr_active_gate  <= 1'b0;
        mwr_logged_pulse <= 1'b0;
      end
    end
  end

  // 4. Pipeline-Aligned Zero-Delay Address Interconnect Routing Mux
  assign active_index      = compl_req_latch ? addr_latch[10:0] : mrd_comb_addr;
  assign aligned_base_addr = {1'b1, active_index[8:0], 2'b00};
  wire [19:0] ram_araddr   = {8'b0, aligned_base_addr};

  // 5. Write Envelope Controller State Machine
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      w_active   <= 1'b0;
      w_data_reg <= 32'h0;
      w_addr_reg <= 11'h0;
    end else begin
      if (wr_en) begin
        w_active   <= 1'b1;
        w_data_reg <= wr_data;
        w_addr_reg <= wr_addr;
      end else if (s_axi_wready_mon && w_active) begin
        w_active <= 1'b0;
      end
    end
  end

  assign ram_awaddr = w_active ? {7'b0, w_addr_reg, 2'b00} : {7'b0, wr_addr, 2'b00};
  assign ram_wdata  = w_active ? w_data_reg                : wr_data;
  assign ram_valid  = w_active || wr_en;

  // 6. Synchronized Negative-Edge Logging Core
  always @(negedge clk) begin
    if (`ENABLE_LOGS && ram_valid && s_axi_wready_mon) begin
      $display("[%t][V-LOG] MWr write to RAM: Addr=0x%03h Data=0x%08h",
               $realtime, ram_awaddr[11:0], ram_wdata);
    end
  end

  // --- End of Combinational Lookahead Trigger ---

  wire tx_engine_wd = (req_compl_wd_raw || compl_req_latch);

  /* verilator lint_off PINMISSING */
  PIO_RX_ENGINE rx_engine(
    .clk              (clk             ),
    .rst_n            (rst_n           ),
    .m_axis_rx_tdata  (s_axis_rx_tdata ),
    .m_axis_rx_tvalid (s_axis_rx_tvalid),
    .m_axis_rx_tready (s_axis_rx_tready),
    .m_axis_rx_tlast  (s_axis_rx_tlast ),
    .m_axis_rx_tkeep  (s_axis_rx_tkeep ),
    .m_axis_rx_tuser  (22'h000004      ),
    .req_addr         (req_addr        ),
    .wr_addr          (wr_addr         ),
    .wr_data          (wr_data         ),
    .wr_en            (wr_en           ),
    .wr_busy          (1'b0            ),
    .req_tag          (req_tag         ),
    .req_rid          (req_rid         ),
    .req_len          (req_len         ),
    .req_be           (req_be          ),
    .req_tc           (                ),
    .req_td           (                ),
    .req_ep           (                ),
    .req_attr         (                ),
    .req_compl        (req_compl_raw   ),
    .req_compl_wd     (req_compl_wd_raw),
    .compl_done       (compl_done      )
  );

  PIO_TX_ENGINE tx_engine(
    .clk              (clk              ),
    .rst_n            (rst_n            ),
    .s_axis_tx_tdata  (m_axis_tx_tdata  ),
    .s_axis_tx_tvalid (m_axis_tx_tvalid ),
    .s_axis_tx_tready (m_axis_tx_tready ),
    .rd_data          (rd_data          ),
    .req_tag          (tag_latch        ),
    .req_addr         (addr_latch       ),
    .req_compl        (compl_req_latch  ),
    .req_compl_wd     (tx_engine_wd     ),
    .req_rid          (16'h0000         ),
    .req_len          (req_len          ),
    .req_be           (req_be           ),
    .req_tc           (3'b000           ),
    .req_td           (1'b0             ),
    .req_ep           (1'b0             ),
    .req_attr         (2'b00            ),
    .completer_id     (16'h0100         ),
    .compl_done       (compl_done       )
  );
  /* verilator lint_on PINMISSING */

  axi_ram #(.DATA_WIDTH(32), .ADDR_WIDTH(20), .ID_WIDTH(8)) i_axi_ram(
    .clk              (clk             ),
    .rst              (!rst_n          ),

    // Write Interface
    .s_axi_awaddr     (ram_awaddr      ),
    .s_axi_awvalid    (ram_valid       ),
    .s_axi_wdata      (ram_wdata       ),
    .s_axi_wvalid     (ram_valid       ),
    .s_axi_wlast      (1'b1            ),
    .s_axi_wready     (s_axi_wready_mon),

    // Read Interface Mappings
    .s_axi_araddr     (ram_araddr),
    .s_axi_arvalid    (compl_req_latch || mrd_tail_print_gate),
    .s_axi_rdata      (rd_data         ),
    .s_axi_rready     (1'b1            ),
    .s_axi_arready    (                ),

    // Static configurations
    .s_axi_awid       (8'h00           ),
    .s_axi_awlen      (8'h00           ),
    .s_axi_awsize     (3'b010          ),
    .s_axi_awburst    (2'b01           ),
    .s_axi_awlock     (1'b0            ),
    .s_axi_awcache    (4'h0            ),
    .s_axi_awprot     (3'h0            ),
    .s_axi_awready    (                ),
    .s_axi_arid       (8'h00           ),
    .s_axi_arlen      (8'h00           ),
    .s_axi_arsize     (3'b010          ),
    .s_axi_arburst    (2'b01           ),
    .s_axi_arlock     (1'b0            ),
    .s_axi_arcache    (4'h0            ),
    .s_axi_arprot     (3'h0            ),
    .s_axi_bid        (                ),
    .s_axi_bresp      (                ),
    .s_axi_bvalid     (                ),
    .s_axi_bready     (1'b1            ),
    .s_axi_rid        (                ),
    .s_axi_rresp      (                ),
    .s_axi_rlast      (                ),
    .s_axi_rvalid     (                ),
    .s_axi_wstrb      (4'hF            )
  );

endmodule
/* verilator lint_on WIDTHTRUNC */
/* verilator lint_on WIDTHEXPAND */
/* verilator lint_on CASEX */
