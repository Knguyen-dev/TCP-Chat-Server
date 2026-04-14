#include "protocol.hpp"

const char* response_messages[] = {
  [RESP_OK] = "Success",
  [RESP_ERROR_MALFORMED] = "Malformed request",
  [RESP_ERROR_USER_EXISTS] = "Username already taken",
  [RESP_ERROR_USER_NOT_FOUND] = "User not found",
  [RESP_ERROR_INVALID_CREDENTIALS] = "Invalid credentials",
  [RESP_ERROR_INTERNAL] = "Internal server error",
  [RESP_ERROR_UNKNOWN_COMMAND] = "Unknown command"
};


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

// TODO: Re-add blocking read and blocking writes. This would be useful for the client
// side 





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
        // Unknown tag so skip it
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
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
      fprintf(stderr, "Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }

    uint8_t tag = *payload_ptr++;
    uint8_t num_bytes = *payload_ptr++;

    if (payload_ptr + num_bytes > payload_end) {
      fprintf(stderr, "Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }

    switch (tag) {
      case TAG_P2P_BROADCAST:
        has_broadcast_tag = 1;
        break;
      case TAG_SENDER_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          fprintf(stderr, "Malformed TLV: sender username length exceeds limits!\n");
          return RESP_ERROR_MALFORMED;
        }
        broadcast.sender_username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_sender = 1;
        break;
      case TAG_RECIPIENT_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          fprintf(stderr, "Malformed TLV: recipient username length exceeds limits!\n");
          return RESP_ERROR_MALFORMED;
        }
        broadcast.recipient_username.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_recipient = 1;
        break;
      case TAG_MESSAGE_CONTENT:
        if (num_bytes > MAX_MSG_CONTENT_SIZE) {
          fprintf(stderr, "Malformed TLV: message content length exceeds limits!\n");
          return -1;
        }
        broadcast.message_content.assign(reinterpret_cast<const char*>(payload_ptr), num_bytes);
        has_content = 1;
        break;
      default:
        // Unknown tag so skip it
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }
  if (!has_broadcast_tag || !has_sender || !has_recipient || !has_content) {
    fprintf(stderr, "Malformed request: broadcast, sender, recipient or content tag missing from request payload!\n");
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

int build_server_response(message_t* response, response_code_t rc, uint8_t *data_buf, uint32_t buf_len) {
  // Build Header fields
  response->version = 1;
  response->type = ACK;
  response->rc = rc;

  // Build Message Payload using data_buf; payload should be 
  uint8_t *data_buf_end = data_buf + buf_len;
  uint8_t *payload_start = response->data_buf + MSG_HEADER_SIZE;
  uint8_t* moving_ptr = payload_start;

  while (data_buf < data_buf_end) {
    tlv_tag_t tag = *data_buf++;
    uint8_t length = *data_buf++;
    uint8_t value_buf[length];
    memcpy(value_buf, data_buf, length);
    data_buf += length;

    // TODO: Figure out a way to encode the endianness of each TLV value in the data_buf.
    write_tlv(&moving_ptr, tag, length, (const void*)value_buf, 0);
  }

  // verify and update payload length
  uint32_t payload_len = (uint32_t)(moving_ptr - payload_start);
  response->payload_length = payload_len;
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Creating payload of size '%d', but maximum is '%d'\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }

  // Write header to data_buf
  response->data_buf[0] = response->version;
  response->data_buf[1] = response->type;
  response->data_buf[2] = response->rc;
  uint32_t net_len = htonl(response->payload_length);
  memcpy(&response->data_buf[3], &net_len, sizeof(uint32_t));
  response->data_len = MSG_HEADER_SIZE + payload_len;

  return 0;
}
