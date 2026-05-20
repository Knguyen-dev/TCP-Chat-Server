#include "protocol.hpp"
#include "shared.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

static const char* TEST_SERVER_IP = "127.0.0.1";
static const uint16_t TEST_SERVER_PORT = 8080;

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
static int connect_to_server(const char* ip, uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    close(sock);
    std::cerr << "Invalid server IP\n";
    return -1;
  }

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("connect");
    close(sock);
    return -1;
  }

  return sock;
}

/**
 * Generates a random alphanumeric string of specified length
 * @param length The desired length of the random string.
 * @return A random alphanumeric string.
 * @note The reason we need this is because it allows us to create unique 
 * usernames, passwords, messages and other info that a user can type.
 */
static std::string random_string(std::size_t length) {
  static const char alphanum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static std::mt19937_64 rng(static_cast<unsigned long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
  static std::uniform_int_distribution<size_t> dist(0, sizeof(alphanum) - 2);

  std::string result;
  result.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    result.push_back(alphanum[dist(rng)]);
  }
  return result;
}

/**
 * Appends a tag, length, and value to a buffer in TLV format.
 * @param ptr Pointer to the current position in the buffer; will be updated to point after
 * the appended TLV.
 * @param tag The tag byte to append.
 * @param len The length byte to append.
 * @param value Pointer to the value bytes to append.
 */
static void append_tlv(uint8_t*& ptr, uint8_t tag, uint8_t len, const void* value) {
  *ptr++ = tag;
  *ptr++ = len;
  memcpy(ptr, value, len);
  ptr += len;
}

/**
 * Reads a response message from the server.
 * @param fd The socket file descriptor connected to the server.
 * @param response Reference to a message_t struct where the response will be stored.
 * @return 0 on success, or -1 on failure.
 * 
 */
static int read_server_response(int fd, message_t& response) {
  return read_one_message(fd, response);
}

/**
 * Sends a user registration request to the server
 * @param fd The socket fd connected to the server.
 * @param username Username of the user we want to register.
 * @param password Password of the user we want to register.
 * @return number of bytes written on success, or -1 on failure.
 */
static int send_register_request(int fd, const std::string& username, const std::string& password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  registration_credentials_t creds{username, password};
  uint32_t message_len = 0;
  if (build_register_request(buffer, creds, message_len) != 0) {
    return -1;
  }
  return write_one_message(fd, buffer, message_len);
}

/**
 * Sends a user login request to the server.
 * @param fd The socket fd connected to the server.
 * @param username Username of the user we want to login.
 * @param password Password of the user we want to login.
 * @return number of bytes written on success, or -1 on failure.
 */
static int send_login_request(int fd, const std::string& username, const std::string& password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  login_credentials_t creds{username, password};
  uint32_t message_len = 0;
  if (build_login_request(buffer, creds, message_len) != 0) {
    return -1;
  }
  return write_one_message(fd, buffer, message_len);
}

/**
 * Sends a malformed user registration request to the server, wiht options to omit certain fields.
 * @param fd The socket fd connected to the server.
 * @param include_username Whether to include the username field in the request.
 * @param include_password Whether to include the password field in the request.
 * @param oversize_username Whether to make the username field exceed the maximum allowed length.
 * @param oversize_password Whether to make the password field exceed the maximum allowed length.
 * @return number of bytes written on success, or -1 on failure.
 */
static int send_malformed_register(int fd, bool include_username, bool include_password, bool oversize_username, bool oversize_password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint8_t* ptr = buffer;
  *ptr++ = 1;
  *ptr++ = REGISTER;
  *ptr++ = 0;
  uint8_t* payload_length_ptr = ptr;
  ptr += 4;

  if (include_username) {
    std::string username = oversize_username ? std::string(MAX_USERNAME_SIZE + 1, 'a') : "user";
    append_tlv(ptr, TAG_USERNAME, static_cast<uint8_t>(username.size()), username.data());
  }
  if (include_password) {
    std::string password = oversize_password ? std::string(MAX_PASSWORD_SIZE + 1, 'b') : "password";
    append_tlv(ptr, TAG_PASSWORD, static_cast<uint8_t>(password.size()), password.data());
  }

  uint32_t payload_length = static_cast<uint32_t>(ptr - (buffer + MSG_HEADER_SIZE));
  uint32_t net_payload_length = htonl(payload_length);
  memcpy(payload_length_ptr, &net_payload_length, sizeof(net_payload_length));

  return write_one_message(fd, buffer, MSG_HEADER_SIZE + payload_length);
}

