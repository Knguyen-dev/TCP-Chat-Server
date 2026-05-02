
#include "shared.hpp"

#define MSG_HEADER_SIZE 7 // max header size in bytes
#define MSG_MAX_PAYLOAD_SIZE 4096 // max payload size in bytes

// Struct representing a message being transferred between client and server
typedef struct {
  uint32_t payload_length; // Size of the message payload in host byte order.
  uint8_t version;         // Application-layer protocol version.
  uint8_t type;            // Type of request/message being sent.
  uint8_t rc;              // Response code, assuming type = ACK (the message represents a server response)
  uint8_t* payload;        // Pointer to the start of the message's body/payload
} message_t;

/**
 * Message Types for the Global Header.
 */
typedef enum {
  REGISTER, // Registration requests
  LOGIN,    // Login requests
  CHAT,     // Requests to chat; 
  LIST,     // For requests to list users.
} msg_type_t;

/**
 * Response codes the server can send to the client
 * NOTE: Codes specific only to ACK/response messages
 */
typedef enum {
  RESP_OK,
  RESP_ERROR_MALFORMED,
  RESP_ERROR_USER_EXISTS, // For "username already taken"
  RESP_ERROR_USER_NOT_FOUND,
  RESP_ERROR_INVALID_CREDENTIALS,
  RESP_ERROR_INTERNAL,
  RESP_ERROR_UNKNOWN_COMMAND
} response_code_t;

/**
 * TLV Tags (Payload Context)
 * NOTE: This should never exceed the range of uint8_t 
 */
typedef enum {
  TAG_USERNAME,
  TAG_PASSWORD,
  TAG_RESPONSE_CODE,
  TAG_RESPONSE_MESSAGE,
  TAG_WORLD_BROADCAST,
  TAG_P2P_BROADCAST,
  TAG_USER_ID,
  TAG_ROOM_ID,
  TAG_IS_AUTH,
  TAG_SENDER_USERNAME,
  TAG_RECIPIENT_USERNAME,
  TAG_MESSAGE_CONTENT,

  // TODO: Timestamps could be a future feature
  TAG_TIMESTAMP
} tlv_tag_t;

typedef struct {
  std::string username;
  std::string password;
} registration_credentials_t;

typedef struct {
  std::string username;
  std::string password;
} login_credentials_t;

typedef struct {
  std::string sender_username;
  std::string message_content;
} world_broadcast_t;

typedef struct {
  std::string sender_username;
  std::string recipient_username;
  std::string message_content;
} p2p_broadcast_t;

/**
 * Gets the response message string asscoiated with a response code
 * @param code Response code in question
 * @return A message string representing the response code.
 */
std::string get_response_message(response_code_t code);

/**
 * Append bytes to the back of a given buffer
 * @param buf Reference variable to the buffer we're appending data into.
 * @param data Pointer to the a buffer we'll read data from and copy into said buffer.
 * @param len Number of bytes we're copying from data into buf.
 */
void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len);

/**
 * Remove data from the start of the buffer
 * @param buf Reference to the buffer that we're erasing data from.
 * @param n Number of bytes we want to remove from the start of the buffer.
 */
void buf_consume(std::vector<uint8_t> &buf, size_t n);

/**
 * Parses a message and places field information into a message struct
 * @param buffer Read-only buffer of data that contains the message header. This represents the conn_t::incoming that contains the message we got from the remote.
 * @param message Reference to the message struct that we're going to populate with parsed header information.
 * 
 * NOTE: This can be used client and server-side. On the server side, we assume the
 * buffer has the first `MSG_HEADER_SIZE + 1` bytes of a message. For now, we want that 
 * extra byte so that we can get a pointer to the start of the message body. The idea being 
 * that we don't have to manually copy the memory from the message body into a message_t struct, just 
 * use a pointer to the conn_t::incoming, which will retain that message body we're done parsing 
 * the message.
 */
void parse_message(const std::vector<uint8_t>& buffer, message_t& message);

/**
 * Writes the serialized data of a message into a buffer
 * @param buffer Buffer that we're appending data to the end to. This represents the conn_t::outgoing buffer
 * @param message Message that we're serializing and writing into the buffer.
 * 
 * @note This function has been designed with server-side use in mind. ATP 
 * we're writing into the conn_t::outgoing buffer. The request message should 
 * have everything set already, we only need to deal with writing bytes, endian-ness
 * etc. This operation appends the bytes to the end of the buffer. This operation should ont be interrupted.
 */
