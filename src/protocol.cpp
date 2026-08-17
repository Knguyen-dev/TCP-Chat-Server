#include "protocol.hpp"
#include "logger.hpp"  // for LOG_ERROR, LOG_WARN
#include "shared.hpp"  // for MAX_USERNAME_SIZE, user_t, MAX_PASSWORD_SIZE
#include <arpa/inet.h> // for htonl, ntohl, htons
#include <cstdint>     // for uint8_t, uint32_t, uint16_t
#include <cstring>     // for memcpy, strerror
#include <errno.h>     // for errno, EINTR
#include <sstream>     // for basic_ostream, operator<<, basic_stringstream
#include <unistd.h>    // for read, ssize_t, write

// ---------------------------------
// Utilities
// ---------------------------------

std::string get_response_message(response_code_t code) {
  switch (code) {
  case RESP_OK:
    return "Success";
  case RESP_ERROR_MALFORMED:
    return "Malformed request";
  case RESP_ERROR_USER_EXISTS:
    return "Username already taken";
  case RESP_ERROR_USER_NOT_FOUND:
    return "User not found";
  case RESP_ERROR_INVALID_CREDENTIALS:
    return "Invalid credentials";
  case RESP_ERROR_INTERNAL:
    return "Internal server error";
  case RESP_ERROR_UNKNOWN_COMMAND:
    return "Unknown command";
  default:
    return "Unknown response code (" + std::to_string(static_cast<int>(code)) +
           ")";
  }
  std::stringstream ss;
  ss << "Unknown response code: " << code << "!";
  return ss.str();
}

