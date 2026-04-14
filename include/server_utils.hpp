#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H
#include "shared.hpp"

#define LISTENQ 100
#define MAX_EVENTS 64



// Read and writing the socket + it contains application-level logic for serving
// handle_read_connection()
// handle_write_connection()

/**
 * Handles reading data from and serving the connection
 * @param conn Connection that we're reading data from and serving
 * 
 * NOTE: If this function is able to serve a request, it may transition this 
 * connection into write mode so that we can write a response message corresponding 
 * to the request message that we processed. 
 */
void handle_read_connection(conn_t* conn);

/**
 * Handles writing data to the remote peer
 */
void handle_write_connection(conn_t* conn);

/**
 * Closes a connection from the TCP server
 * @param fd The fd for the TCP connection socket
 */
void remove_connection(int fd);

/**
 * Handles accepting all incoming TCP connection possible
 * @param listenfd File descriptor for the listening socket.
 * @return Pointer to dynamically allocated memory that stores the conn_t struct representing the connection.
 * 
 * NOTE: This function will accept connections until it empties out the kernel-side buffer 
 * of TCP connection requests are depleted (until EAGAIN or EWOULDBLOCK).
 */
void accept_all_connections(int listenfd);

/**
 * Initializes and starts the server at some port 
 * @param port Port number that we're starting the server on.
 * @return Descriptor for the server's listening socket on success, otherwise -1 on error.
 */
int init_server(char* port);



// TODO: Serve connection might be removed
/**
 * Thread routine that has an infinite server loop.
 * 
 * @param vargp A pointer to the connfd for the thread
 * 
 * NOTE: This is the thread routine that each thread uses to serve a client.
 * We want to maintain the connection until the user disconnects, so 
 * we're probably going to run a while loop
 */
void *serve_connection(void *vargp);

#endif