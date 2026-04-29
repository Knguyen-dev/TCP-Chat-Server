# TCP Chat Server 
This project implements a multi-user TCP chat server in C/C++. Clients connect to a TCP server, register an account, and then authenticate via the credentials they registered with. Now that the client is authenticated, they're able to send/receive messages to/from other authenticated clients. After authenticating, the client can do two types of messages:
- **World Broadcasts:** The client sends a message to all other authenticated connections on the server. This is essentially a TCP multicast.
- **Point-to-Point Broadcasts:** The client sends a message to one other authenticated connection on the server.

The server's main responsibility is to efficiently maintain and route messages for a large number of concurrent connections at once. To do this in a resource and I/O efficient manner, the server will be implemented using u single-thread event loop that uses non-blocking I/O when reading/writing data from/to a client. We'll use the `epoll()` syscall as our high-performance I/O readiness API, to let us know when we can read or write from/to a socket with minimum time wasted doing nothing. We'll implement a struct `conn_t` that represents the connection for each user in our program (Finite State Machine). We'll also leverage C++'s dynamic arrays to maintain a global table of connections and I/O bytes for each connection.

Other noteworthy things handled in this project:
- Application-layer binary protocol design
- C++ programming
- Network Programming
- Concurrency: I/O Multiplexing, Non-blocking I/O

## Project setup
```bash
# g++, make
sudo apt build-essentials 

# Install sqlite
sudo apt install libsqlite3-dev

# Build and run the server
make run-server

# Build and run the client
make run-client
```