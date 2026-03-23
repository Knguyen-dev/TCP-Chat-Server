# Variables
CC        = gcc
CFLAGS    = -Wall -Werror -std=c11 -pedantic -Iinclude
BUILD_DIR = build
SRC_DIR   = src
PORT     ?= 8080

# File groups: Shared code, client specific, and server specific code
SHARED_SRC = $(SRC_DIR)/shared.c $(SRC_DIR)/protocol.c
SERVER_SRCS = $(SRC_DIR)/server.c $(SRC_DIR)/server_utils.c $(SHARED_SRC)
CLIENT_SRCS = $(SRC_DIR)/client.c $(SRC_DIR)/client_utils.c $(SHARED_SRC)

# Object files (maps src/*.c to build/*.o); source to object files
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

.PHONY: all build-server build-client run-server clean format

all: build-server build-client

# Link the server
build-server: $(BUILD_DIR)/server.out


# Builds the 'server.out' executable
# 1. Calculates all of the server object file names.
# 2. Create build directory.
# 3. Compiles server.out using all server object files calculated.
$(BUILD_DIR)/server.out: $(SERVER_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

# Builds the client executable.
build-client: $(BUILD_DIR)/client.out

# Builds the client.out executable
# 1. Builds all client object files.
# 2. Creates buidl directory if needed
# 3. Compiles and links
$(BUILD_DIR)/client.out: $(CLIENT_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

# Builds .o (object files) by compiling their corresponding .c files.
# 1. Create build directory if it doesn't exist
# 2. Compiles the .c files into object files.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Runs the TCP server, "server.out"
run-server: build-server
	./$(BUILD_DIR)/server.out $(PORT)

run-client: build-client
	./$(BUILD_DIR)/client.out

# Destroys build directory for cleanups.
clean:
	rm -rf $(BUILD_DIR)

# Formats our code
format:
	clang-format -i $(SRC_DIR)/*.c $(SRC_DIR)/*.h