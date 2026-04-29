
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

  // TODO: Timestamps could be a future feature
  TAG_TIMESTAMP = 12
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
 * @param conn Connection that read from and appending data to the conn_t::incoming.
 * @param msg Message struct that'll be populated which'll contain readable information about the processed message.
 * @return 0 on success, otherwise -1.
 * 
 * @note Appends the full message to conn_t::incoming and parses 
 * the important header fields and pointer to the payload into 
 * the msg object. The pointer to the payload points to inside the conn_t::incoming 
 * buffer.
 */
int read_one_message(conn_t& conn, message_t& msg);

/**
 * Performs a blocking write to write one full message from the conn_t::outgoing into the socket
 * @param conn Connection that we're going to write from.
 * @param msg The response message that we're writing to the remote peer
 * @return Number of bytes written on success, otherwise -1 on failure.
 * @note This assumes that you have a full message already serialized in conn_t::outgoing 
 * buffer. It'll write the message from that and remove the emssage from the buffer after 
 * to keep things clean. The `msg` representing the more 'human-readable' request is needed 
 * for some metadata!
 * 
 * This is going to be used by the client for a typical request-response cycle.
 * The server has its own non-blocking version that fits its architecture.
 */
int write_one_message(conn_t& conn, message_t& msg);

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
 * @param rc Response code of the response.
 * @param payload Pointer to start of the buffer that contains the payload data. This is assumed to already have the stream of TLVs already contained in it.
 * @param payload_len Length of the payload buffer
 * @return Response message that represent an ACK
 * 
 * NOTE: For our TCP server, there'll be a couple types of messages that the server 
 * sends back to the client. The most prominent will be the broadcast notification messages obviously, where 
 * we're transmitting a sender's message across the wire to one or more remote recipients. The other type 
 * of message that a server will send is an ACK, which tells the client that their request to do an action
 * was successful! This function builds the ACK message! 
 */
message_t build_server_response(response_code_t rc, uint8_t *payload, uint32_t payload_len);


/**
 * Parses a server ACK message
 * 
 * TODO: May need to finish this
 * NOTE: 'Updating it' may include turning fields into host byte order and parsing payload.
 */
int parse_server_response(message_t* server_response);