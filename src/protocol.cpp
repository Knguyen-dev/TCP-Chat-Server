#include "protocol.hpp"
#include "logger.hpp"


std::string get_response_message(response_code_t code) {
  switch (code) {
    case RESP_OK: return "Success";
    case RESP_ERROR_MALFORMED: return "Malformed request";
    case RESP_ERROR_USER_EXISTS: return "Username already taken";
    case RESP_ERROR_USER_NOT_FOUND: return "User not found";
    case RESP_ERROR_INVALID_CREDENTIALS: return "Invalid credentials";
    case RESP_ERROR_INTERNAL: return "Internal server error";
    case RESP_ERROR_UNKNOWN_COMMAND: return "Unknown command";
    default: return "Unknown response code (" + std::to_string(static_cast<int>(code)) + ")";
  }
  std::stringstream ss;
  ss << "Unknown response code: " << code << "!";
  return ss.str();
}


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
static void write_tlv(uint8_t* &buf, tlv_tag_t tag, uint8_t len, const void *value, int convert_to_network) {
  
  // 1. Write tag and length (1 byte each, no flipping)
  *buf++ = static_cast<uint8_t>(tag); 
  *buf++ = len;

  // 2. Write the converted (if necessary) byte sequence into the buffer
  if (convert_to_network) {
    // If we want to convert a multi-byte number into network-byte-order. Three cases:
    // a. Converting a 2-byte sequence
    // b. Converting a 4-byte sequence
    // c. Converting a 1-byte sequence (no conversion needed even if the caller wants it)
    if (len == 2) {
      uint16_t val;
      memcpy(&val, value, 2);
      val = htons(val);
      memcpy(buf, &val, 2);
    } else if (len == 4) {
      uint32_t val;
      memcpy(&val, value, 4);
      val = htonl(val);
      memcpy(buf, &val, 4);
    } else {
      memcpy(buf, value, len);
    }
  } else {
    // Else, the caller intends to write a string into the buffer
    memcpy(buf, value, len);
  }

  // 3. Advance buffer pointer by the byte size of the value.
  buf += len;
}

void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
  buf.insert(buf.end(), data, data + len);
}

void buf_consume(std::vector<uint8_t> &buf, size_t n) {
  buf.erase(buf.begin(), buf.begin() + n);
}

int read_one_message(conn_t& conn, message_t& msg) {

  // ##### 1. Read message header and copy it to the conn_t::incoming #####
  uint8_t buffer[1024 * 32];
  uint8_t *ptr = buffer;
  int bytes_to_read = MSG_HEADER_SIZE;
  while (bytes_to_read > 0) {
    int bytes_read = read(conn.fd, ptr, bytes_to_read);
    if (bytes_read == 0) {
      LOG_ERROR("EOF when reading msg header. Remote peer closed!\n");
      return -1;
    }

    // Interrupted by signal handling; not a real error, re-read.
    if (bytes_read == -1 && (errno == EINTR)) {
      continue;
    }

    if (bytes_read == -1) {
      LOG_ERROR("read error when reading msg header: %s\n", strerror(errno));
      return -1;
    }
    ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }

  // ##### 2. Insert data into conn_t::incoming and parse Header Fields into msg_t #####
  // a. bytes 0-2 are version, type, and response code
  // b. bytes 3-6 represent the payload length; handle byte order conversion and validate payload size
  conn.incoming.insert(conn.incoming.end(), buffer, buffer+MSG_HEADER_SIZE);
  msg.version = buffer[0];
  msg.type = buffer[1];
  msg.rc = buffer[2];
  memcpy(&msg.payload_length, buffer+3, sizeof(msg.payload_length));
  msg.payload_length = ntohl(msg.payload_length);
  if (msg.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size '%d' is bigger than maximum!\n", msg.payload_length);
    return -1;
  }

  // ##### Step 3: Read the message payload from peer #####
  // NOTE: Resets pointer to point at the start of the buffer, which will probably overwrite 
  // those 7 header bytes which is fine because those bytes have already been saved into conn_t::incoming
  bytes_to_read = msg.payload_length;
  ptr = &buffer[0];
  while (bytes_to_read > 0) {
    int bytes_read = read(conn.fd, ptr, bytes_to_read);
    if (bytes_read == 0) {
      LOG_ERROR("Unexpected EOF when reading msg payload!\n");
      return -1;
    }
    if (bytes_read == -1) {
      LOG_ERROR("read error when reading msg payload: %s\n", strerror(errno));
      return -1;
    }    
    ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }

  // a. Append message payload to the back conn_t::incoming 
  // b. Update message_t::payload to point at the start of the payload in conn_t::incoming
  // NOTE: An std::vector::insert can cause a reallocation, causing all previous pointers 
  // pointing to it to become dangling. That's why you always insert data first and then start assigning pointers.
  conn.incoming.insert(conn.incoming.end(), buffer, buffer+msg.payload_length);
  size_t payload_offset = conn.incoming.size() - msg.payload_length;
  msg.payload = conn.incoming.data() + payload_offset;

  return 0;
}

