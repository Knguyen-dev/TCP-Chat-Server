#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

/**
 * Initializes and starts the server at some port 
 * @param port Port number that we're starting the server on.
 * @return Descriptor for the server's listening socket on success, otherwise -1 on error.
 */
int init_server(char* port);

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