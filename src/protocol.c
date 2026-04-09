#include "protocol.h"

const char* response_messages[] = {
  [RESP_OK] = "Success",
  [RESP_ERROR_MALFORMED] = "Malformed request",
  [RESP_ERROR_USER_EXISTS] = "Username already taken",
  [RESP_ERROR_USER_NOT_FOUND] = "User not found",
  [RESP_ERROR_INVALID_CREDENTIALS] = "Invalid credentials",
  [RESP_ERROR_INTERNAL] = "Internal server error",
  [RESP_ERROR_UNKNOWN_COMMAND] = "Unknown command"
};

void write_tlv(uint8_t **buf, tlv_tag_t tag, uint8_t len, const void *value, int convert_to_network) {
  
  // 1. Write tag and length (1 byte each, no flipping)
  *(*buf)++ = tag; // equivalent to **buf = tag, then (*buf)++
  *(*buf)++ = len;

  // 2. Write the convered (if necessary) byte seqeunce into the buffer
  if (convert_to_network) {
    // If we want to convert a multi-byte number into network-byte-order. Three cases:
    // a. Converting a 2-byte sequence
    // b. Converting a 4-byte sequence
    // c. Converting a 1-byte sequence (no conversion needed even if the caller wants it)
    if (len == 2) {
      uint16_t val;
      memcpy(&val, value, 2);
      val = htons(val);
      memcpy(*buf, &val, 2);
    } else if (len == 4) {
      uint32_t val;
      memcpy(&val, value, 4);
      val = htonl(val);
      memcpy(*buf, &val, 4);
    } else {
      memcpy(*buf, value, len);
    }
  } else {
    // Else, the caller intends to write a string into the buffer
    memcpy(*buf, value, len);
  }

  // 3. Advance buffer pointer by the byte size of the value.
  *buf += len;
}

int read_one_message(int connfd, message_t* msg) {
  
  // 1. Read message header and copy it to the data_buf
  uint8_t header[MSG_HEADER_SIZE];
  int bytes_to_read = MSG_HEADER_SIZE;
  uint8_t *buf_ptr = header;
  while (bytes_to_read > 0) {
    int bytes_read = read(connfd, buf_ptr, bytes_to_read);
    if (bytes_read == 0) {
      LOG_ERROR("Unexpected EOF when reading msg header!\n");
      return -1;
    }
    if (bytes_read == -1) {
      LOG_ERROR("read error when reading msg header: %s\n", strerror(errno));
      return -1;
    }
    buf_ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }

  // Read header into our message buffer
  memcpy(msg->data_buf, header, MSG_HEADER_SIZE);

  // 2. Parse Header Fields into msg_t
  // a. bytes 0-2 are version, type, and response code
  // b. bytes 3-6 represent the payload length; handle byte order conversion and validate payload size
  msg->version = msg->data_buf[0];
  msg->type = msg->data_buf[1];
  msg->rc = msg->data_buf[2];
  uint32_t payload_length;
  memcpy(&payload_length, &msg->data_buf[3], 4); 
  msg->payload_length = ntohl(payload_length);
  if (msg->payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("read_one_message: Payload of size '%d' is bigger than maximum!\n", msg->payload_length);
    return -1;
  }

  // 3. Read payload into data_buf; following header fields
  bytes_to_read = msg->payload_length;
  buf_ptr = msg->data_buf + MSG_HEADER_SIZE;
  while (bytes_to_read > 0) {
    int bytes_read = read(connfd, buf_ptr, bytes_to_read);
    if (bytes_read == 0) {
      LOG_ERROR("Unexpected EOF when reading msg payload!\n");
      return -1;
    }
    if (bytes_read == -1) {
      LOG_ERROR("read error when reading msg payload: %s\n", strerror(errno));
      return -1;
    }    
    buf_ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }

  msg->data_len = MSG_HEADER_SIZE + msg->payload_length;
  return 0;
}

int write_one_message(int connfd, message_t* response) { 
  // Send the data_buf directly, which contains header + payload in network byte order

  size_t bytes_sent = 0;
  size_t bytes_to_send = response->payload_length+MSG_HEADER_SIZE;
  while (bytes_sent < bytes_to_send) {
    int res = send(connfd, (char*)response->data_buf+bytes_sent, bytes_to_send-bytes_sent, 0);
    if (res <= 0) {
      // If signal interruption, then continue looping
      if (res < 0 && (errno == EINTR)) {
        continue;
      } else {
        // Otherwise an actual error or connection closed
        LOG_ERROR("Failed to write one message with errno '%s'\n", strerror(errno));
        return -1;
      }
    }
    bytes_sent += res;
  }
  return bytes_to_send;
}

