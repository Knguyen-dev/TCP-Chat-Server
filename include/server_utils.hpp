#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H
#include "shared.hpp"

#include <sys/epoll.h> // for epoll readiness API

#include <fstream>
#include <unistd.h>
#include <malloc.h>
#include <string>


#define LISTENQ 100
#define MAX_EVENTS 64

extern std::vector<conn_t*> conn_table;

struct MemoryMetrics {
    long virtual_mem_bytes;
    long physical_ram_bytes;
};

int handle_server_input();

void set_nonblocking_fd(int fd);

/**
 * Handles reading data from and serving the connection
 * @param conn Connection that we're reading data from and serving
 * @param epollfd Fd for the epoll instance being used on the server.
 * 
 * NOTE: If this function is able to serve a request, it may transition this 
 * connection into write mode so that we can write a response message corresponding 
 * to the request message that we processed. 
 */
void handle_read_connection(conn_t& conn, int epollfd);

/**
 * Handles writing data to the remote peer
 * @param conn Connection that we're writing data into.
 * @param epollfd Fd for the epoll instance being used on the server.
 */
void handle_write_connection(conn_t& conn, int epollfd);

/**
 * Closes a connection from the TCP server
 * @param fd The fd for the TCP connection socket
 * @param epollfd FD of the epoll instance being uesd on the server.
 */
void remove_connection(int fd, int epollfd);

/**
 * Handles accepting all incoming TCP connection possible
 * @param listenfd File descriptor for the listening socket.
 * @param epollfd File descriptor for the epoll instance
 * @note This function will accept connections until it empties out the kernel-side buffer 
 * of TCP connection requests are depleted (until EAGAIN or EWOULDBLOCK).
 */
void accept_all_connections(int listenfd, int epollfd);

/**
 * Initializes and starts the server at some port 
 * @param port Port number that we're starting the server on.
*  @param is_test Integer representing whether we're running in test mode.
 * @return Descriptor for the server's listening socket on success, otherwise -1 on error.
 */
int init_server(char* port, int is_test);

#endif