void write_tlv(uint8_t *&buf, tlv_tag_t tag, uint8_t len, const void *value,
               int convert_to_network) {

  // 1. Write tag and length (1 byte each, no flipping)
  *buf++ = static_cast<uint8_t>(tag);
  *buf++ = len;

  // 2. Write the converted (if necessary) byte sequence into the buffer
  if (convert_to_network) {
    // If we want to convert a multi-byte number into network-byte-order. Three
    // cases: a. Converting a 2-byte sequence b. Converting a 4-byte sequence c.
    // Converting a 1-byte sequence (no conversion needed even if the caller
    // wants it)
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

  /*
  n is unsigned but iterator arithmetic expects a signed type. Implicitly
  converted unsigned to signed has sign conversion risks. It's saying that our
  unsigned long (8 bytes) n could get interpreted as a negative number (two's
  complement). However that only happens if the leading bit of the unsigned
  value is 1, which would still be extremely as that means n would have to be
  extremely big. Okay, so you just have to tell the compiler "Hey, n will never
  be that big." If you want to be thorough, n is only as big as the
  MSG_HEADER_SIZE + request.payload_length or the number of bytes you read from
  the outgoing buffer.

  The solution is to cast n to the vector's difference type to satisfy the
  compiler's strict check. This tells the compiler "I guarantee n won't be large
  enough to overlofw into a negative value when interpreted via two's
  complement, so ignore the wraning."
  */

  using diff_t = std::vector<unsigned char>::difference_type;
  buf.erase(buf.begin(), buf.begin() + static_cast<diff_t>(n));
}

void write_message_to_buffer(std::vector<uint8_t> &buffer, message_t &message) {
  size_t total_size = sizeof(message.version) + sizeof(message.type) +
                      sizeof(message.rc) + sizeof(message.payload_length) +
                      message.payload_length;

  // Reallocate buffer size to a new maximum capacity if needed
  buffer.reserve(buffer.size() + total_size);
  buf_append(buffer, &message.version, sizeof(message.version));
  buf_append(buffer, &message.type, sizeof(message.type));
  buf_append(buffer, &message.rc, sizeof(message.rc));

  uint32_t net_payload_len = htonl(message.payload_length);
  buf_append(buffer, (const uint8_t *)&net_payload_len,
             sizeof(net_payload_len));
  if (message.payload && message.payload_length > 0) {
    // Then read from the payload ptr and read data into output buffer
    buf_append(buffer, message.payload, message.payload_length);
  }
}

void parse_message(const std::vector<uint8_t> &buffer, message_t &message) {
  // Parse header fields into message header struct
  // a. bytes 0-2 are version, type, and response code
  // b. bytes 3-6 represent the payload length; handle byte order conversion and
  // validate payload size
  message.version = buffer[0];
  message.type = buffer[1];
  message.rc = buffer[2];
  memcpy(&message.payload_length, &buffer[3], 4);
  message.payload_length = ntohl(message.payload_length);
  message.payload = (uint8_t *)&buffer[MSG_HEADER_SIZE];
}

int read_one_message(int fd, message_t &message) {

  // ##### 1. Read message header ONLY and copy it to the conn_t::incoming #####
  uint8_t header_buffer[MSG_HEADER_SIZE];
  uint8_t *ptr = header_buffer;
  size_t bytes_to_read = MSG_HEADER_SIZE;
  while (bytes_to_read > 0) {
    /*
    bytes_to_read is a signed integer whilst the read() syscall
    expects an unsigned long. The compiler's concerned that bytes_to_read
    could potentially represent negative values in our code, and  interpreting
    a negative signed integer as an unsigned integer would make the integer
    positive, which may not be our intent. The solution is to declare
    bytes_to_read is a size_t.

    The next issue is that read() returns a ssize_t (a long int) and truncating
    it to a regular 4 byte int would literally truncate the value. I mean fair,
    for modern C++ practices I'll change the bytes_read to type ssize_t from
    int.

    The last issue is that the decrement operator tries to implicitly convert
    our signed long int into an unsigned long int, when doing the decrement
    operation. The compiler is concerned that bytes_read could represent a
    negative value (e.g., leading 1 bit), and interpreting it as unsigned would
    lead to a warp around to a large positive number. Reason that the only way
    bytes_read is negative is if the syscall returns -1, which we explicitly
    check for. Therefore, ATP in the code, bytes_read is guaranteed to be
    positive for pointer arithmetic. Do a simple static_cast.
    */

    ssize_t bytes_read = read(fd, ptr, bytes_to_read);
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
    bytes_to_read -= static_cast<size_t>(bytes_read);
  }

  // ##### 2. Insert data into conn_t::incoming and parse Header Fields into
  // msg_t ##### a. bytes 0-2 are version, type, and response code b. bytes 3-6
  // represent the payload length; handle byte order conversion and validate
  // payload size
  message.version = header_buffer[0];
  message.type = header_buffer[1];
  message.rc = header_buffer[2];
  memcpy(&message.payload_length, header_buffer + 3,
         sizeof(message.payload_length));
  message.payload_length = ntohl(message.payload_length);
  if (message.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size '%d' is bigger than maximum!\n",
              message.payload_length);
    return -1;
  }

  // ##### Step 3: Read the message payload from peer #####
  // NOTE: Resets pointer to point at the start of the buffer, which will
  // probably overwrite those 7 header bytes which is fine because those bytes
  // have already been saved into conn_t::incoming
  bytes_to_read = message.payload_length;
  ptr = message.payload;
  while (bytes_to_read > 0) {
    ssize_t bytes_read = read(fd, ptr, bytes_to_read);
    if (bytes_read == 0) {
      LOG_ERROR("Unexpected EOF when reading msg payload!\n");
      return -1;
    }
    if (bytes_read == -1) {
      LOG_ERROR("read error when reading msg payload: %s\n", strerror(errno));
      return -1;
    }
    ptr += bytes_read;

    // Guaranteed to be positive, no sign conversion issues here.
    bytes_to_read -= static_cast<size_t>(bytes_read);
  }

  return 0;
}

int write_one_message(int fd, uint8_t *message_buffer, uint32_t message_len) {
  size_t bytes_sent = 0;
  size_t bytes_to_send{message_len};
  while (bytes_sent < bytes_to_send) {
    ssize_t res = write(fd, message_buffer, bytes_to_send - bytes_sent);
    if (res <= 0) {
      // If signal interruption, then continue looping
      if (res < 0 && errno == EINTR) {
        continue;
      }
      // Otherwise an actual error or remote connection closed.
      LOG_ERROR("Failed to write one message with errno '%s'\n",
                strerror(errno));
      return -1;
    }
    message_buffer += res;

    // No sign conversion issues as res is guaranteed to be positive here
    bytes_sent += static_cast<size_t>(res);
  }

  return (int)bytes_to_send;
}

