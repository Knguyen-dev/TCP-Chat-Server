#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include <arpa/inet.h>
#include <poll.h>
#include "shared.hpp"
#include "protocol.hpp"
#include "logger.hpp"
#include <vector>
#include <signal.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <mutex>


/**
 * Sets up signal handlers for the client; for graceful shutdown.
 * @param conn Connection representing the client's TCP connection with the server.
 * @return 0 on success, otherwise -1.
 */
int setup_client_signal_handlers(conn_t* conn);

/**
 * Handles running an event loop that'll let the user chat to user others and see messages from other users
 * @param conn TCP connection that the client has with the server.
 */
void run_messaging_loop(conn_t& conn);

/**
 * Creates a TCP connection with a server at a given IP:port
 * 
 * @param ip String representing IP address
 * @param port Short representing the port number
 * @param conn Connection representing the client's connection with the server, which will be populated with info from the callee.
 * @return Connection descriptor on success, otherwise -1.
 */
int create_client_connection(char* ip, short port, conn_t& conn);

#endif