int build_register_request(message_t *request, registration_credentials_t *credentials) {
  request->version = 1;
  request->type = REGISTER;
  request->rc = 0;

  // Validate credentials and write them into the payload of the data_buf
  uint8_t *payload_start = request->data_buf + MSG_HEADER_SIZE;
  uint8_t* moving_ptr = payload_start;

  size_t username_len = strlen(credentials->username);
  size_t password_len = strlen(credentials->password);

  if (username_len > MAX_USERNAME_SIZE) {
    LOG_ERROR("Username length (%d) surpasses maximum length (%d)!\n", username_len, MAX_USERNAME_SIZE);
    return -1;
  }
  if (password_len > MAX_PASSWORD_SIZE) {
    LOG_ERROR("Password length (%d) is bigger than maximum length (%d)!\n", password_len, MAX_PASSWORD_SIZE);
    return -1;
  }

  write_tlv(&moving_ptr, TAG_USERNAME, username_len, credentials->username, 0);
  write_tlv(&moving_ptr, TAG_PASSWORD, password_len, credentials->password, 0);
  uint32_t payload_len = (uint32_t)(moving_ptr - payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request->payload_length = payload_len;

  // Write message header to data_buf
  request->data_buf[0] = request->version;
  request->data_buf[1] = request->type;
  request->data_buf[2] = request->rc;
  uint32_t net_len = htonl(request->payload_length);
  memcpy(&request->data_buf[3], &net_len, sizeof(uint32_t));
  request->data_len = MSG_HEADER_SIZE + payload_len;

  return 0;
}

int parse_register_request(message_t *msg, registration_credentials_t *credentials) {
  uint8_t *payload_ptr = msg->data_buf + MSG_HEADER_SIZE;
  uint8_t *payload_end = payload_ptr + msg->payload_length;
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
        memcpy(credentials->username, payload_ptr, num_bytes);
        credentials->username[num_bytes] = '\0';
        has_username = 1;
        break;
      case TAG_PASSWORD:
        if (num_bytes > MAX_PASSWORD_SIZE) {
          LOG_ERROR("Password of length %d surpasses maximum password size %d!\n", num_bytes, MAX_PASSWORD_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        memcpy(credentials->password, payload_ptr, num_bytes);
        credentials->password[num_bytes] = '\0';
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

int build_login_request(message_t *request, login_credentials_t *credentials) {
  request->version = 1;
  request->type = LOGIN;
  request->rc = 0;

  // Validate credentials
  uint8_t *payload_start = request->data_buf + MSG_HEADER_SIZE;
  uint8_t* moving_ptr = payload_start;

  size_t username_len = strlen(credentials->username);
  size_t password_len = strlen(credentials->password);

  if (username_len > MAX_USERNAME_SIZE) {
    LOG_ERROR("Username length (%d) surpasses maximum length (%d)!\n", username_len, MAX_USERNAME_SIZE);
    return -1;
  }
  if (password_len > MAX_PASSWORD_SIZE) {
    LOG_ERROR("Password length (%d) is bigger than maximum length (%d)!\n", password_len, MAX_PASSWORD_SIZE);
    return -1;
  }

  // Write credenitals into our payload 
  write_tlv(&moving_ptr, TAG_USERNAME, username_len, credentials->username, 0);
  write_tlv(&moving_ptr, TAG_PASSWORD, password_len, credentials->password, 0);
  uint32_t payload_len = (uint32_t)(moving_ptr - payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request->payload_length = payload_len;
  request->data_len = MSG_HEADER_SIZE + payload_len;

  // Write header fields to message data buffer
  request->data_buf[0] = request->version;
  request->data_buf[1] = request->type;
  request->data_buf[2] = request->rc;
  uint32_t net_len = htonl(request->payload_length);
  memcpy(&request->data_buf[3], &net_len, sizeof(uint32_t));
  return 0;
}

int parse_login_request(message_t *request, login_credentials_t *credentials) {
  uint8_t *payload_ptr = request->data_buf + MSG_HEADER_SIZE;
  uint8_t *payload_end = payload_ptr + request->payload_length;
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
        memcpy(credentials->username, payload_ptr, num_bytes);
        credentials->username[num_bytes] = '\0';
        has_username = 1;
        break;
      case TAG_PASSWORD:
        if (num_bytes > MAX_PASSWORD_SIZE) {
          LOG_ERROR("Password of length %d surpasses maximum password size %d!\n", num_bytes, MAX_PASSWORD_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        memcpy(credentials->password, payload_ptr, num_bytes);
        credentials->password[num_bytes] = '\0';
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

int build_world_broadcast(message_t *request, world_broadcast_t *broadcast) {
  request->version = 1;
  request->type = CHAT;
  request->rc = 0;

  // Write world broadcast's data into the message payload
  uint8_t *payload_start = request->data_buf + MSG_HEADER_SIZE;
  uint8_t *moving_ptr = payload_start;
  
  write_tlv(&moving_ptr, TAG_WORLD_BROADCAST, 0, NULL, 0); 
  write_tlv(&moving_ptr, TAG_SENDER_USERNAME, strlen(broadcast->sender_username), broadcast->sender_username, 0);
  write_tlv(&moving_ptr, TAG_MESSAGE_CONTENT, strlen(broadcast->message_content), broadcast->message_content, 0);
  uint32_t payload_len = (uint32_t)(moving_ptr - payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("World Message Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request->payload_length = payload_len;

  // Write header fields into the message's data buffer
  request->data_buf[0] = request->version;
  request->data_buf[1] = request->type;
  request->data_buf[2] = request->rc;
  uint32_t net_len = htonl(request->payload_length);
  memcpy(&request->data_buf[3], &net_len, sizeof(uint32_t));
  request->data_len = MSG_HEADER_SIZE + payload_len;

  return 0;
}

int parse_world_broadcast(message_t *msg, world_broadcast_t *broadcast) {
  uint8_t *payload_ptr = msg->data_buf + MSG_HEADER_SIZE;
  uint8_t *payload_end = payload_ptr + msg->payload_length;
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
        memcpy(broadcast->sender_username, payload_ptr, num_bytes);
        broadcast->sender_username[num_bytes] = '\0';
        has_sender = 1;
        break;
      case TAG_MESSAGE_CONTENT:
        if (num_bytes > MAX_MSG_CONTENT_SIZE) {
          LOG_ERROR("Malformed TLV: message content length exceeds limits!\n");
          return -1;
        }
        memcpy(broadcast->message_content, payload_ptr, num_bytes);
        broadcast->message_content[num_bytes] = '\0';
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

int build_world_broadcast_notification(message_t *response, world_broadcast_t *broadcast) {
  return build_world_broadcast(response, broadcast);
}

int parse_world_broadcast_notification(message_t *msg, world_broadcast_t *broadcast) {
  return parse_world_broadcast(msg, broadcast);
}

int build_p2p_broadcast(message_t *request, p2p_broadcast_t *broadcast) {
  request->version = 1;
  request->type = CHAT;
  request->rc = 0;
  uint8_t *payload_start = request->data_buf + MSG_HEADER_SIZE;
  uint8_t *moving_ptr = payload_start;
  
  // Validate usernames, write TLVs into payload buffer
  if (strcmp(broadcast->sender_username, broadcast->recipient_username) == 0) {
    LOG_ERROR("P2P Message Failed: Sender and Recipient username matched!\n");
    return -1;
  }
  write_tlv(&moving_ptr, TAG_P2P_BROADCAST, 0, NULL, 0); 
  write_tlv(&moving_ptr, TAG_SENDER_USERNAME, strlen(broadcast->sender_username), broadcast->sender_username, 0);
  write_tlv(&moving_ptr, TAG_RECIPIENT_USERNAME, strlen(broadcast->recipient_username), broadcast->recipient_username, 0);
  write_tlv(&moving_ptr, TAG_MESSAGE_CONTENT, strlen(broadcast->message_content), broadcast->message_content, 0);
  uint32_t payload_len = (uint32_t)(moving_ptr - payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("P2P Message Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request->payload_length = payload_len;

  // Write header to data buffer
  request->data_buf[0] = request->version;
  request->data_buf[1] = request->type;
  request->data_buf[2] = request->rc;
  uint32_t net_len = htonl(request->payload_length);
  memcpy(&request->data_buf[3], &net_len, sizeof(uint32_t));
  request->data_len = MSG_HEADER_SIZE + payload_len;

  return 0;
}

int parse_p2p_broadcast(message_t *msg, p2p_broadcast_t *broadcast) {
  uint8_t *payload_ptr = msg->data_buf + MSG_HEADER_SIZE;
  uint8_t *payload_end = payload_ptr + msg->payload_length;
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
        memcpy(broadcast->sender_username, payload_ptr, num_bytes);
        broadcast->sender_username[num_bytes] = '\0';
        has_sender = 1;
        break;
      case TAG_RECIPIENT_USERNAME:
        if (num_bytes > MAX_USERNAME_SIZE) {
          fprintf(stderr, "Malformed TLV: recipient username length exceeds limits!\n");
          return RESP_ERROR_MALFORMED;
        }
        memcpy(broadcast->recipient_username, payload_ptr, num_bytes);
        broadcast->recipient_username[num_bytes] = '\0';
        has_recipient = 1;
        break;
      case TAG_MESSAGE_CONTENT:
        if (num_bytes > MAX_MSG_CONTENT_SIZE) {
          fprintf(stderr, "Malformed TLV: message content length exceeds limits!\n");
          return -1;
        }
        memcpy(broadcast->message_content, payload_ptr, num_bytes);
        broadcast->message_content[num_bytes] = '\0';
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

int build_p2p_broadcast_notification(message_t *response, p2p_broadcast_t *broadcast) {
  return build_p2p_broadcast(response, broadcast);
}

int parse_p2p_broadcast_notification(message_t *msg, p2p_broadcast_t *broadcast) {
  return parse_p2p_broadcast(msg, broadcast);
}

int peek_broadcast_type(message_t *msg) {
  uint8_t *buf_ptr = msg->data_buf + MSG_HEADER_SIZE;
  tlv_tag_t tag = *buf_ptr; // Assumes the broadcast type is the first 1 byte tag we see in the payload
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