tlv_tag_t peek_broadcast_type(message_t &request) {
  // NOTE: Assumes the broadcast type is the first byte in the message payload.
  tlv_tag_t tag = static_cast<tlv_tag_t>(*request.payload);
  return tag;
}

message_t build_server_response(uint8_t type, response_code_t rc,
                                uint8_t *payload, uint32_t payload_len) {
  message_t response;
  response.version = 1;
  response.type = type;
  response.rc = rc;
  response.payload = payload;
  response.payload_length = payload_len;
  return response;
}

// ---------------------------------
// User Registration
// ---------------------------------

int build_register_request(uint8_t *request_buffer,
                           registration_credentials_t &credentials,
                           uint32_t &message_len) {
  uint8_t *header_ptr = request_buffer;
  uint8_t *payload_ptr = request_buffer + MSG_HEADER_SIZE;
  if (credentials.username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("Username length (%d) surpasses maximum length (%d)!\n",
              credentials.username.length(), MAX_USERNAME_SIZE);
    return -1;
  }
  if (credentials.password.length() > MAX_PASSWORD_SIZE) {
    LOG_ERROR("Password length (%d) is bigger than maximum length (%d)!\n",
              credentials.password.length(), MAX_PASSWORD_SIZE);
    return -1;
  }

  // Write Headers: version, type, rc, payload is later
  *header_ptr++ = 1;
  *header_ptr++ = REGISTER;
  *header_ptr++ = 0;

  /*
  The compiler is concerned because we're converting a long (8 bytes) unsigned
  int to uint8_t. This is a narrowing conversion as we're truncating the top 7
  bytes. Let's justify this. When making the protocol, the length of a given
  value in a TLV MUST represented by 1 byte. So there. We also have checks that
  ensure username and password have lengths in range [0, 255]. So while it looks
  dumb, the solution is to static cast which tells the compiler that the upper
  7 bytes are already zeroed out, and so no narrowing happens.
  */

  // Write payload into buffer
  write_tlv(payload_ptr, TAG_USERNAME,
            static_cast<uint8_t>(credentials.username.length()),
            credentials.username.data(), 0);
  write_tlv(payload_ptr, TAG_PASSWORD,
            static_cast<uint8_t>(credentials.password.length()),
            credentials.password.data(), 0);

  /*
  Calculate payload length and write it

  Converting from a long int into an unsigned int. The issue is that the long
  int could represent a negative value and then interpreting it as a unsigned
  value would make it positive, flipping the signs. The pointer arithmetic
  results in a long int, in specific a std::ptrdiff_t (a long int). It's also a
  narrowing conversion. So let's justify this.

  We can guarantee that the result of pointer arithmetic is non-negiatve since
  payload_ptr is further down than request_buffer. We can reason whilst the
  result is typed as a long int (8 bytes), the maximum possible message size is
  4103. This is within the bounds of what a uint32_t can represent, therefore
  there's no possible narrowing. This is safe!

  Therefore the solution for this is a static_cast!

  */
  message_len = static_cast<uint32_t>(payload_ptr - request_buffer);
  uint32_t payload_len = message_len - MSG_HEADER_SIZE;
  uint32_t net_payload_len = htonl(payload_len);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n", payload_len,
              MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }

  memcpy(header_ptr, &net_payload_len, sizeof(net_payload_len));
  return 0;
}

