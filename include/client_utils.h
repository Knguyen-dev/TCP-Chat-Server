#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include <arpa/inet.h>
#include <poll.h>
#include "shared.h"
#include "protocol.h"


int handle_client_registration(int clientfd);

/**
 * Handles prompting user input, and attempting a login for the user.
 * @param clientfd Client fd that's used in the socket connection.
 * @param user Pointer to a user struct that we'll populate with the info of the authenticated user.
 * @return 0 on successful login, otherwise -1.
 */
int handle_client_login(int clientfd, user_t *user);


void run_messaging_loop(int clientfd, user_t* user);

/**
 * Creates a TCP connection with a server at a given IP:port
 * 
 * @param ip String representing IP address
 * @param port Short representing the port number
 * @return Connection descriptor on success, otherwise -1.
 */
int create_client_connection(char* ip, short port);




#endif