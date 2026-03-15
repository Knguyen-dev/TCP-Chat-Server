#ifndef SHARED_H
#define SHARED_H

#define _POSIX_C_SOURCE 200112L // TODO: Explain why this is needed to use structs related to getaddrinfo
#define LISTENQ 100

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>

// Header size in bytes and maximum payload size for a 
// single message in bytes.
const size_t MSG_HEADER_SIZE = 7;
const size_t MSG_MAX_PAYLOAD_SIZE = 4096; 
const size_t MAX_USERNAME_SIZE = 32;
const size_t MAX_PASSWORD_SIZE = 32;

const size_t MAX_MSG_CONTENT_SIZE = 250; // MAX message size in bytes/characters

// -----------------------------------
// Database (File) States
// -----------------------------------
uint64_t num_users = 0;

/**
 * Struct representing a registered user in the application
 * 
 * - id: Incrementing ID associated with the user.
 * - username: Username of the user.
 * - password: Plain-text password for the user.
 */
typedef struct _user {
  uint32_t id;
  char username[MAX_USERNAME_SIZE + 1];
  char password[MAX_PASSWORD_SIZE + 1];
} user_t;

const char* user_file_path = "./src/user_file_db.csv";
pthread_mutex_t user_file_mutex = PTHREAD_MUTEX_INITIALIZER;

// -----------------------------------
// Connection Table States
// -----------------------------------

/**
 * Struct representing an existing TCP client.
 * 
 * - ip_address: An IPv4 or IPv6 address structure.
 * - ip_version: Unsigned integer represesnting the IP protocol version, either 4 or 6.
 * - connfd: Integer containing the file descriptor for the TCP connection socket.
 * - user_t: Pointer to the user associated with the connection.
 * 
 * NOTE:
 * - You should create a connection object, even if the user hasn't logged in.
 *   A user who's not logged in still consumes resources, can make requests and 
 *   overall participate in the app.
 * - A connection also implies that the user is online.
 */
typedef struct _conn {
  struct sockaddr_storage ip_address; // Handles both IPv4/IPv6
  int connfd;
  int is_authenticated;
  user_t* user;
} conn_t;

/*
A dynamically allocated array of `conn_t` structs representing all the clients that are connected 
to the TCP chat server. It's indexed by connfd, so as long as you have the connection descriptor, you
should be able to access it. 
*/
conn_t *conn_table = NULL;
uint32_t conn_table_capacity = 10;
uint32_t num_conns = 0;
pthread_mutex_t conn_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Struct representing a single message being transferred between client and server
 * 
 * - version: Application-layer protocol version.
 * - type: Type of request/message being sent.
 * - flags: Additional flags for special features, e.g., encryption
 * - payload_length: Size of the message payload.
 * - payload: A buffer containing the payload itself.
 */
typedef struct _message {
  uint8_t version;
  uint8_t type;
  uint8_t flags;
  uint32_t payload_length;

  // TODO: Flexible array member; this will be dynamic
  char payload[MSG_MAX_PAYLOAD_SIZE];
} message_t;

// Message Types (Global Header)
typedef enum {
  REGISTER = 0, // Registration requests
  LOGIN = 1,    // Login requests
  CHAT = 2,     // Requests to chat; 
  ACK = 3,      // Messages that we'll send in response to the user; big umbrella, could be improved but it makes sense
  LIST = 4,     // For requests to list users.
} msg_type_t;

// Response codes we can send to the user
// NOTE: Codes specific only to ACK/response messages
typedef enum {
  RESP_OK = 0,
  RESP_ERROR_MALFORMED = 1,
  RESP_ERROR_USER_EXISTS = 2,
  RESP_ERROR_USER_NOT_FOUND = 3,
  RESP_ERROR_INVALID_CREDENTIALS = 4,
  RESP_ERROR_INTERNAL = 5,
  RESP_ERROR_UNKNOWN_COMMAND = 6
} response_code_t;

// Simple response 
static const char* response_messages[] = {
  [RESP_OK] = "Success",
  [RESP_ERROR_MALFORMED] = "Malformed request",
  [RESP_ERROR_USER_EXISTS] = "Username already taken",
  [RESP_ERROR_USER_NOT_FOUND] = "User not found",
  [RESP_ERROR_INVALID_CREDENTIALS] = "Invalid credentials",
  [RESP_ERROR_INTERNAL] = "Internal server error",
  [RESP_ERROR_UNKNOWN_COMMAND] = "Unknown command"
};

// TLV Tags (Payload Context)
// NOTE: This should never exceed the range of uint8_t 
typedef enum {
  TAG_USERNAME = 0,
  TAG_PASSWORD = 1,
  TAG_RESPONSE_CODE = 2,
  TAG_RESPONSE_MESSAGE = 3,
  
  TAG_WORLD = 4,
  TAG_USER_ID = 5,
  TAG_ROOM_ID = 6,
  TAG_IS_ONLINE = 7,
  

  

  TAG_MESSAGE_CONTENT = 8,
  // TAG_SERVER_ERROR = 8
} tlv_tag_t;


void *serve_connection(void *vargp);

#endif