#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include <arpa/inet.h>
#include <poll.h>
#include "shared.hpp"
#include "protocol.hpp"


int handle_client_registration(int clientfd);

/**
 * Handles prompting user input, and attempting a login for the user.
 * @param clientfd Fd for the TCP connection socket that the client has with the server.
 * @param user A user struct that we'll populate with the info of the authenticated user.
 * @return 0 on successful login, otherwise -1.
 */
int handle_client_login(int clientfd, user_t& user);

/**
 * Handles running an event loop that'll let the user chat to user others and see messages from other users
 * @param clientfd Fd for the TCP connection socket that the client has with the server.
 * @param user A user struct that represents the user that the client is authenticated as.
 */
void run_messaging_loop(int clientfd, user_t& user);

/**
 * Creates a TCP connection with a server at a given IP:port
 * 
 * @param ip String representing IP address
 * @param port Short representing the port number
 * @return Connection descriptor on success, otherwise -1.
 */
int create_client_connection(char* ip, short port);




#endif