/**
 * Sends a malformed user registration request to the server, wiht options to omit certain fields.
 * @param fd The socket fd connected to the server.
 * @param include_username Whether to include the username field in the request.
 * @param include_password Whether to include the password field in the request.
 * @param oversize_username Whether to make the username field exceed the maximum allowed length.
 * @param oversize_password Whether to make the password field exceed the maximum allowed length.
 * @return number of bytes written on success, or -1 on failure.
 */
static int send_malformed_login(int fd, bool include_username, bool include_password, bool oversize_username, bool oversize_password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint8_t* ptr = buffer;
  *ptr++ = 1;
  *ptr++ = LOGIN;
  *ptr++ = 0;
  uint8_t* payload_length_ptr = ptr;
  ptr += 4;

  if (include_username) {
    std::string username = oversize_username ? std::string(MAX_USERNAME_SIZE + 1, 'a') : "user";
    append_tlv(ptr, TAG_USERNAME, static_cast<uint8_t>(username.size()), username.data());
  }
  if (include_password) {
    std::string password = oversize_password ? std::string(MAX_PASSWORD_SIZE + 1, 'b') : "password";
    append_tlv(ptr, TAG_PASSWORD, static_cast<uint8_t>(password.size()), password.data());
  }

  uint32_t payload_length = static_cast<uint32_t>(ptr - (buffer + MSG_HEADER_SIZE));
  uint32_t net_payload_length = htonl(payload_length);
  memcpy(payload_length_ptr, &net_payload_length, sizeof(net_payload_length));

  return write_one_message(fd, buffer, MSG_HEADER_SIZE + payload_length);
}

/**
 * Tests whether we can connect to the server, which is a pre-requisite for all other tests.
 * Connection will close after successful connection and verification.
 */
int test_connect_server() {
  int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock != -1);
  close(sock);
  return 0;
}

/**
 * Tests successful user registration with valid username and password. Verifies that the server responds with RESP_OK.
 */
int test_register_success() {
  const std::string username = "testuser_" + random_string(8);
  const std::string password = "testpass";

  int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock != -1);
  ASSERT_TRUE(send_register_request(sock, username, password) != -1);

  message_t response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  response.payload = payload_buffer;
  
  ASSERT_TRUE(read_server_response(sock, response) == 0);
  ASSERT_TRUE(response.type == REGISTER);
  ASSERT_TRUE(response.rc == RESP_OK);

  close(sock);
  return 0;
}

/**
 * Tests that attempting to register with a username that already exists results in an error response from the server. 
 * Verifies that the server responds with RESP_ERROR_USER_EXISTS.
 */
int test_register_duplicate_username() {
  // Register with a unique username first.
  const std::string username = "duplicate_" + random_string(8);
  const std::string password = "password123";
  int sock1 = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock1 != -1);
  ASSERT_TRUE(send_register_request(sock1, username, password) != -1);
  
  message_t first_response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  first_response.payload = payload_buffer;


  ASSERT_TRUE(read_server_response(sock1, first_response) == 0);
  ASSERT_TRUE(first_response.type == REGISTER);
  ASSERT_TRUE(first_response.rc == RESP_OK);
  close(sock1);

  // Then attempt to register with the same username again, which should fail.
  int sock2 = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock2 != -1);
  ASSERT_TRUE(send_register_request(sock2, username, password) != -1);
  
  // NOTE: Using the same payload buffer is fine since we don't need to preserve the first
  message_t second_response{};
  second_response.payload = payload_buffer;
  ASSERT_TRUE(read_server_response(sock2, second_response) == 0);
  ASSERT_TRUE(second_response.type == REGISTER);
  ASSERT_TRUE(second_response.rc == RESP_ERROR_USER_EXISTS);
  close(sock2);
  return 0;
}

/**
 * Tests whether the server handles when a username field is missing.
 */
int test_username_field_missing() {
  int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock != -1);
  ASSERT_TRUE(send_malformed_register(sock, false, true, false, false) != -1);

  message_t response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  response.payload = payload_buffer;

  ASSERT_TRUE(read_server_response(sock, response) == 0);
  ASSERT_TRUE(response.type == REGISTER);
  ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
  close(sock);
  return 0;
}


