#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H
#include "shared.hpp"

#include <sys/epoll.h> // for epoll readiness API

#include <fstream>
#include <malloc.h>
#include <string>
#include <unistd.h>

#define LISTENQ 100
#define MAX_EVENTS 64

/**
 * ENUM representing the main states that a given connection will be in.
 */
enum class ConnState { READING, WRITING, CLOSING };

/**
 * Struct of Arrays memory layout that's used in
 * the hot loops of WORLD/P2P broadcasts.
 */
struct ConnectionManager {
  std::vector<ConnFlags> flags;
  std::vector<std::vector<uint8_t>> incoming_buffers;
  std::vector<std::vector<uint8_t>> outgoing_buffers;
  std::vector<uint32_t> user_ids;
  std::vector<std::string> usernames;

  size_t get_size() const noexcept { return flags.size(); }
};

constexpr int INITIAL_CAPACITY{10000};

extern ConnectionManager conn_manager;

struct MemoryMetrics {
  long virtual_mem_bytes;
  long physical_ram_bytes;
};

/**
 * Takes input from stdin (some command) and processes it.
 */
int handle_server_input();

// TODO: Should make this static later hoenstly
void set_nonblocking_fd(int fd);

/**
 * Handles reading data from and serving the connection
 * @param connfd Fd of the TCP connection.
 * @param epollfd Fd for the epoll instance being used on the server.
 *
 * NOTE: If this function is able to serve a request, it may transition this
 * connection into write mode so that we can write a response message
 * corresponding to the request message that we processed.
 */
void handle_read_connection(int connfd, int epollfd);

/**
 * Handles writing data to the remote peer
 * @param connfd Fd of the TCP connection.
 * @param epollfd Fd for the epoll instance being used on the server.
 */
void handle_write_connection(int connfd, int epollfd);

/**
 * Closes a connection from the TCP server
 * @param fd The fd for the TCP connection socket
 * @param epollfd FD of the epoll instance powering the server.
 * @note The idea is that a connection is set as wanting to be closed, then
 * the event loop will call this function.
 */
void remove_connection(int fd, int epollfd);

/**
 * Handles accepting all incoming TCP connection possible
 * @param listenfd File descriptor for the listening socket.
 * @param epollfd File descriptor for the epoll instance
 * @note This function will accept connections until it empties out the
 * kernel-side buffer of TCP connection requests are depleted (until EAGAIN or
 * EWOULDBLOCK).
 */
void accept_all_connections(int listenfd, int epollfd);

/**
 * Initializes and starts the server at some port
 * @param port Port number that we're starting the server on.
 *  @param is_test Integer representing whether we're running in test mode.
 * @return Descriptor for the server's listening socket on success, otherwise -1
 * on error.
 */
int init_server(char *port, int is_test);

#endif