int write_one_message(conn_t& conn, message_t& msg) { 
  size_t bytes_sent = 0;
  std::vector<uint8_t>& buf = conn.outgoing;
  size_t bytes_to_send = msg.payload_length+MSG_HEADER_SIZE;
  uint8_t* ptr = buf.data();

  while (bytes_sent < bytes_to_send) {
    int res = write(conn.fd, ptr, bytes_to_send-bytes_sent);
    if (res <= 0) {
      // If signal interruption, then continue looping
      if (res < 0 && errno == EINTR) {
        continue;
      }
        
      // Otherwise an actual error or remote connection closed.
      LOG_ERROR("Failed to write one message with errno '%s'\n", strerror(errno));
      return -1;
    }
    ptr += res;
    bytes_sent += res;    
  }
  
  // Remove those bytes from the beginning of output buffer
  buf.erase(buf.begin(), buf.begin() + bytes_sent);
  return (int)bytes_to_send;
}

void write_message_to_buffer(std::vector<uint8_t>& buffer, message_t& message) {
  size_t total_size = sizeof(message.version) + 
    sizeof(message.type) + 
    sizeof(message.rc) +
    sizeof(message.payload_length) +
    message.payload_length;
  
  // Reallocate buffer size to a new maximum capacity if needed
  buffer.reserve(buffer.size() + total_size);
  buf_append(buffer, &message.version, sizeof(message.version));
  buf_append(buffer, &message.type, sizeof(message.type));
  buf_append(buffer, &message.rc, sizeof(message.rc));

  uint32_t net_payload_len = htonl(message.payload_length);
  buf_append(buffer, (const uint8_t*)&net_payload_len, sizeof(net_payload_len));
  if (message.payload && message.payload_length > 0) {
    // Then read from the payload ptr and read data into output buffer 
    buf_append(buffer, message.payload, message.payload_length);
  }  
}

void parse_message(const std::vector<uint8_t>& buffer, message_t& message) {
  // Parse header fields into message header struct
  // a. bytes 0-2 are version, type, and response code
  // b. bytes 3-6 represent the payload length; handle byte order conversion and validate payload size
  message.version = buffer[0];
  message.type = buffer[1];
  message.rc = buffer[2];
  memcpy(&message.payload_length, &buffer[3], 4);
  message.payload_length = ntohl(message.payload_length);
  message.payload = (uint8_t*)&buffer[MSG_HEADER_SIZE];
}

int build_register_request(message_t& request, registration_credentials_t& credentials) {
  // Validate fields and write into the message payload
  uint8_t* moving_ptr = request.payload;
  if (credentials.username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("Username length (%d) surpasses maximum length (%d)!\n", credentials.username.length(), MAX_USERNAME_SIZE);
    return -1;
  }
  if (credentials.password.length() > MAX_PASSWORD_SIZE) {
    LOG_ERROR("Password length (%d) is bigger than maximum length (%d)!\n", credentials.password.length(), MAX_PASSWORD_SIZE);
    return -1;
  }
  write_tlv(moving_ptr, TAG_USERNAME, credentials.username.length(), credentials.username.data(), 0);
  write_tlv(moving_ptr, TAG_PASSWORD, credentials.password.length(), credentials.password.data(), 0);

  uint32_t payload_len = (uint32_t)(moving_ptr - request.payload);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }

  // Write header fields into message
  request.version = 1;
  request.type = REGISTER;
  request.rc = 0;
  request.payload_length = payload_len;
  return 0;
}

