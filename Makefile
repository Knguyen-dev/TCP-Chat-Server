# Variables
CC        = gcc
DEBUG    ?= 0
DEBUG_FLAGS =
ifeq ($(DEBUG),1)
DEBUG_FLAGS = -g -O0
else
DEBUG_FLAGS = -O2
endif
CFLAGS    = -Wall -Werror -std=c11 -pedantic -Iinclude $(DEBUG_FLAGS)
LDFLAGS = -lsqlite3
BUILD_DIR = build
SRC_DIR   = src
INC_DIR   = include
PORT     ?= 8080


# TODO: Update makefile for C++ compilation

# Header files (for dependency tracking)
HEADERS   = $(wildcard $(INC_DIR)/*.h)

# File groups: Shared code, client specific, and server specific code
SHARED_SRC  = $(SRC_DIR)/shared.c $(SRC_DIR)/protocol.c
SERVER_SRCS = $(SRC_DIR)/server.c $(SRC_DIR)/server_utils.c $(SRC_DIR)/db.c $(SHARED_SRC)
CLIENT_SRCS = $(SRC_DIR)/client.c $(SRC_DIR)/client_utils.c $(SHARED_SRC)
TEST_SRCS   = $(SRC_DIR)/client_tests.c $(SHARED_SRC)

# Object files (maps src/*.c to build/*.o); source to object files
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TEST_OBJS   = $(TEST_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

.PHONY: all build-server build-client run-server run-client test clean format \
        debug-server debug-client debug-test help

all: build-server build-client

# Builds the server and client
build-server: $(BUILD_DIR)/server.out
build-client: $(BUILD_DIR)/client.out
build-tests: $(BUILD_DIR)/client_tests.out

# Builds and runs server
run-server: build-server
	./$(BUILD_DIR)/server.out $(PORT)

# Builds and runs client 
run-client: build-client
	./$(BUILD_DIR)/client.out

# Runs Tests
test: $(BUILD_DIR)/client_tests.out
	@echo "Starting test server on port $(PORT)..."
	@./$(BUILD_DIR)/server.out $(PORT) & SERVER_PID=$$!; \
	sleep 1; \
	echo "Running tests..."; \
	./$(BUILD_DIR)/client_tests.out; TEST_RESULT=$$?; \
	echo "Stopping test server..."; \
	kill $$SERVER_PID 2>/dev/null || true; \
	exit $$TEST_RESULT

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

debug-test:
	@echo "Building tests with debug symbols..."
	@$(MAKE) DEBUG=1 clean
	@$(MAKE) DEBUG=1 build-tests
	@echo ""
	@echo "Start server in another terminal with: make run-server"
	@echo "Then run: gdb ./$(BUILD_DIR)/client_tests.out"
	@echo ""

# Link executables
$(BUILD_DIR)/server.out: $(SERVER_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Server built: $@"

$(BUILD_DIR)/client.out: $(CLIENT_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@
	@echo "Client built: $@"

$(BUILD_DIR)/client_tests.out: $(TEST_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@
	@echo "Tests built: $@"

# Compile object files - now depends on headers too
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

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
	@echo "  make debug-test       - Build tests with debug symbols"
	@echo "  make clean            - Remove build directory"
	@echo "  make format           - Format all source files"
	@echo ""
	@echo "Options:"
	@echo "  PORT=<port>          - Set server port (default: 8080)"
	@echo "  DEBUG=1              - Build with debug symbols and no optimization"