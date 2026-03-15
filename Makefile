
PORT?=8080
CXX = gcc
CXXFLAGS = -Wall -Werror -std=c++17 -pedantic
SRC = $(wildcard *.c)
HDRS = $(wildcard *.h)
SERVER = server

.PHONY: build-server run-server clean check-format format

build-server:
	$(CXX) $(CXXFLAGS) $(SERVER).c -o $(SERVER).out

run-server:
	@./$(SERVER).out $(PORT)

clean:
	rm -f *.out

# NOTE: Will return an error if the code isn't formatted correctly. Useful for CI/CD
check-format:
	@clang-format --dry-run --Werror $(SRCS) $(HDRS)

format:
	@clang-format -i $(SRCS) $(HDRS)

