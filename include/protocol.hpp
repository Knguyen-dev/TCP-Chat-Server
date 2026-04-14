
#include "shared.hpp"

#define MSG_HEADER_SIZE 7 // max header size in bytes
#define MSG_MAX_PAYLOAD_SIZE 4096 // max payload size in bytes

// Response messages corresponding with response codes 
extern const char* response_messages[];

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
  REGISTER = 0, // Registration requests
  LOGIN = 1,    // Login requests
  CHAT = 2,     // Requests to chat; 
  ACK = 3,      // Messages that we'll send in response to the user; big umbrella, could be improved but it makes sense
  LIST = 4,     // For requests to list users.
} msg_type_t;

/**
 * Response codes the server can send to the client
 * NOTE: Codes specific only to ACK/response messages
 */
typedef enum {
  RESP_OK = 0,
  RESP_ERROR_MALFORMED = 1,
  RESP_ERROR_USER_EXISTS = 2, // For "username already taken"
  RESP_ERROR_USER_NOT_FOUND = 3,
  RESP_ERROR_INVALID_CREDENTIALS = 4,
  RESP_ERROR_INTERNAL = 5,
  RESP_ERROR_UNKNOWN_COMMAND = 6
} response_code_t;

/**
 * TLV Tags (Payload Context)
 * NOTE: This should never exceed the range of uint8_t 
 */
typedef enum {
  TAG_USERNAME = 0,
  TAG_PASSWORD = 1,
  TAG_RESPONSE_CODE = 2,
  TAG_RESPONSE_MESSAGE = 3,
  TAG_WORLD_BROADCAST = 4,
  TAG_P2P_BROADCAST = 5,
  TAG_USER_ID = 6,
  TAG_ROOM_ID = 7,
  TAG_IS_AUTH = 8,
  TAG_SENDER_USERNAME = 9,
  TAG_RECIPIENT_USERNAME = 10,
  TAG_MESSAGE_CONTENT = 11,
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
 * Parses a message and places field information into a message struct
 * @param buffer Read-only buffer of data that contains the message header.
 * @param message Reference to the message struct that we're going to populate with parsed header information.
 * 
 * NOTE: Assumes buffer has the first MSG_HEADER_SIZE + 1 bytes of a message.
 */
void parse_message(const std::vector<uint8_t>& buffer, message_t& message);

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
 * the fields and payload in the response message to already be 
 * in network byte order.
 */
int write_one_message(int connfd, message_t* response);

/**
 * Builds a user registration request
 * @param request Request message that we're going to populate with user registration data.
 * @param credentials Struct containing the user registration credentials that the user inputted in.
 * @return 0 on success, -1 on error 
 * NOTE: Used by the client to create a registration request message.
 */
int build_register_request(message_t& request, registration_credentials_t& credentials);

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
 * Builds a login request message using login credentials
 * 
 * @param request Request message that we'll populate.
 * @param credentials Credentials that the user created that we'll use to create the login request message.
 * @return 0 on success, -1 on error
 * 
 * NOTE: Intended to be used by the client
 */
int build_login_request(message_t& request, login_credentials_t& credentials);

/**
 * Parses a login request message and populates credentials object.
 * @param request Login request message sent by the client.
 * @param credentials Credentials object that we'll populate after parsing the request message object.
 * @return 0 on success, non-zero response code on error.
 * 
 * NOTE: Intended to be used by the server.
 */
int parse_login_request(message_t& request, login_credentials_t& credentials);

/**
 * Creates a request message for a world broadcast
 * 
 * @param request Request message that'll be populated with data.
 * @param broadcast World broadcast whose data we want to put into the request message.
 * @return 0 on success, -1 otherwise.
 * 
 * NOTE: This is supposed to be used by the client since the client is the one initiating 
 * the world broadcast request.
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
 * 
 * @param response The response message that we'll fill and write to all other users.
 * @param broadcast the broadcast object that contains the message that they want send
 * to all other users.
 * 
 * NOTE: 
 * - This is used by the server. So client sends world-broadcast and now the server has 
 * to actually send that broadcast to all other users. Here we build the response message 
 * that's sent to all other users.
 * - Same as build_world_broadcast_request, meaning the message format is same for client and server.
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

/**
 * Creates request message for a p2p broadcast
 * @param request Request object to be populated with broadcast information
 * @param broadcast Broadcast object that the client filled.
 * @return 0 on success, non-zero return code on error.
 * 
 * NOTE: This is supposed to be used by the client since they're the one initiating 
 * the p2p broadcast request.
 */
int build_p2p_broadcast(message_t& request, p2p_broadcast_t& broadcast);

/**
 * Parses a message sent by the client and populates the p2p broadcast object.
 * @param request Request message sent by the client to do a p2p broadcast.
 * @param broadcast Broadcast message that we'll populate with data from the request message
 * 
 * NOTE: Supposed to be used by the server
 */
int parse_p2p_broadcast(message_t& request, p2p_broadcast_t& broadcast);

/**
 * Builds a p2p broadcast response message
 * 
 * @param response Response message we're populating data with, which we'll send to the recipient of the P2P response.
 * @param broadcast P2P broadcast sent by the client.
 * @return 0 on success, response code otherwise.
 * 
 * NOTE: 
 * - This is supposed to be used by the server. The client sends a P2P broadcast request, and then 
 * we build a message that we'll send to the recipient. This message will contain the message from the sender.
 * - Identical format to creating a P2P broadcast, so client and sender have the same message format.
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
 * Builds a server response message to be sent to a user, an ACK!
 * 
 * @param response Pointer to response message struct that this function will populate.
 * @param rc Response code of the response.
 * @param data_buf Pointer to the start of the buffer that contains data we'll fill the payload with.
 * @param data_buf_len Length of the data_buf in bytes.
 * 
 * NOTE: For our TCP server, there'll be a couple types of messages that the server 
 * sends back to the client. The most prominent will be the broadcast notification messages obviously, where 
 * we're transmitting a sender's message across the wire to one or more remote recipients. The other type 
 * of message that a server will send is an ACK, which tells the client that their request to do an action
 * was successful! This function builds the ACK message! 
 */
int build_server_response(message_t* response, response_code_t rc, uint8_t *data_buf, uint32_t data_buf_len);


/**
 * Parses a server ACK message
 * 
 * TODO: May need to finish this
 * NOTE: 'Updating it' may include turning fields into host byte order and parsing payload.
 */
int parse_server_response(message_t* server_response);