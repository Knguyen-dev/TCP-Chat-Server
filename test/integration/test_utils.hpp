#ifndef TEST_UTILS_HPP
#define TEST_UTILS_HPP
#include "protocol.hpp"
#include "shared.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

extern const char* TEST_SERVER_IP;
extern const uint16_t TEST_SERVER_PORT;

/**
 * If the expression is a falsy value, it prints a failure message with the file,
 * line number, and the expression that failed. 
 * @return -1 if the assertion fails, otherwise does nothing.
 */
#define ASSERT_TRUE(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #expr << "\n"; \
      return -1; \
    } \
  } while (0)

/**
 * Runs a test function and prints whether it passed or failed based on its return value.
 * @param func The test fucntion to be run, which should return 0 on success and non-zero on failure.
 * @return The return value of the test function if it fails. Otherwise returns nothing on pass.
 */
#define RUN_TEST(func) \
  do { \
    std::cout << "Running " << #func << "... "; \
    int rc = func(); \
    if (rc != 0) { \
      std::cout << "FAILED\n"; \
      return rc; \
    } \
    std::cout << "OK\n"; \
  } while (0)

/**
 * Connects to the server at the specified IP and port.
 * @param ip The IP address of the server.
 * @param port The port number of the server.
 * @return The socket file descriptor on success, or -1 on failure.
 */
int connect_to_server(const char* ip, uint16_t port);

/**
 * Generates a random alphanumeric string of specified length
 * @param length The desired length of the random string.
 * @return A random alphanumeric string.
 * @note The reason we need this is because it allows us to create unique 
 * usernames, passwords, messages and other info that a user can type.
 */
std::string random_string(std::size_t length);

/**
 * Sends a user registration request to the server
 * @param fd The socket fd connected to the server.
 * @param username Username of the user we want to register.
 * @param password Password of the user we want to register.
 * @return number of bytes written on success, or -1 on failure.
 */
int send_register_request(int fd, const std::string& username, const std::string& password);

/**
 * Sends a user login request to the server.
 * @param fd The socket fd connected to the server.
 * @param username Username of the user we want to login.
 * @param password Password of the user we want to login.
 * @return number of bytes written on success, or -1 on failure.
 */
int send_login_request(int fd, const std::string& username, const std::string& password);

/**
 * Sends a malformed user registration request to the server, wiht options to omit certain fields.
 * @param fd The socket fd connected to the server.
 * @param include_username Whether to include the username field in the request.
 * @param include_password Whether to include the password field in the request.
 * @param oversize_username Whether to make the username field exceed the maximum allowed length.
 * @param oversize_password Whether to make the password field exceed the maximum allowed length.
 * @return number of bytes written on success, or -1 on failure.
 */
int send_malformed_register(int fd, bool include_username, bool include_password, bool oversize_username, bool oversize_password);

/**
 * Sends a malformed user registration request to the server, wiht options to omit certain fields.
 * @param fd The socket fd connected to the server.
 * @param include_username Whether to include the username field in the request.
 * @param include_password Whether to include the password field in the request.
 * @param oversize_username Whether to make the username field exceed the maximum allowed length.
 * @param oversize_password Whether to make the password field exceed the maximum allowed length.
 * @return number of bytes written on success, or -1 on failure.
 */
int send_malformed_login(int fd, bool include_username, bool include_password, bool oversize_username, bool oversize_password);

/**
 * Builds a world broadcast without client side validation.
 */
int build_world_broadcast_unsafe(uint8_t* request_buffer, world_broadcast_t& broadcast, uint32_t& message_len);

/**
 * Sends a world broadcast request to the server.
 * @param fd The socket fd connected to the server.
 * @param sender_username Username of the user sending the broadcast.
 * @param message_content Content of the broadcast message.
 * @return number of bytes written on success, or -1 on failure.
 */
int send_world_broadcast_request(int fd, const std::string& sender_username, const std::string& message_content);

/**
 * Registers a random user and logs them into the server. 
 * @param out_user Reference to a user_t struct that'll be populated with the info of the authenticated user.
 * @return The socket file descriptor for the logged-in connection, or -1 on failure.
 */
int register_and_login(user_t& out_user);

/**
 * Sends a malformed world broadcast request to the server, with options to omit certain fields or make the message content exceed the maximum allowed length.
 * @param fd The socket fd connected to the server.
 * @param include_sender_username Whether to include the sender username field in the request.
 * @param include_message_content Whether to include the message content field in the request.
 * @param oversize_message_content Whether to make the message content field exceed the maximum allowed length
 * @return number of bytes written on success, or -1 on failure.
 */
int send_malformed_world_broadcast(int fd, bool include_sender_username, bool include_message_content, bool oversize_message_content);

/* P2P helpers */

/**
 * Builds a p2p broadcast without any client side validation.
 */
int build_p2p_broadcast_unsafe(uint8_t* request_buffer, p2p_broadcast_t& broadcast, uint32_t& message_len);

int send_p2p_broadcast_request(int fd, const std::string& sender_username, const std::string& recipient_username, const std::string& message_content);
int send_malformed_p2p_broadcast(int fd, bool include_sender, bool include_recipient, bool include_content, bool oversize_content);

/**
 * Reads a response message from the server.
 * @param fd The socket file descriptor connected to the server.
 * @param response Reference to a message_t struct where the response will be stored.
 * @return 0 on success, or -1 on failure.
 */
int read_server_response(int fd, message_t& response);

#endif // TEST_UTILS_HPP