# Variables
CCX        = g++
DEBUG    ?= 0
DEBUG_FLAGS =
ifeq ($(DEBUG),1)
	DEBUG_FLAGS = -g -O0
else
	DEBUG_FLAGS = -O2
endif
CXXFLAGS    = -Wall -Werror -std=c++20 -pedantic -Iinclude $(DEBUG_FLAGS)
LDFLAGS = -lsqlite3
BUILD_DIR = build
SRC_DIR   = src
INC_DIR   = include
PORT     ?= 8080

# Flag to indicate if we're running tests (used to conditionally compile test code)
IS_TEST ?= 0
ENABLE_LOGGING ?= 0

# Header files (for dependency tracking)
HEADERS   = $(wildcard $(INC_DIR)/*.hpp)

# File groups: Shared code, client specific, and server specific code
SHARED_SRC  = $(SRC_DIR)/shared.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/logger.cpp 
SERVER_SRCS = $(SRC_DIR)/server.cpp $(SRC_DIR)/server_utils.cpp $(SRC_DIR)/db.cpp $(SHARED_SRC)
CLIENT_SRCS = $(SRC_DIR)/client.cpp $(SRC_DIR)/client_utils.cpp $(SHARED_SRC)

# Object files (maps src/*.c to build/*.o); source to object files
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

.PHONY: all build-server build-client run-server run-client test clean format \
        debug-server debug-client debug-test help

all: build-server build-client

# Builds the server and client
build-server: $(BUILD_DIR)/server.out
build-client: $(BUILD_DIR)/client.out

# Builds and runs server
# make run-server PORT=8080 IS_TEST=1 for test mode
run-server: build-server
	./$(BUILD_DIR)/server.out $(PORT) $(IS_TEST) $(ENABLE_LOGGING)


# Builds and runs client 
run-client: build-client
	./$(BUILD_DIR)/client.out

# Builds and runs the test suite
# 1. Run test server in the background; wait until it's fully up before running tests.
# 2. Run auth integration tests, alongside any other tests
# The '-' means that even if the test binary returns a non-zero exit code, the Makefile won't stop executing.
# This allows us to run all tests and then clean up the server, even if some tests fail.
# 3. Cleanup testing server
test: build-server $(BUILD_DIR)/test_auth.out
	@echo "Starting test server (in background)..."
	./$(BUILD_DIR)/server.out $(PORT) 1 1 &
	sleep 2

	@echo "Running auth integration tests..."
	./$(BUILD_DIR)/test_auth.out

	@echo "Cleaning up server..."
	-pkill -f "./$(BUILD_DIR)/server.out"

# Compiles the test binary if source files changed.
$(BUILD_DIR)/test_auth.out: test/integration/test_auth.cpp src/protocol.cpp src/logger.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Link object files to create server executable.
$(BUILD_DIR)/server.out: $(SERVER_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Server built: $@"

# Link object files to create client executable
$(BUILD_DIR)/client.out: $(CLIENT_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Client built: $@"

# Compile object files: Also depends on header files existing
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@ $(LDFLAGS)

# Debug targets - properly rebuild with debug flags
debug-server:
	@echo "Building server with debug symbols..."
	@$(MAKE) DEBUG=1 clean
	@$(MAKE) DEBUG=1 build-server
	gdb --args ./$(BUILD_DIR)/server.out $(PORT)

debug-client:
	@echo "Building client with debug symbols..."
	@$(MAKE) DEBUG=1 clean
	@$(MAKE) DEBUG=1 build-client
	gdb ./$(BUILD_DIR)/client.out

# Cleanup 
clean:
	rm -rf $(BUILD_DIR)
	@echo "Build directory cleaned"

# Format code
format:
	clang-format -i $(SRC_DIR)/*.c $(INC_DIR)/*.h
	@echo "Code formatted"

# Help target
help:
	@echo "Available targets:"
	@echo "  make                  - Build server and client"
	@echo "  make build-server     - Build server only"
	@echo "  make build-client     - Build client only"
	@echo "  make run-server       - Build and run server (PORT=8080)"
	@echo "  make run-client       - Build and run client"
	@echo "  make test             - Run automated tests (starts/stops server)"
	@echo "  make debug-server     - Build with debug symbols and launch gdb"
	@echo "  make debug-client     - Build with debug symbols and launch gdb"
	@echo "  make clean            - Remove build directory"
	@echo "  make format           - Format all source files"
	@echo ""
	@echo "Options:"
	@echo "  PORT=<port>          - Set server port (default: 8080)"
	@echo "  DEBUG=1              - Build with debug symbols and no optimization"