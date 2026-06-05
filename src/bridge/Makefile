# Makefile for mocked PCIe AXI RAM EP simulation.
# This Makefile compiles the sources to create a PCIe Co-simulation Bridge executable.

BRIDGE_DIR := $(abspath $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))
COSIM_PATH := $(abspath $(BRIDGE_DIR)/../..)
LIBVFIO_PATH = $(COSIM_PATH)/third_party/lib/libvfio-user

BUILD_DIR = build

TARGET = $(BUILD_DIR)/pcie_bridge
ENABLE_TCP = 1
# SW Log Level (0-50): 0=NONE, 10=CRITICAL, 20=ERROR, 30=WARNING, 40=INFO, 50=DEBUG
LOG_LEVEL = 50
ENABLE_SW_LOGS = 1
ENABLE_PKT_LOGS = 1
ENABLE_WAIT_LIMIT = 1

# --- C++ Sources ---
CPP_SOURCES = \
	$(COSIM_PATH)/src/bridge/pcie_cosim_bridge.cpp \
	$(COSIM_PATH)/src/bridge/pcie_cosim_vfio_if.cpp \
	$(COSIM_PATH)/src/bridge/pcie_cosim_if.cpp \
	$(COSIM_PATH)/common/src/pcie_cosim_infra.cpp \
	$(COSIM_PATH)/common/src/socket_channel.cpp

TEST_CPP_SOURCES = \
	$(COSIM_PATH)/src/sim/pcie_cosim_test_driver.cpp

INC_FLAGS = \
	-I$(COSIM_PATH)/src/bridge \
	-I$(COSIM_PATH)/common/include \
	-I$(LIBVFIO_PATH)/lib \
	-I$(LIBVFIO_PATH)/include \

# Software enable flags
SEFLAGS = -DENABLE_SW_LOGS=$(ENABLE_SW_LOGS) -DLOG_LEVEL=$(LOG_LEVEL) -DENABLE_WAIT_LIMIT=$(ENABLE_WAIT_LIMIT) -DENABLE_PKT_LOGS=$(ENABLE_PKT_LOGS) -DENABLE_MOCK=1 -DENABLE_TCP=$(ENABLE_TCP)
CFLAGS = $(strip -fPIC -g -O3 -fcoroutines -Wall $(INC_FLAGS) $(SEFLAGS))
LDFLAGS += -L$(LIBVFIO_PATH)/build/lib -Wl,-rpath,$(LIBVFIO_PATH)/build/lib -lvfio-user

OBJECTS = $(addprefix $(BUILD_DIR)/, $(notdir $(CPP_SOURCES:.cpp=.o)))
TEST_OBJECTS = $(addprefix $(BUILD_DIR)/, $(notdir $(TEST_CPP_SOURCES:.cpp=.o)))

vpath %.cpp $(sort $(dir $(CPP_SOURCES) $(TEST_CPP_SOURCES)))

all: $(TARGET)

$(TARGET): clean $(OBJECTS)
	@$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

test: CFLAGS += -DENABLE_TEST=1
test: clean $(OBJECTS) $(TEST_OBJECTS)
	@$(CXX) $(OBJECTS) $(TEST_OBJECTS) -o $(TARGET) $(LDFLAGS)
	@echo "Running PCIe Bridge Test..."
	@./$(BUILD_DIR)/pcie_bridge

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f /tmp/*.sock

.PHONY: all clean test