int parse_register_request(message_t &msg,
                           registration_credentials_t &credentials) {
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
        LOG_ERROR("Username of length %d surpasses maximum username size %d!\n",
                  num_bytes, MAX_USERNAME_SIZE);
        return RESP_ERROR_MALFORMED;
      }
      credentials.username.assign(reinterpret_cast<const char *>(payload_ptr),
                                  num_bytes);
      has_username = 1;
      break;
    case TAG_PASSWORD:
      if (num_bytes > MAX_PASSWORD_SIZE) {
        LOG_ERROR("Password of length %d surpasses maximum password size %d!\n",
                  num_bytes, MAX_PASSWORD_SIZE);
        return RESP_ERROR_MALFORMED;
      }
      credentials.password.assign(reinterpret_cast<const char *>(payload_ptr),
                                  num_bytes);
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

int build_register_response(message_t &response, user_t &user) {
  response.version = 1;
  response.type = REGISTER;
  response.rc = RESP_OK;
  uint8_t *moving_ptr = response.payload;
  write_tlv(moving_ptr, TAG_USER_ID, sizeof(user.user_id),
            static_cast<void *>(&user.user_id), 1);

  // NOTE: Again username.length() shuold have its upper 7 bytes already zeroed,
  // so it's not narrowing.
  write_tlv(moving_ptr, TAG_USERNAME,
            static_cast<uint8_t>(user.username.length()), user.username.data(),
            0);

  response.payload_length = (uint32_t)(moving_ptr - response.payload);
  if (response.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n",
              response.payload_length, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

int parse_register_response(message_t &response, user_t &user) {
  uint8_t *payload_ptr = response.payload;
  uint8_t *payload_end = response.payload + response.payload_length;

  int has_id = 0;
  int has_username = 0;
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
    case TAG_USER_ID:
      memcpy(&user.user_id, payload_ptr, num_bytes);
      user.user_id = ntohl(user.user_id);
      has_id = 1;
      break;
    case TAG_USERNAME:
      if (num_bytes > MAX_USERNAME_SIZE) {
        LOG_ERROR("Username of length %d surpasses maximum username size %d!\n",
                  num_bytes, MAX_USERNAME_SIZE);
        return RESP_ERROR_MALFORMED;
      }
      user.username.assign(reinterpret_cast<const char *>(payload_ptr),
                           num_bytes);
      has_username = 1;
      break;
    default:
      LOG_ERROR("Unknown TLV tag '%d', skipping %d bytes!\n", tag, num_bytes);
    }

    payload_ptr += num_bytes;
  }
  if (!has_id || !has_username) {
    LOG_ERROR("Missing user id, username, or password tag!\n");
    return -1;
  }
  return 0;
}

// ---------------------------------
// User Login
// ---------------------------------

int build_login_request(uint8_t *request_buffer,
                        login_credentials_t &credentials,
                        uint32_t &message_len) {
  uint8_t *header_ptr = request_buffer;
  uint8_t *payload_ptr = request_buffer + MSG_HEADER_SIZE;
  if (credentials.username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("Username length (%d) surpasses maximum length (%d)!\n",
              credentials.username.length(), MAX_USERNAME_SIZE);
    return -1;
  }
  if (credentials.password.length() > MAX_PASSWORD_SIZE) {
    LOG_ERROR("Password length (%d) is bigger than maximum length (%d)!\n",
              credentials.password.length(), MAX_PASSWORD_SIZE);
    return -1;
  }

  // Write Headers: version, type, rc, payload is later
  *header_ptr++ = 1;
  *header_ptr++ = LOGIN;
  *header_ptr++ = 0;

  // Write payload into buffer
  write_tlv(payload_ptr, TAG_USERNAME,
            static_cast<uint8_t>(credentials.username.length()),
            credentials.username.data(), 0);
  write_tlv(payload_ptr, TAG_PASSWORD,
            static_cast<uint8_t>(credentials.password.length()),
            credentials.password.data(), 0);

  // Calculate payload length and write it to header
  message_len = static_cast<uint32_t>(payload_ptr - request_buffer);
  uint32_t payload_len = message_len - MSG_HEADER_SIZE;
  uint32_t net_payload_len = htonl(payload_len);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n", payload_len,
              MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  memcpy(header_ptr, &net_payload_len, sizeof(net_payload_len));
  return 0;
}