int test_password_field_missing() {
  int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock != -1);
  ASSERT_TRUE(send_malformed_register(sock, true, false, false,false) != -1);
  message_t response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  response.payload = payload_buffer;
  ASSERT_TRUE(read_server_response(sock, response) == 0);
  ASSERT_TRUE(response.type == REGISTER);
  ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
  close(sock);
  return 0;
}

/**
 * Tests whether the server handles when the username and password fields exceed the maximum
 * allowed length.
 */
int test_field_length_exceeded() {
  int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(sock != -1);
  ASSERT_TRUE(send_malformed_register(sock, true, true, true, true) != -1);
  message_t response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  response.payload = payload_buffer;

  ASSERT_TRUE(read_server_response(sock, response) == 0);
  ASSERT_TRUE(response.type == REGISTER);
  ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
  close(sock);
  return 0;
}

/**
 * Tests successful user login with valid username and password. 
 */
int test_login_success() {

    // First register the user
  const std::string username = "loginuser_" + random_string(8);
  const std::string password = "password123";
  int register_sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(register_sock != -1);
  ASSERT_TRUE(send_register_request(register_sock, username, password) != -1);
  message_t register_response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  register_response.payload = payload_buffer;
  ASSERT_TRUE(read_server_response(register_sock, register_response) == 0);
  ASSERT_TRUE(register_response.rc == RESP_OK);
  close(register_sock);

//   Then login with the same credenitals which should work.
  int login_sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  ASSERT_TRUE(login_sock != -1);
  ASSERT_TRUE(send_login_request(login_sock, username, password) != -1);
  message_t login_response{};
  login_response.payload = payload_buffer;
  ASSERT_TRUE(read_server_response(login_sock, login_response) == 0);
  ASSERT_TRUE(login_response.type == LOGIN);
  ASSERT_TRUE(login_response.rc == RESP_OK);
  close(login_sock);
  return 0;
}

/**
 * Tests that attempting to login with a username that doesn't exist results in an error response.
 */
int test_login_nonexistent_username() {
    // Attempts to login with obviously non-existent username.
    // NOTE: This only works if no one has registered with "no_such_user" in the before tests.
    int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(sock != -1);
    ASSERT_TRUE(send_login_request(sock, "no_such_user", "password") != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(sock, response) == 0);
    ASSERT_TRUE(response.type == LOGIN);
    ASSERT_TRUE(response.rc == RESP_ERROR_USER_NOT_FOUND);
    close(sock);
    return 0;
}

/**
 * Tests that attempting to login with an incorrect password results in an error response.
 */
int test_login_incorrect_password() {
    const std::string username = "badpass_" + random_string(8);
    const std::string password = "correctpass";
    int register_sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(register_sock != -1);
    ASSERT_TRUE(send_register_request(register_sock, username, password) != -1);
    message_t register_response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    register_response.payload = payload_buffer;

    ASSERT_TRUE(read_server_response(register_sock, register_response) == 0);
    ASSERT_TRUE(register_response.rc == RESP_OK);
    close(register_sock);

    int login_sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(login_sock != -1);
    ASSERT_TRUE(send_login_request(login_sock, username, "wrongpass") != -1);
    message_t login_response{};
    login_response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(login_sock, login_response) == 0);
    ASSERT_TRUE(login_response.type == LOGIN);
    ASSERT_TRUE(login_response.rc == RESP_ERROR_INVALID_CREDENTIALS);
    close(login_sock);
    return 0;
}

int test_login_field_missing() {
    int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(sock != -1);
    ASSERT_TRUE(send_malformed_login(sock, true, false, false, false) != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(sock, response) == 0);
    ASSERT_TRUE(response.type == LOGIN);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(sock);
    return 0;
}

int main() {
  RUN_TEST(test_connect_server);
  RUN_TEST(test_register_success);
  RUN_TEST(test_register_duplicate_username);
  RUN_TEST(test_username_field_missing);
  RUN_TEST(test_password_field_missing);
  RUN_TEST(test_field_length_exceeded);
  RUN_TEST(test_login_success);
  RUN_TEST(test_login_nonexistent_username);
  RUN_TEST(test_login_incorrect_password);
  RUN_TEST(test_login_field_missing);
  std::cout << "All integration tests passed.\n";
  return 0;
}
