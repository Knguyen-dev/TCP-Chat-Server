#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

/**
 * Opens a listening socket at the given port
 * 
 * @param port Port number that we're launching the server at.
 * @return The fd of the listening socket we've opened. Otherwise -1 on error.
 */
int open_listenfd(char *port);

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