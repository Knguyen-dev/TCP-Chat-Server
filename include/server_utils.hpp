#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H
#include "shared.hpp"

#include <sys/epoll.h> // for epoll readiness API

#define LISTENQ 100
#define MAX_EVENTS 64



// Read and writing the socket + it contains application-level logic for serving
// handle_read_connection()
// handle_write_connection()

// Connection table; 
extern std::vector<conn_t*> conn_table;

/**
 * Handles reading data from and serving the connection
 * @param conn Connection that we're reading data from and serving
 * 
 * NOTE: If this function is able to serve a request, it may transition this 
 * connection into write mode so that we can write a response message corresponding 
 * to the request message that we processed. 
 */
void handle_read_connection(conn_t& conn);

/**
 * Handles writing data to the remote peer
 * @param conn Connection that we're writing data into.
 */
void handle_write_connection(conn_t& conn);

/**
 * Closes a connection from the TCP server
 * @param fd The fd for the TCP connection socket
 * @param epollfd FD of the epoll instance
 */
void remove_connection(int fd, int epollfd);

/**
 * Handles accepting all incoming TCP connection possible
 * @param listenfd File descriptor for the listening socket.
 * @param epollfd File descriptor for the epoll instance
 * @param ev Epoll event struct that we'll reuse for registration.
 * 
 * NOTE: This function will accept connections until it empties out the kernel-side buffer 
 * of TCP connection requests are depleted (until EAGAIN or EWOULDBLOCK).
 */
void accept_all_connections(int listenfd, int epollfd, struct epoll_event& ev);

/**
 * Initializes and starts the server at some port 
 * @param port Port number that we're starting the server on.
 * @return Descriptor for the server's listening socket on success, otherwise -1 on error.
 */
int init_server(char* port);

#endif