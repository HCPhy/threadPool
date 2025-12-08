CXX = clang++
CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -pthread
OMP_FLAGS = -Xpreprocessor -fopenmp -lomp
INCLUDES = -I./include
# Attempt to auto-detect homebrew libomp location
BREW_PREFIX := $(shell brew --prefix libomp 2>/dev/null)
ifneq ($(BREW_PREFIX),)
    OMP_FLAGS += -I$(BREW_PREFIX)/include -L$(BREW_PREFIX)/lib
endif

BIN_DIR = bin

TARGET = $(BIN_DIR)/bench_inner_product
COMPUTE_TARGET = $(BIN_DIR)/bench_heavy_compute
STRESS_TEST = $(BIN_DIR)/stress_test

all: $(BIN_DIR) $(TARGET) $(COMPUTE_TARGET) $(STRESS_TEST)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(TARGET): test/bench_inner_product.cpp include/ms_jthread_pool.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OMP_FLAGS) $(INCLUDES) $< -o $@

$(COMPUTE_TARGET): test/bench_heavy_compute.cpp include/ms_jthread_pool.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OMP_FLAGS) $(INCLUDES) $< -o $@

$(STRESS_TEST): test/stress_test.cpp include/ms_jthread_pool.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf $(BIN_DIR)

run: $(TARGET)
	./$(TARGET)

run_compute: $(COMPUTE_TARGET)
	./$(COMPUTE_TARGET)

run_stress: $(STRESS_TEST)
	./$(STRESS_TEST)

.PHONY: all clean run run_stress run_compute
