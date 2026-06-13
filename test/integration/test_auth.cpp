#include "test_utils.hpp"

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

// ##### Registration Tests Below #####

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

/**
 * Tests whether registering without a password field results in 
 * an error response from the server.
 */
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

// ##### Login Tests Below #####

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

/**
 * Tests that attempting to login with a missing username or password 
 * will result in an error response from the server.
 */
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
  std::cout << "All auth integration tests passed.\n";
  return 0;
}
