#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "shared.h"
#include "protocol.h"
#define BUFFER_SIZE 1024

// Test statistics
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Helper macros for assertions
#define ASSERT(condition, message) do { \
  if (!(condition)) { \
    printf("ASSERTION FAILED: %s\n", message); \
    return 0; \
  } \
} while(0)

#define ASSERT_EQ(actual, expected, message) do { \
  if ((actual) != (expected)) { \
    printf("ASSERTION FAILED: %s (expected %d, got %d)\n", message, expected, actual); \
    return 0; \
  } \
} while(0)

/**
 * Connects to the server
 */
int connect_to_server() {
  int SERVER_PORT = 8080;
  char SERVER_IP[16] = "127.0.0.1";
  int clientfd = socket(AF_INET, SOCK_STREAM, 0);
  if (clientfd < 0) {
    perror("socket() failed");
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(SERVER_PORT);

  if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) != 1) {
    fprintf(stderr, "inet_pton Failure: %s\n", strerror(errno));
    close(clientfd);
    return -1;
  }

  int conn_result = connect(clientfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result < 0) {
    perror("Connection failed!\n");
    close(clientfd);
    return -1;
  }

  printf("Client is connected to server at %s:%d\n", SERVER_IP, SERVER_PORT);
  return clientfd;
}

// Return 1 for pass, otherrwise 0 for fail
int test_registration() {
  printf("Test: User Registration\n");
  int fd = connect_to_server();
  ASSERT(fd >= 0, "Failed to connect to server");

  message_t request = {0};
  registration_credentials_t creds = {0};
  strcpy(creds.username, "testuser");
  strcpy(creds.password, "testpass");
  
  int rc = build_register_request(&request, &creds);
  ASSERT(rc == 0, "Failed to build registration request!");
  
  rc = write_one_message(fd, &request);
  ASSERT(rc != -1, "Failed to write registration request!");

  message_t server_response = {0};
  rc = read_one_message(fd, &server_response);
  ASSERT(rc != -1, "Failed to read server response message!");
  printf("Server Response (code=%d): %s\n", server_response.rc, response_messages[server_response.rc]);
  ASSERT_EQ(server_response.rc, 0, "Registration should succeed with valid credentials");

  close(fd);
  return 1;
}

int test_login_after_registration() {
  int fd = connect_to_server();
  message_t request = {0};
  login_credentials_t creds = {0};
  strcpy(creds.username, "testuser");
  strcpy(creds.password, "testpass");

  int rc = build_login_request(&request, &creds);
  ASSERT(rc == 0, "Failed to build login request");

  rc = write_one_message(fd, &request);
  ASSERT(rc != -1, "Failed to write login request!");

  message_t response = {0};
  rc = read_one_message(fd, &response);
  ASSERT(rc != -1, "Failed to read login response!");

  printf("Server Response (code=%d): %s\n", response.rc, response_messages[response.rc]);
  ASSERT_EQ(response.rc, 0, "Login should succeed with correct credentials");
  
  close(fd);

  printf("Passed\n");
  return 1;
}

int test_login_bad_credentials() {
  int fd = connect_to_server();
  message_t request = {0};
  login_credentials_t creds = {0};
  strcpy(creds.username, "testuser");
  strcpy(creds.password, "testpass123");

  int rc = build_login_request(&request, &creds);
  ASSERT(rc == 0, "Failed to build login request");

  rc = write_one_message(fd, &request);
  ASSERT(rc != -1, "Failed to write login request!");

  message_t response = {0};
  rc = read_one_message(fd, &response);
  ASSERT(rc != -1, "Failed to read login response!");

  printf("Server Response (code=%d): %s\n", response.rc, response_messages[response.rc]);

  ASSERT(response.rc != 0, "Login should fail with bad credentials");

  close(fd);
  printf("Passed\n");
  return 1;
}

int test_duplicate_registration() {
  printf("Test: User Registration\n");
  int fd = connect_to_server();
  ASSERT(fd >= 0, "Failed to connect to server");

  message_t request = {0};
  registration_credentials_t creds = {0};
  strcpy(creds.username, "testuser");
  strcpy(creds.password, "testpass");
  
  int rc = build_register_request(&request, &creds);
  ASSERT(rc == 0, "Failed to build registration request!");
  
  rc = write_one_message(fd, &request);
  ASSERT(rc != -1, "Failed to write registration request!");

  message_t server_response = {0};
  rc = read_one_message(fd, &server_response);
  ASSERT(rc != -1, "Failed to read server response message!");

  printf("Server Response (code=%d): %s\n", server_response.rc, response_messages[server_response.rc]);
  ASSERT_EQ(server_response.rc, 2, "Registration should fail because this user already exists!");

  close(fd);
  return 1;
}


void run_test(int (*test_func)(), const char* test_name) {
  tests_run++;
  if (test_func()) {
    tests_passed++;
  } else {
    tests_failed++;
    printf("  ❌ FAILED: %s\n", test_name);
  }
}

int main() {
  
  run_test(test_registration, "test_registration");
  // run_test(test_login_after_registration, "test_login_after_registration");
  // run_test(test_login_bad_credentials, "test_login_bad_credentials");
  // run_test(test_duplicate_registration, "test_duplicate_registration");


  printf("\n===========================================\n");
  printf("Test Results\n");
  printf("===========================================\n");
  printf("Total:  %d\n", tests_run);
  printf("Passed: %d ✅\n", tests_passed);
  printf("Failed: %d ❌\n", tests_failed);
  printf("===========================================\n");

  return tests_failed == 0 ? 0 : 1;  // Exit with error if any failed
}