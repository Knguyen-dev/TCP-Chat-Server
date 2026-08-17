.PHONY: build debug asan format test clean
BUILD_DIR ?= build
DEBUG_DIR ?= build/debug
ASAN_DIR  ?= build/asan

PORT ?= 8080
IS_TEST ?= 0
ENABLE_LOGGING ?= 1

install:
	sudo apt update
	sudo apt install build-essential && clang-format && iwyu && ninja-build && libsqlite3-dev

# NOTE: --jobs allows CMake to use multiple cores for builds
# to parallelize and speed them up. 
# -s: Silent mode suppresses raw compiler command printing.
# --no-print-directory: Removes all gmake entering/leaving directory
build:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR)

debug:
	cmake -S . -B $(DEBUG_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(DEBUG_DIR)

asan:
	cmake -S . -B $(ASAN_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
	cmake --build $(ASAN_DIR)

benchmark: build
	sudo perf stat ./$(BUILD_DIR)/TCPChatServer_benchmark

run-server: build
	./$(BUILD_DIR)/TCPChatServer_server $(PORT) $(IS_TEST) $(ENABLE_LOGGING)

run-client: build
	./$(BUILD_DIR)/TCPChatServer_client 

gdb-server: debug
	gdb --args ./$(DEBUG_DIR)/TCPChatServer_server 8080 1 1

run-asan-server: asan
	./$(ASAN_DIR)/TCPChatServer_server 8080 1 1

format:
	find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

clean:
	rm -rf build