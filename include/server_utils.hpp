#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H
#include "shared.hpp"

#include <sys/epoll.h> // for epoll readiness API

#define LISTENQ 100
#define MAX_EVENTS 64

// Connection table; exposing this so that the client can run a loop.
// TODO: May not be necessary!
extern std::vector<conn_t*> conn_table;


// TODO: Maybe will implement this later for cleaniness and to reinforce clean code
// not necessary for now
// /**
//  * Struct representing the state of the server.
//  * - listenfd: The fd for the listening socket.
//  * - epollfd: The fd of the epoll instance the server is using.
//  * - conn_table: Vector of pointers, where each one points to a client connection.
//  */
// struct server_state_t {
//   int listenfd{-1};
//   int epollfd{-1};  
//   std::vector<conn_t*> conn_table;
// };

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
 * @return Descriptor for the server's listening socket on success, otherwise -1 on error.
 */
int init_server(char* port);

#endif