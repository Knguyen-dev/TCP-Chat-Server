# TCP Chat Server 
This project implements a multi-user TCP chat server in C/C++. Clients connect to a TCP server, register an account, and then authenticate. After being authenticated, they're able to send/receive messages to/from other authenticated clients. The client can send two types of messages (aka 'broadcasts'):
- **World Broadcasts:** The client sends a message to all other authenticated connections on the server. This is essentially a TCP multicast.
- **Point-to-Point Broadcasts:** The client sends a message to one other authenticated connection on the server.

## Architecture

**Server**
The server's main responsibility is to efficiently maintain and route messages for a large number of concurrent connections. The server will be implement a single-thread event loop that uses non-blocking I/O when reading/writing data from/to a client. We'll use the `epoll()` syscall as our high-performance I/O readiness API, notifying us of when we can read or write from/to a socket with minimum time wasted doing nothing. We'll implement a struct `conn_t` that represents the connection for each user in our program (Finite State Machine). We'll also leverage C++'s dynamic arrays to maintain a global table of connections and I/O bytes for each connection.

**Client**
The client will need to handle two distinct events:
- Keyboard input from the user.
- Asynchronous broadcast notifications (messages from other users) that are sent via the server.

To handle both, we'll again implement I/O multiplexing. We don't really need `epoll()` for this, so we'll use a simpler version called `poll()`. Though this is still in its early testing stages, so the client's architecture might change. It seems most TUI/GUI libraries opt to use two separate threads in situations like these. One thread for updating UI only, and the other thread for posting events to the UI layer or handling other background processes. We'll keep the I/O multiplexing approach for the client for now, but later it may switch if we run into roadblocks.

## MISC
Other noteworthy things handled in this project:
- Application-layer binary protocol design
- C++ programming
- Network Programming
- Goals: Aiming for high performance metrics and load testing once it's finished. For example, can it hold 10k+ users concurrently? What's the latency of sending a world-broadcast or p2p broadcast when there are so many users to look through? Any other fancy optimizations?

**Note:** We won't be leveraging a GUI/TUI library such as Ncurses or FTXUI. That's out of scope as the main focus is low-level systems programming and performant code.

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