int parse_login_request(message_t &request, login_credentials_t &credentials) {
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
        LOG_ERROR("Username of length %d surpasses maximum username size %d!\n",
                  num_bytes, MAX_USERNAME_SIZE);
        return RESP_ERROR_MALFORMED;
      }

      credentials.username.assign(reinterpret_cast<const char *>(payload_ptr),
                                  num_bytes);
      has_username = 1;
      break;
    case TAG_PASSWORD:
      if (num_bytes > MAX_PASSWORD_SIZE) {
        LOG_ERROR("Password of length %d surpasses maximum password size %d!\n",
                  num_bytes, MAX_PASSWORD_SIZE);
        return RESP_ERROR_MALFORMED;
      }
      credentials.password.assign(reinterpret_cast<const char *>(payload_ptr),
                                  num_bytes);
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

int build_login_response(message_t &response, user_t &user) {
  response.version = 1;
  response.type = LOGIN;
  response.rc = RESP_OK;
  uint8_t *moving_ptr = response.payload;
  write_tlv(moving_ptr, TAG_USER_ID, sizeof(user.user_id),
            static_cast<void *>(&user.user_id), 1);
  write_tlv(moving_ptr, TAG_USERNAME,
            static_cast<uint8_t>(user.username.length()), user.username.data(),
            0);
  response.payload_length = (uint32_t)(moving_ptr - response.payload);
  if (response.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Payload of size %d bytes exceeds maximum of %d!\n",
              response.payload_length, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

int parse_login_response(message_t &response, user_t &user) {
  uint8_t *payload_ptr = response.payload;
  uint8_t *payload_end = response.payload + response.payload_length;
  int has_id = 0;
  int has_username = 0;
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
    case TAG_USERNAME:
      if (num_bytes > MAX_USERNAME_SIZE) {
        LOG_ERROR("Username of length %d surpasses maximum username size %d!\n",
                  num_bytes, MAX_USERNAME_SIZE);
        return RESP_ERROR_MALFORMED;
      }
      user.username.assign(reinterpret_cast<const char *>(payload_ptr),
                           num_bytes);
      has_username = 1;
      break;
    case TAG_USER_ID:
      memcpy(&user.user_id, payload_ptr, num_bytes);
      user.user_id = ntohl(user.user_id);
      has_id = 1;
      break;
    default:
      LOG_WARN("Unknown TLV tag '%d', skipping %d bytes!\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }
  if (!has_id || !has_username) {
    LOG_ERROR("Missing user id or username tag!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

// ---------------------------------
// World Broadcasting
// ---------------------------------

int build_world_broadcast(uint8_t *request_buffer, world_broadcast_t &broadcast,
                          uint32_t &message_len) {
  uint8_t *header_ptr{request_buffer};
  uint8_t *payload_ptr{request_buffer + MSG_HEADER_SIZE};
  if (broadcast.sender_username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("build_world_broadcast Failure: Sender username length is '%d', "
              "higher than maximum!\n",
              broadcast.sender_username.length());
    return -1;
  }
  if (broadcast.message_content.length() > MAX_MSG_CONTENT_SIZE) {
    LOG_ERROR("build_world_broadcast Failure: Message content length is '%d', "
              "higher than maximum!\n",
              broadcast.message_content.length());
    return -1;
  }

  // Write payload into buffer; calculate payload length
  write_tlv(payload_ptr, TAG_WORLD_BROADCAST, 0, NULL, 0);
  write_tlv(payload_ptr, TAG_SENDER_USERNAME,
            static_cast<uint8_t>(broadcast.sender_username.length()),
            broadcast.sender_username.data(), 0);
  write_tlv(payload_ptr, TAG_MESSAGE_CONTENT,
            static_cast<uint8_t>(broadcast.message_content.length()),
            broadcast.message_content.data(), 0);

  // end of payload - start of message = total message size.
  message_len = static_cast<uint32_t>(payload_ptr - request_buffer);
  uint32_t payload_len = message_len - MSG_HEADER_SIZE;
  uint32_t net_payload_len = htonl(payload_len);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("World Message Failed: Payload of size %d bytes exceeds maximum "
              "of %d!\n",
              payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }

  // Write main header fields.
  *header_ptr++ = 1;
  *header_ptr++ = CHAT;
  *header_ptr++ = 0;
  memcpy(header_ptr, &net_payload_len, sizeof(net_payload_len));
  return 0;
}

int build_world_broadcast(message_t &request, world_broadcast_t &broadcast) {
  // Write world broadcast's data into the message payload
  uint8_t *moving_ptr = request.payload;
  write_tlv(moving_ptr, TAG_WORLD_BROADCAST, 0, NULL, 0);

  write_tlv(moving_ptr, TAG_SENDER_USERNAME,
            static_cast<uint8_t>(broadcast.sender_username.length()),
            broadcast.sender_username.data(), 0);
  write_tlv(moving_ptr, TAG_MESSAGE_CONTENT,
            static_cast<uint8_t>(broadcast.message_content.length()),
            broadcast.message_content.data(), 0);

  // Calculate payload and write header fields into message
  uint32_t payload_len = (uint32_t)(moving_ptr - request.payload);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("World Message Failed: Payload of size %d bytes exceeds maximum "
              "of %d!\n",
              payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request.version = 1;
  request.type = CHAT;
  request.rc = 0;
  request.payload_length = payload_len;
  return 0;
}

int parse_world_broadcast(message_t &request, world_broadcast_t &broadcast) {
  uint8_t *payload_ptr = request.payload;
  uint8_t *payload_end = payload_ptr + request.payload_length;
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
      broadcast.sender_username.assign(
          reinterpret_cast<const char *>(payload_ptr), num_bytes);
      has_sender = 1;
      break;
    case TAG_MESSAGE_CONTENT:

      // If the client follows uses our code to build a world broadcast,
      // then the content of the message won't exceed the maximum else.
      // Otherwise the client could manually engineer a world broadcast would a
      // content size that exceeds the limit, but that broadcast wouldn't be
      // parsed correctly
      broadcast.message_content.assign(
          reinterpret_cast<const char *>(payload_ptr), num_bytes);
      has_content = 1;
      break;
    default:
      LOG_WARN("Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }
  if (!has_broadcast_tag || !has_sender || !has_content) {
    LOG_ERROR("Malformed request: broadcast, sender, or content tag missing "
              "from request payload!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

int build_world_broadcast_notification(message_t &response,
                                       world_broadcast_t &broadcast) {
  return build_world_broadcast(response, broadcast);
}

int parse_world_broadcast_notification(message_t &msg,
                                       world_broadcast_t &broadcast) {
  return parse_world_broadcast(msg, broadcast);
}

// ---------------------------------
// P2P Broadcasting
// ---------------------------------

int build_p2p_broadcast(uint8_t *request_buffer, p2p_broadcast_t &broadcast,
                        uint32_t &message_len) {
  uint8_t *header_ptr{request_buffer};
  uint8_t *payload_ptr{request_buffer + MSG_HEADER_SIZE};

  // Input validation checks
  if (broadcast.sender_username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("build_p2p_broadcast Failure: Sender username length is '%d', "
              "higher than maximum!\n",
              broadcast.sender_username.length());
    return -1;
  }
  if (broadcast.recipient_username.length() > MAX_USERNAME_SIZE) {
    LOG_ERROR("build_p2p_broadcast Failure: Recipient username length is '%d', "
              "higher than maximum!\n",
              broadcast.sender_username.length());
    return -1;
  }
  if (broadcast.sender_username == broadcast.recipient_username) {
    LOG_ERROR("build_p2p_broadcast Failure: Sender and Recipient username "
              "matched!\n");
    return -1;
  }
  if (broadcast.message_content.length() > MAX_MSG_CONTENT_SIZE) {
    LOG_ERROR("build_p2p_broadcast Failure: Message content length is '%d', "
              "higher than maximum!\n",
              broadcast.message_content.length());
    return -1;
  }

  // Write payload data into the buffer
  write_tlv(payload_ptr, TAG_P2P_BROADCAST, 0, NULL, 0);
  write_tlv(payload_ptr, TAG_SENDER_USERNAME,
            static_cast<uint8_t>(broadcast.sender_username.length()),
            broadcast.sender_username.data(), 0);
  write_tlv(payload_ptr, TAG_RECIPIENT_USERNAME,
            static_cast<uint8_t>(broadcast.recipient_username.length()),
            broadcast.recipient_username.data(), 0);
  write_tlv(payload_ptr, TAG_MESSAGE_CONTENT,
            static_cast<uint8_t>(broadcast.message_content.length()),
            broadcast.message_content.data(), 0);

  // Calculate payload and write header fields into message
  message_len = static_cast<uint32_t>(payload_ptr - request_buffer);
  uint32_t payload_len = message_len - MSG_HEADER_SIZE;
  uint32_t net_payload_len = htonl(payload_len);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR(
        "P2P Message Failed: Payload of size %d bytes exceeds maximum of %d!\n",
        payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }

  *header_ptr++ = 1;
  *header_ptr++ = CHAT;
  *header_ptr++ = 0;
  memcpy(header_ptr, &net_payload_len, sizeof(uint32_t));
  return 0;
}

int build_p2p_broadcast(message_t &request, p2p_broadcast_t &broadcast) {

  // Validate usernames, write TLVs into payload buffer
  uint8_t *moving_ptr = request.payload;
  if (broadcast.sender_username == broadcast.recipient_username) {
    LOG_ERROR("P2P Message Failed: Sender and Recipient username matched!\n");
    return -1;
  }
  write_tlv(moving_ptr, TAG_P2P_BROADCAST, 0, NULL, 0);
  write_tlv(moving_ptr, TAG_SENDER_USERNAME,
            static_cast<uint8_t>(broadcast.sender_username.length()),
            broadcast.sender_username.data(), 0);
  write_tlv(moving_ptr, TAG_RECIPIENT_USERNAME,
            static_cast<uint8_t>(broadcast.recipient_username.length()),
            broadcast.recipient_username.data(), 0);
  write_tlv(moving_ptr, TAG_MESSAGE_CONTENT,
            static_cast<uint8_t>(broadcast.message_content.length()),
            broadcast.message_content.data(), 0);

  // Calculate payload and write header fields into message
  uint32_t payload_len = (uint32_t)(moving_ptr - request.payload);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR(
        "P2P Message Failed: Payload of size %d bytes exceeds maximum of %d!\n",
        payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  request.version = 1;
  request.type = CHAT;
  request.rc = 0;
  request.payload_length = payload_len;

  return 0;
}

int parse_p2p_broadcast(message_t &request, p2p_broadcast_t &broadcast) {
  uint8_t *payload_ptr = request.payload;
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
      broadcast.sender_username.assign(
          reinterpret_cast<const char *>(payload_ptr), num_bytes);
      has_sender = 1;
      break;
    case TAG_RECIPIENT_USERNAME:
      if (num_bytes > MAX_USERNAME_SIZE) {
        LOG_ERROR("Malformed TLV: recipient username length exceeds limits!\n");
        return RESP_ERROR_MALFORMED;
      }
      broadcast.recipient_username.assign(
          reinterpret_cast<const char *>(payload_ptr), num_bytes);
      has_recipient = 1;
      break;
    case TAG_MESSAGE_CONTENT:
      broadcast.message_content.assign(
          reinterpret_cast<const char *>(payload_ptr), num_bytes);
      has_content = 1;
      break;
    default:
      LOG_WARN("Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
    }
    payload_ptr += num_bytes;
  }
  if (!has_broadcast_tag || !has_sender || !has_recipient || !has_content) {
    LOG_ERROR("Malformed request: broadcast, sender, recipient or content tag "
              "missing from request payload!\n");
    return RESP_ERROR_MALFORMED;
  }
  return 0;
}

int build_p2p_broadcast_notification(message_t &response,
                                     p2p_broadcast_t &broadcast) {
  return build_p2p_broadcast(response, broadcast);
}

int parse_p2p_broadcast_notification(message_t &msg,
                                     p2p_broadcast_t &broadcast) {
  return parse_p2p_broadcast(msg, broadcast);
}
