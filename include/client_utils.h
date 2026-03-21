#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include "shared.h"

/**
 * Creates request message for user registration
 * @param username User's username.
 * @param password User's password.
 * @return 0 on success, otherwise -1
 */
int create_registration_message(char *username, char *password, message_t *request);

/**
 * Handles prompting user input and sending data over the wire.
 * 
 * @param clientfd The client fd associated with our TCP connection
 * @return 0 on success, otherwise -1 on error.
 */
int handle_client_registration(int clientfd);

/**
 * Creates a login request message
 * @param username Username of the user loggging in.
 * @param password Password of the user logging in.
 * @param request Request message that's going to be used.
 * @return 0 on success, otherwise -1.
 */
int create_login_message(char *username, char *password, message_t *request);

/**
 * Handles prompting user input, and attempting a login for the user.
 * @param clientfd Client fd that's used in the socket connection.
 * @param user Pointer to a user struct that we'll populate with the info of the authenticated user.
 * @return 0 on successful login, otherwise -1.
 */
int handle_client_login(int clientfd, user_t *user);

/**
 * Creates a request message for broadcasting a message to the world.
 *
 * @param message_content Message content that the client wants to broadcast to the world.
 * @param request Request message that they're going to use to broadcast.
 * @return 0 on success, -1 otherwise.
 */
int create_world_message(char *message_content, message_t *request);

/**
 * Handles letting the user broadcast a message to the world
 * 
 * @param clientfd The client's fd.
 * @param user Authenticated user that's sending a message.
 * @return 0 on success, otherwise -1.
 */
int handle_world_message(int clientfd, user_t *user);

/**
 * Creates a message that you can send to a peer.
 * 
 * @param recipient_username Username of the recipient we're sending the message.
 * @param message_content Actual text content that we want to send over to the remote.
 * @param request Pointer to message struct that we'll use to send over.
 */
int create_peer_message(char *recipient_username, char *message_content, message_t *request);

/**
 * Creates a TCP connection with a server at a given IP:port
 * 
 * @param ip String representing IP address
 * @param port Short representing the port number
 * @return Connection descriptor on success, otherwise -1.
 */
int create_client_connection(char* ip, short port);

#endif