void write_message_to_buffer(std::vector<uint8_t>& buffer, message_t& message);

/**
 * Performs a blocking read to get one full message from the remote peer.
 * It'll also parse that entire message into the passed message object.
 * @param fd FD for the TCP connection socket.
 * @param message Message struct that'll be populated which'll contain readable information about the processed message.
 * @return 0 on success, otherwise -1.
 */
int read_one_message(int fd, message_t& message);

/**
 * Performs a blocking write to write one full message from the conn_t::outgoing into the socket
 * @param fd Fd for the tcp connection socket 
 * @param message_buffer Buffer containing the serialized message.
 * @param message_len The size of the message.
 * @return Number of bytes written on success, otherwise -1 on failure.
 */
int write_one_message(int fd, uint8_t* message_buffer, uint32_t message_len);

// ---------------------------------
// User Registration
// ---------------------------------

/**
 * Builds a user registration request
 * @param request Request message that we're going to populate with user registration data.
 * @param credentials Struct containing the user registration credentials that the user inputted in.
 * @param message_len Length of the message, which will be populated/updated here for easy access.
 * @return 0 on success, -1 on error 
 * @note Used by the client to create a registration request message.
 */
int build_register_request(uint8_t* request_buffer, registration_credentials_t& credentials, uint32_t& message_len);

/**
 * Parses a user registration request message and populates credentials object with it.
 * 
 * @param msg User registration request message sent by the client that needs to be parsed.
 * @param credentials Credentials struct that we're going to populate.
 * @return 0 on success, response code on error.
 * 
 * NOTE: Supposed to be used by the server.
 */
int parse_register_request(message_t& msg, registration_credentials_t& credentials);

/**
 * Creates a response after successful registration!
 * @param response Response message that we're going to send.
 * @param user New user that registered 
 * @return 0 on success, otherwise -1 on error
 */
int build_register_response(message_t& response, user_t& user);

/**
 * Parses the server's response to the user's registraton request.
 * @param response Response message representing the server's response to a registration request.
 * @param user Empty user that will be populated with the response's payload data.
 * @return 0 on success, otherwise -1 on error when reading the response.
 * @note Intended to only be used by the client.
 */
int parse_register_response(message_t& response, user_t& user);

// ---------------------------------
// User Login
// ---------------------------------

/**
 * Builds a login request message to be sent to the server
 * @param request_buffer Buffer that contains the serialized request message.
 * @param credentials User credenitals that they're using to login.
 * @param message_len Length of the entire request message in bytes.
 * @return 0 on success, otherwise -1.
 * @note Intended to be used by the client.
 */
int build_login_request(uint8_t* request_buffer, login_credentials_t& credentials, uint32_t& message_len);

/**
 * Parses a login request message and populates credentials object.
 * @param request Login request message sent by the client.
 * @param credentials Credentials object that we'll populate after parsing the request message object.
 * @return 0 on success, non-zero response code on error.
 * @note Intended to be used by the server.
 */
int parse_login_request(message_t& request, login_credentials_t& credentials);

/**
 * Builds a response message for a successful login request.
 * @param response Empty response message that we'll populate with data.
 * @param user User account that was successfully logged in, which will be serialized into the response message.
 * @return 0 on success, -1 otherwise.
 */
int build_login_response(message_t& response, user_t& user);

/**
 * Parses a successful login response message from the server. It processes the payload.
 * 
 * @param response Response object that represents a successful login response message sent by the server.
 * @param user Empty user struct that will be populated by the callee with the info of the user that was 
 * just logged in with.
 * @return 0 on successful parsing, otherwise a non-zeor response code.
 */
int parse_login_response(message_t& response, user_t& user);

// ---------------------------------
// World Broadcasting
// ---------------------------------

/**
 * Creates a request message for a world broadcast
 * @param request_buffer Empty buffer that'll be populated with the serialized data for the world broadcast request.
 * @param broadcast World broadcast whose data we want to put into the request message.
 * @param message_len Reference that'll be populated with the length of the entire message.
 * @return 0 on success, -1 otherwise.
 * @note Designed to minimize the number of data copies to just the kernel 
 * buffer. Other than it's designed to be used by the client.
 */
int build_world_broadcast(uint8_t* request_buffer, world_broadcast_t& broadcast, uint32_t& message_len);

/**
 * @note Designed to be used by the server. The 
 * build world broadcast notification function needs this function
 */
int build_world_broadcast(message_t& request, world_broadcast_t& broadcast);

