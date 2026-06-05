# Makefile for PCIe AXI RAM EP Verilated simulation.
# This Makefile compiles the sources to create a Verilated PCIe RTL simulation executable.

COSIM_PATH := $(abspath $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))
LIBVFIO_PATH = $(COSIM_PATH)/third_party/lib/libvfio-user
OPEN_PCIE_PATH = $(COSIM_PATH)/third_party/hw/openPCIE
OPEN_PCIE_EP_PATH = $(OPEN_PCIE_PATH)/2.amd-rtl-with-Vivado-build/1.EP.amd/src

BUILD_DIR = build

TARGET = pcie_sim
VERILATOR = verilator
NUM_THREADS = 4
TOP_MODULE = sim
ENABLE_TRACE = 0
WAVEFORM_NAME = sim
# SW Log Level (0-50): 0=NONE, 10=CRITICAL, 20=ERROR, 30=WARNING, 40=INFO, 50=DEBUG
LOG_LEVEL = 40
ENABLE_SW_LOGS = 1
ENABLE_HW_LOGS = 1
ENABLE_PKT_LOGS = 0
ENABLE_WAIT_LIMIT = 1 # Use 1 to enable a wait limit for the PCIe Sim's communication channel establishment

# --- HDL Sources ---
HDL_SOURCES = \
	$(OPEN_PCIE_EP_PATH)/PIO_RX_ENGINE.v \
	$(OPEN_PCIE_EP_PATH)/PIO_TX_ENGINE.v \
	$(COSIM_PATH)/src/hdl/axi_ram.v \
	$(COSIM_PATH)/src/hdl/sim_top.sv

# --- C++ Sources ---
CPP_SOURCES = \
	$(COSIM_PATH)/src/bridge/pcie_cosim_bridge.cpp \
	$(COSIM_PATH)/src/bridge/pcie_cosim_vfio_if.cpp \
	$(COSIM_PATH)/src/bridge/pcie_cosim_if.cpp \
	$(COSIM_PATH)/common/src/pcie_cosim_infra.cpp \
	$(COSIM_PATH)/common/src/socket_channel.cpp

COSIM_CPP_SOURCES= \
	$(COSIM_PATH)/src/sim/pcie_cosim.cpp

TEST_CPP_SOURCES = \
	$(COSIM_PATH)/src/sim/pcie_cosim_test_driver.cpp

INC_FLAGS = \
	-I$(COSIM_PATH) \
	-I$(COSIM_PATH)/src/bridge \
	-I$(COSIM_PATH)/common/include \
	-I$(LIBVFIO_PATH)/lib \
	-I$(LIBVFIO_PATH)/include \

# HEFLAGS - Hardware Enable Flags
# SEFLAGS - Software Enable Flags
# NFLAGS - Normal Flags
# DFLAGS - Debug Flags
# VFLAGS - Verilation Flags
# CFLAGS - Compilation Flags
# LDFLAGS - Linker Flags
HEFLAGS = -DENABLE_LOGS=$(ENABLE_HW_LOGS)
SEFLAGS = -DENABLE_SW_LOGS=$(ENABLE_SW_LOGS) -DLOG_LEVEL=$(LOG_LEVEL) -DENABLE_WAIT_LIMIT=$(ENABLE_WAIT_LIMIT) -DENABLE_PKT_LOGS=$(ENABLE_PKT_LOGS) -DENABLE_MSI_TEST=1
NFLAGS = -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-CASEX -Wno-COMBDLY -Wno-UNOPTFLAT --no-timing -Wno-fatal
DFLAGS = --converge-limit 2000 --x-assign unique --x-initial unique -DVL_DEBUG
VFLAGS = $(strip -I$(OPEN_PCIE_EP_PATH) $(NFLAGS) $(HEFLAGS))
CFLAGS = $(strip -fPIC -g -O3 -fcoroutines $(INC_FLAGS) $(SEFLAGS))
LDFLAGS = -lrt -lpthread
LDFLAGS += -L$(LIBVFIO_PATH)/build/lib -Wl,-rpath,$(LIBVFIO_PATH)/build/lib -lvfio-user

all: $(TARGET)

$(TARGET): clean $(HDL_SOURCES) $(CPP_SOURCES) $(COSIM_CPP_SOURCES)
	$(VERILATOR) -cc --exe -j 24 \
		--top-module $(TOP_MODULE) \
		$(HDL_SOURCES) \
		$(CPP_SOURCES) \
		$(COSIM_CPP_SOURCES) \
		$(INC_FLAGS) \
		--threads $(NUM_THREADS) \
		--Mdir $(BUILD_DIR) \
		$(VFLAGS) \
		-CFLAGS "$(CFLAGS)" \
		-LDFLAGS "$(strip $(LDFLAGS))" \
		-o $(TARGET) \
		--build 2>&1 | tr -s ' '

trace: clean
	$(VERILATOR) -cc --exe -j 24 \
		--trace \
		--top-module $(TOP_MODULE) \
		$(HDL_SOURCES) \
		$(CPP_SOURCES) \
		$(COSIM_CPP_SOURCES) \
		$(INC_FLAGS) \
		--threads $(NUM_THREADS) \
		--Mdir $(BUILD_DIR) \
		$(VFLAGS) \
		-CFLAGS "$(CFLAGS) -DENABLE_TRACE=1 -DWAVEFORM_NAME=$(WAVEFORM_NAME)" \
		-LDFLAGS "$(strip $(LDFLAGS))" \
		-o $(TARGET) \
		--build 2>&1 | tr -s ' '

test: clean
	$(VERILATOR) -cc --exe -j 24 \
		--trace \
		--top-module $(TOP_MODULE) \
		$(HDL_SOURCES) \
		$(CPP_SOURCES) \
		$(COSIM_CPP_SOURCES) \
		$(TEST_CPP_SOURCES) \
		$(INC_FLAGS) \
		--threads $(NUM_THREADS) \
		--Mdir $(BUILD_DIR) \
		$(VFLAGS) \
		-CFLAGS "$(CFLAGS) -DENABLE_TEST=1 -DENABLE_TRACE=$(ENABLE_TRACE) -DWAVEFORM_NAME=$(WAVEFORM_NAME)" \
		-LDFLAGS "$(strip $(LDFLAGS))" \
		-o $(TARGET) \
		--build 2>&1 | tr -s ' '
	@echo "Running PCIe Simulation Test..."
	@./$(BUILD_DIR)/pcie_sim -v

wave:
	gtkwave sim_waveform.gtkw &

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f /tmp/*.sock

.PHONY: all trace clean wave