int parse_register_request(message_t& msg, registration_credentials_t &credentials) {
  uint8_t *payload_ptr = msg.payload;
  uint8_t *payload_end = msg.payload + msg.payload_length;
  int has_username = 0;
  int has_password = 0;
  while (payload_ptr < payload_end) {
    if (payload_ptr + 2 > payload_end) {
      // Check if we have at least tag (1byte) + length (1byte).
      LOG_ERROR("Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }

    uint8_t tag = *payload_ptr++;
    uint8_t num_bytes = *payload_ptr++;

    if (payload_ptr + num_bytes > payload_end) {
      // Check to see if we have the full value from the TLV
      LOG_ERROR("Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }

    switch (tag) {
      case TAG_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          LOG_ERROR("Username of length %d surpasses maximum username size %d!\n", num_bytes, MAX_USERNAME_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        credentials.username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_username = 1;
        break;
      case TAG_PASSWORD:
        if (num_bytes > MAX_PASSWORD_SIZE) {
          LOG_ERROR("Password of length %d surpasses maximum password size %d!\n", num_bytes, MAX_PASSWORD_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        credentials.password.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_password = 1;
        break;
      default:
        LOG_ERROR("Unknown TLV tag '%d', skipping %d bytes!\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }

  if (!has_username || !has_password) {
    LOG_ERROR("Missing username or password tag, failed registration!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

int build_login_request(message_t& request, login_credentials_t& credentials) {
  
  // Validate credentials and write into message payload
  uint8_t* moving_ptr = request.payload;
  if (credentials.username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("Username length (%d) surpasses maximum length (%d)!\n", credentials.username.length(), MAX_USERNAME_SIZE);
    return -1;
  }
  if (credentials.password.length() > MAX_PASSWORD_SIZE) {
    LOG_ERROR("Password length (%d) is bigger than maximum length (%d)!\n", credentials.password.length(), MAX_PASSWORD_SIZE);
    return -1;
  }
  write_tlv(moving_ptr, TAG_USERNAME, credentials.username.length(), credentials.username.data(), 0);
  write_tlv(moving_ptr, TAG_PASSWORD, credentials.password.length(), credentials.password.data(), 0);
  
  // Calculate payload length and write header fields into message
  uint32_t payload_len = (uint32_t)(moving_ptr - request.payload);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request.version = 1;
  request.type = LOGIN;
  request.rc = 0;
  request.payload_length = payload_len;
  return 0;
}

int parse_login_request(message_t& request, login_credentials_t& credentials) {
  uint8_t *payload_ptr = request.payload;
  uint8_t *payload_end = payload_ptr + request.payload_length;
  int has_username = 0;
  int has_password = 0;
  while (payload_ptr < payload_end) {
    if (payload_ptr + 2 > payload_end) {
      // Check if we have at least tag (1byte) + length (1byte).
      LOG_ERROR("Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }

    uint8_t tag = *payload_ptr++;
    uint8_t num_bytes = *payload_ptr++;

    if (payload_ptr + num_bytes > payload_end) {
      // Check to see if we have the full value from the TLV
      LOG_ERROR("Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }

    switch (tag) {
      case TAG_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          LOG_ERROR("Username of length %d surpasses maximum username size %d!\n", num_bytes, MAX_USERNAME_SIZE);
          return RESP_ERROR_MALFORMED;
        }

        credentials.username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_username = 1;
        break;
      case TAG_PASSWORD:
        if (num_bytes > MAX_PASSWORD_SIZE) {
          LOG_ERROR("Password of length %d surpasses maximum password size %d!\n", num_bytes, MAX_PASSWORD_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        credentials.password.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_password = 1;
        break;
      default:
        LOG_ERROR("Unknown TLV tag '%d', skipping %d bytes!\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }

  if (!has_username || !has_password) {
    LOG_ERROR("Missing username or password tag, failed login!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

int build_world_broadcast(message_t& request, world_broadcast_t& broadcast) {
  // Write world broadcast's data into the message payload
  uint8_t *moving_ptr = request.payload;
  write_tlv(moving_ptr, TAG_WORLD_BROADCAST, 0, NULL, 0); 
  write_tlv(moving_ptr, TAG_SENDER_USERNAME, broadcast.sender_username.length(), broadcast.sender_username.data(), 0);
  write_tlv(moving_ptr, TAG_MESSAGE_CONTENT, broadcast.message_content.length(), broadcast.message_content.data(), 0);
  
  // Calculate payload and write header fields into message
  uint32_t payload_len = (uint32_t)(moving_ptr - request.payload);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("World Message Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request.version = 1;
  request.type = CHAT;
  request.rc = 0;
  request.payload_length = payload_len;
  return 0;
}

int parse_world_broadcast(message_t& request, world_broadcast_t& broadcast) {
  uint8_t* payload_ptr = request.payload;
  uint8_t* payload_end = payload_ptr + request.payload_length;
  int has_broadcast_tag = 0;
  int has_sender = 0;
  int has_content = 0;
  while (payload_ptr < payload_end) {
    if (payload_ptr + 2 > payload_end) {
      // Check if we have at least tag (1byte) + length (1byte).
      LOG_ERROR("Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }

    uint8_t tag = *payload_ptr++;
    uint8_t num_bytes = *payload_ptr++;

    if (payload_ptr + num_bytes > payload_end) {
      // Check to see if we have the full value from the TLV
      LOG_ERROR("Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }

    switch (tag) {
      case TAG_WORLD_BROADCAST:
        has_broadcast_tag = 1;
        break;
      case TAG_SENDER_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          LOG_ERROR("Malformed TLV: username length exceeds limits!\n");
          return RESP_ERROR_MALFORMED;
        }
        broadcast.sender_username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_sender = 1;
        break;
      case TAG_MESSAGE_CONTENT:
        if (num_bytes > MAX_MSG_CONTENT_SIZE) {
          LOG_ERROR("Malformed TLV: message content length exceeds limits!\n");
          return -1;
        }
        broadcast.message_content.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_content = 1;
        break;
      default:
        LOG_WARN("Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }
  if (!has_broadcast_tag || !has_sender || !has_content) {
    LOG_ERROR("Malformed request: broadcast, sender, or content tag missing from request payload!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

int build_world_broadcast_notification(message_t& response, world_broadcast_t& broadcast) {
  return build_world_broadcast(response, broadcast);
}

int parse_world_broadcast_notification(message_t& msg, world_broadcast_t& broadcast) {
  return parse_world_broadcast(msg, broadcast);
}

int build_p2p_broadcast(message_t& request, p2p_broadcast_t& broadcast) {
  
  // Validate usernames, write TLVs into payload buffer
  uint8_t *moving_ptr = request.payload;
  if (broadcast.sender_username == broadcast.recipient_username) {
    LOG_ERROR("P2P Message Failed: Sender and Recipient username matched!\n");
    return -1;
  }
  write_tlv(moving_ptr, TAG_P2P_BROADCAST, 0, NULL, 0); 
  write_tlv(moving_ptr, TAG_SENDER_USERNAME, broadcast.sender_username.length(), broadcast.sender_username.data(), 0);
  write_tlv(moving_ptr, TAG_RECIPIENT_USERNAME, broadcast.recipient_username.length(), broadcast.recipient_username.data(), 0);
  write_tlv(moving_ptr, TAG_MESSAGE_CONTENT, broadcast.message_content.length(), broadcast.message_content.data(), 0);
  
  // Calculate payload and write header fields into message
  uint32_t payload_len = (uint32_t)(moving_ptr - request.payload);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("P2P Message Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request.version = 1;
  request.type = CHAT;
  request.rc = 0;
  request.payload_length = payload_len;

  return 0;
}

int parse_p2p_broadcast(message_t& request, p2p_broadcast_t& broadcast) {  
  uint8_t* payload_ptr = request.payload;
  uint8_t *payload_end = payload_ptr + request.payload_length;
  int has_broadcast_tag = 0;
  int has_sender = 0;
  int has_recipient = 0;
  int has_content = 0;

  while (payload_ptr < payload_end) {
    if (payload_ptr + 2 > payload_end) {
      LOG_ERROR("Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }

    uint8_t tag = *payload_ptr++;
    uint8_t num_bytes = *payload_ptr++;

    if (payload_ptr + num_bytes > payload_end) {
      LOG_ERROR("Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }

    switch (tag) {
      case TAG_P2P_BROADCAST:
        has_broadcast_tag = 1;
        break;
      case TAG_SENDER_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          LOG_ERROR("Malformed TLV: sender username length exceeds limits!\n");
          return RESP_ERROR_MALFORMED;
        }
        broadcast.sender_username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_sender = 1;
        break;
      case TAG_RECIPIENT_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          LOG_ERROR("Malformed TLV: recipient username length exceeds limits!\n");
          return RESP_ERROR_MALFORMED;
        }
        broadcast.recipient_username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_recipient = 1;
        break;
      case TAG_MESSAGE_CONTENT:
        if (num_bytes > MAX_MSG_CONTENT_SIZE) {
          LOG_ERROR("Malformed TLV: message content length exceeds limits!\n");
          return -1;
        }
        broadcast.message_content.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_content = 1;
        break;
      default:
        LOG_WARN("Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }
  if (!has_broadcast_tag || !has_sender || !has_recipient || !has_content) {
    LOG_ERROR("Malformed request: broadcast, sender, recipient or content tag missing from request payload!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

int build_p2p_broadcast_notification(message_t& response, p2p_broadcast_t& broadcast) {
  return build_p2p_broadcast(response, broadcast);
}

int parse_p2p_broadcast_notification(message_t& msg, p2p_broadcast_t& broadcast) {
  return parse_p2p_broadcast(msg, broadcast);
}

tlv_tag_t peek_broadcast_type(message_t& request) {
  // NOTE: Assumes the broadcast type is the first byte in the message payload.
  tlv_tag_t tag = static_cast<tlv_tag_t>(*request.payload);
  return tag;
}

message_t build_server_response(response_code_t rc, uint8_t *payload, uint32_t payload_len) {
  message_t response;
  response.version = 1;
  response.type = ACK;
  response.rc = rc;
  response.payload = payload;
  response.payload_length = payload_len;
  return response;
}
