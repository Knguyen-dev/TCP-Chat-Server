# TCP Chat Server 

A Linux-native, non-blocking TCP messaging server in C++20. Implemented as a single-threaded event loop with non-blocking I/O this system achieves deterministic low latency and resource efficiency by enforcing zero-allocation hot paths, data-oriented memory layouts, and strict mechanical sympathy.

## Core Metrics 
- **Concurrency Ceiling:** Sustains up to 100,000 concurrent, idle connection slots within a single process footprint.
- **Fan-Out Throughput:** TCP broadcast latency for sending  262 byte message to 100,000 authenticated clients in ~900-1000ms over loopback. 
- **Microbenchmark Efficiency:** Google Benchmark suites confirm that the Structure of Arrays (SoA) data layout reduces L1 data cache miss rates to 9.22% when iterating over 100,000 connections. 

## Architectural Hightlights

**Single Thread Non-Blocking Event Loop**
Bypasses the resource exhaustion and context-switching bottlenecks of traditional Thread-per-Client models. The application impelments the idea of I/O multiplexing to efficiently handle up to 100,000 concurrent connections at once. This is implemented by Linux epoll API configured for Edge-Triggered (EPOLLET) mode.

**Minimal Heap Allocations (Upfront Arena Allocation)**
Dynamic heap allocations (malloc/free, new/delete) are entirely banned within the processing cycle. The server reserves a large chunk of heap memory for its internal connection tables up front. This pays the system call tax up front and keeps the connection table contiguous on heap.

**Data-Oriented Memory Alignment (SoA vs. AoS)**
To maximize cache-line information density and leverage hardware prefetching, the connection table is represented as a Structure of Arrays. We eliminate pointer chasing and layout memory to be hardware friendly, allowing for very fast traversals and efficient use of cpu cache lines.

**Network Protocol Design**
The custom application-layer binary protocol eliminates text-parsing overhead and keeps things fast.
- **7-Byte Header:** Fixed frame comprising Protocol Version (1B), Request/Response Type (1B), Status Response Code (1B), and Network-to-Host byte-ordered Payload Length (4B).
- **Tag-Length-Value (TLV) Payload:** Serialized byte-stream enabling robust schema evolution and order-independent parameter parsing.

## Installation
```bash
# g++, make, clang-format
sudo apt build-essential
sudo apt install clang-format

# Install sqlite
sudo apt install libsqlite3-dev

# Build and run the server
make run-server

# Build and run the client
make run-client

### Whilst in client terminal ###
# Register and log in on the server
/register <your_username> <your_password>
/login <your_username> <your_password>

# World and P2P broadcasts
/world <your_message>
/p2p <recipient_username> <your_message>

# a. Run Google Benchmark Tests
# b. Run unit tests
# c. Run integration tests
# d. Run load tests
make run-benchmark
make unit-tests
make integration-tests
make load-test
```

## Technical Documentation
For an exhaustive architectural analysis of this platform, please navigate to our [GitHub Wiki](https://github.com/Knguyen-dev/TCP-Chat-Server/wiki). The documentation includes in-depth chapters on:
- Detailed mathematical modeling of our end-to-end telemetry system.
- A post-mortem review mapping legacy cache-unfriendly behaviors to the remediated, low-latency framework.
- Extensive hardware counter profiles (L1 d-cache thrashing, branch mispredictions).

The GitHub Wiki also contains contribution rules. For anything else, such as future features and other writeups, see the **GitHub Discussion** tab.
