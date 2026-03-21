#ifndef SHARED_H
#define SHARED_H

#define _POSIX_C_SOURCE 200112L
#define LISTENQ 100
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>

#define MSG_HEADER_SIZE 7 // max header size in bytes
#define MSG_MAX_PAYLOAD_SIZE 4096 // max payload size in bytes
#define MAX_USERNAME_SIZE 32
#define MAX_PASSWORD_SIZE 32
#define MAX_MSG_CONTENT_SIZE 250 // MAX message size a user can type/send out

// -----------------------------------
// Database (File) States
// -----------------------------------

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
  uint8_t payload[MSG_MAX_PAYLOAD_SIZE];
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
  RESP_ERROR_USER_EXISTS = 2, // For "username already taken"
  RESP_ERROR_USER_NOT_FOUND = 3,
  RESP_ERROR_INVALID_CREDENTIALS = 4,
  RESP_ERROR_INTERNAL = 5,
  RESP_ERROR_UNKNOWN_COMMAND = 6
} response_code_t;

// Response messages corresponding with response codes 
extern const char* response_messages[];

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
  TAG_IS_AUTH = 7,

  TAG_SENDER_USERNAME = 8,
  TAG_RECIPIENT_USERNAME = 9,
  TAG_MESSAGE_CONTENT = 10,
  // TAG_SERVER_ERROR = 8
} tlv_tag_t;

/**
 * Writes a TLV into a buffer.
 * 
 * @param buf A double pointer to a buffer. The motivation is that with a single pointer *buf
 *            modifications like buf += 1, doesn't update the pointer in the caller. However by 
 *            using a double pointer, we'll be able to update the buffer pointer in the caller.
 * @param tag The tag that we're giving the TLV; "what is it?".
 * @param len The size of the data (in bytes from 0-255) of the value, "how many bytes is it?".
 * @param value A pointer to the value that we want to write into the buffer
 * @param convert_to_network 1 to convert the value into network-byte-order, otherwise 0 for no conversion.
 *
 * NOTE: 
 * - Intends to mainly be a helper function to create_response.
 * - If len > 1, we know that the value ("payload") is a multi-byte type.
 * - Limitations: Since len is a uint8_t we can only represent payloads of size [0, 255] bytes. If we wanted to represent bigger payloads, 
 * we'd simply upgrade to uint16_t, allowing us to write values of size [0, 65335] bytes, which will later be useful for messages. Of course, 
 * if we decide to use uint16_t, we'd need to ensure the 16-bit integer is represented in network-byte-order and probably use memcpy to copy 
 * from bytes from the integer into the buffer.
 */
void write_tlv(uint8_t **buf, tlv_tag_t tag, uint8_t len, const void *value, int convert_to_network);

/**
 * Fully read one message from a connection socket 
 * 
 * @param connfd File descriptor for the connection socket we're reading from.
 * @param msg Pointer to message_t struct that we're storing the message information in.
 * @return 0 on success, -1 otherwise
 */
int read_one_message(int connfd, message_t* msg);

/**
 * Writes one message across the TCP socket 
 * 
 * @param connfd Descriptor for the connection socket 
 * @param response Response message that we want to write to the remote peer.
 * @return Number of bytes sent, otherwise -1 on error
 * 
 * NOTE: 
 * It's probably best for this function to assume that 
 * the fields and payload in the response messaeg to already be 
 * in network byte order.
 */
int write_one_message(int connfd, message_t* response);

/**
 * Prompts input for a value within a given range.
 * 
 * @param prompt Input prompt we're going to repeat.
 * @param min Minimum valid value.
 * @param max Maximum valid value.
 * @return The valid integer value that they inputted.
 */
int get_valid_input_range(char *prompt, int min, int max);

#endif