/**
 * Parses a request and populate a world broadcast object with its info.
 * 
 * @param request Request message sent by the client that indicates they're doing a world broadcast.
 * @param broadcast World broadcast that we'll fill with data from this request message.
 * @return 0 if parsing is correct, otherwise a response code.
 * 
 * NOTE: This is supposed to be used server-side
 */
int parse_world_broadcast(message_t& request, world_broadcast_t& broadcast);

/**
 * Build a world-broadcast response message. 
 * @param response The response message that we'll fill and write to all other users.
 * @param broadcast the broadcast object that contains the message that they want send
 * to all other users.
 * @note This is used by the server. So client sends world-broadcast and now the server has 
 * to actually send that broadcast to all other users. Here we build the response message 
 * that's sent to all other users. Same as build_world_broadcast_request, meaning the message 
 * format is same for client and server.
 */
int build_world_broadcast_notification(message_t& response, world_broadcast_t& broadcast);

/**
 * Parses a notification for a world broadcast
 * 
 * @param msg World broadcast notification message sent by the server.
 * @param broadcast Broadcast object that'll be populated
 * @return 0 on success, non-zero response code on error.
 * 
 * NOTE: 
 * - Intended to be used by the client to parse world broadcasts. 
 * - Since world broadcasts and world broadcast notifications are the same, the logic for parsing them are the same.
 */
int parse_world_broadcast_notification(message_t& msg, world_broadcast_t& broadcast);

// ---------------------------------
// P2P Broadcasting
// ---------------------------------

/**
 * Creates request message for a p2p broadcast
 * @param request_buffer Empty buffer that'll be populated with the serialized request data.
 * @param broadcast P2P Broadcast object that'll be serialized and stored in the request buffer.
 * @param message_len Reference that'll be populated with the length of the entire message.
 * @return 0 on success, -1 on error.
 * @note This is supposed to be used by the client since they're the one initiating 
 * the p2p broadcast request.
 */
int build_p2p_broadcast(uint8_t* request_buffer, p2p_broadcast_t& broadcast, uint32_t& message_len);

/**
 * @note Needed for build_p2p_broadcast_notification(). Designed to be 
 * used internally by the server.
 */
int build_p2p_broadcast(message_t& request, p2p_broadcast_t& broadcast);

/**
 * Parses a message sent by the client and populates the p2p broadcast object.
 * @param request Request message sent by the client to do a p2p broadcast.
 * @param broadcast Broadcast message that we'll populate with data from the request message
 * @note Supposed to be used by the server.
 */
int parse_p2p_broadcast(message_t& request, p2p_broadcast_t& broadcast);

/**
 * Builds a p2p broadcast response message
 * @param response Response message we're populating data with, which we'll send to the recipient of the P2P response.
 * @param broadcast P2P broadcast sent by the client.
 * @return 0 on success, response code otherwise.
 * @note This is supposed to be used by the server. The client sends a P2P broadcast request, and then 
 * we build a message that we'll send to the recipient. This message will contain the message from the sender.
 * Identical format to creating a P2P broadcast, so client and sender have the same message format.
 */
int build_p2p_broadcast_notification(message_t& response, p2p_broadcast_t& broadcast);

/**
 * Parses a notification for a p2p broadcast
 * 
 * @param msg A P2P broadcast notification message sent by the server
 * @param broadcast The P2P broadcast object we'll parse that message data into
 * @return 0 on success, non-zero code on error.
 */
int parse_p2p_broadcast_notification(message_t& msg, p2p_broadcast_t& broadcast);

/**
 * Given a request message for a chat, return the type of broadcast it wants to do.
 * 
 * @param request The request message sent by the client, where message->type = CHAT. 
 * @return The tag value of the broadcast.
 * 
 * NOTE: Should be used by client and server.
 */
tlv_tag_t peek_broadcast_type(message_t& request);

/**
 * Builds a server response message to be sent to a user.
 * 
 * @param type The response message type to send back to the client.
 * @param rc Response code of the response.
 * @param payload Pointer to start of the buffer that contains the payload data. This is assumed to already have the stream of TLVs already contained in it.
 * @param payload_len Length of the payload buffer
 * @return Response message that represents the server response.
 * 
 * @note For this protocol, the response can reuse the request message type. That means
 * a registration response can be type REGISTER, a login response can be type LOGIN,
 * and so on. This avoids a separate ACK-only type while preserving the response code.
 */
message_t build_server_response(uint8_t type, response_code_t rc, uint8_t *payload, uint32_t payload_len);

