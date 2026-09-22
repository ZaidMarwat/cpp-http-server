CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -pthread
BUILD    := build

LIB_SRCS := src/http_types.cpp src/request_parser.cpp src/router.cpp src/server.cpp
LIB_OBJS := $(LIB_SRCS:src/%.cpp=$(BUILD)/%.o)

all: server $(BUILD)/tests $(BUILD)/bench

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

server: src/main.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/tests: tests/test_main.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -Isrc $^ -o $@

$(BUILD)/bench: bench/bench.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

check: $(BUILD)/tests
	./$(BUILD)/tests

# Rebuild everything with AddressSanitizer + UBSan, or ThreadSanitizer, and run the tests.
check-asan:
	$(MAKE) clean
	$(MAKE) check CXXFLAGS="-std=c++17 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer"
	$(MAKE) clean

check-tsan:
	$(MAKE) clean
	$(MAKE) check CXXFLAGS="-std=c++17 -O1 -g -pthread -fsanitize=thread"
	$(MAKE) clean

bench: $(BUILD)/bench

clean:
	rm -rf $(BUILD) server server.dSYM

-include $(LIB_OBJS:.o=.d)

.PHONY: all check check-asan check-tsan bench clean
