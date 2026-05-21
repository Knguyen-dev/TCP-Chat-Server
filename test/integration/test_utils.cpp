#include "test_utils.hpp"

const char* TEST_SERVER_IP = "127.0.0.1";
const uint16_t TEST_SERVER_PORT = 8080;

int connect_to_server(const char* ip, uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    close(sock);
    std::cerr << "Invalid server IP\n";
    return -1;
  }

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("connect");
    close(sock);
    return -1;
  }

  return sock;
}

std::string random_string(std::size_t length) {
  static const char alphanum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  static std::mt19937_64 rng(static_cast<unsigned long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
  static std::uniform_int_distribution<size_t> dist(0, sizeof(alphanum) - 2);

  std::string result;
  result.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    result.push_back(alphanum[dist(rng)]);
  }
  return result;
}

// ##### Registration Test Utils #####

int send_register_request(int fd, const std::string& username, const std::string& password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  registration_credentials_t creds{username, password};
  uint32_t message_len = 0;
  if (build_register_request(buffer, creds, message_len) != 0) {
    return -1;
  }
  return write_one_message(fd, buffer, message_len);
}

int send_malformed_register(int fd, bool include_username, bool include_password, bool oversize_username, bool oversize_password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint8_t* ptr = buffer;
  *ptr++ = 1;
  *ptr++ = REGISTER;
  *ptr++ = 0;
  uint8_t* payload_length_ptr = ptr;
  ptr += 4;

  if (include_username) {
    std::string username = oversize_username ? std::string(MAX_USERNAME_SIZE + 1, 'a') : "user";
    write_tlv(ptr, TAG_USERNAME, static_cast<uint8_t>(username.size()), username.data(), 0);
  }
  if (include_password) {
    std::string password = oversize_password ? std::string(MAX_PASSWORD_SIZE + 1, 'b') : "password";
    write_tlv(ptr, TAG_PASSWORD, static_cast<uint8_t>(password.size()), password.data(), 0);
  }

  uint32_t payload_length = static_cast<uint32_t>(ptr - (buffer + MSG_HEADER_SIZE));
  uint32_t net_payload_length = htonl(payload_length);
  memcpy(payload_length_ptr, &net_payload_length, sizeof(net_payload_length));

  return write_one_message(fd, buffer, MSG_HEADER_SIZE + payload_length);
}

// ##### Login Test Utils #####

int send_login_request(int fd, const std::string& username, const std::string& password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  login_credentials_t creds{username, password};
  uint32_t message_len = 0;
  if (build_login_request(buffer, creds, message_len) != 0) {
    return -1;
  }
  return write_one_message(fd, buffer, message_len);
}

int send_malformed_login(int fd, bool include_username, bool include_password, bool oversize_username, bool oversize_password) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint8_t* ptr = buffer;
  *ptr++ = 1;
  *ptr++ = LOGIN;
  *ptr++ = 0;
  uint8_t* payload_length_ptr = ptr;
  ptr += 4;

  if (include_username) {
    std::string username = oversize_username ? std::string(MAX_USERNAME_SIZE + 1, 'a') : "user";
    write_tlv(ptr, TAG_USERNAME, static_cast<uint8_t>(username.size()), username.data(), 0);
  }
  if (include_password) {
    std::string password = oversize_password ? std::string(MAX_PASSWORD_SIZE + 1, 'b') : "password";
    write_tlv(ptr, TAG_PASSWORD, static_cast<uint8_t>(password.size()), password.data(), 0);
  }

  uint32_t payload_length = static_cast<uint32_t>(ptr - (buffer + MSG_HEADER_SIZE));
  uint32_t net_payload_length = htonl(payload_length);
  memcpy(payload_length_ptr, &net_payload_length, sizeof(net_payload_length));

  return write_one_message(fd, buffer, MSG_HEADER_SIZE + payload_length);
}

// ##### World Broadcast Test Utils #####

int build_world_broadcast_unsafe(uint8_t* request_buffer, world_broadcast_t& broadcast, uint32_t& message_len) {
  uint8_t* header_ptr{request_buffer};
  uint8_t* payload_ptr{request_buffer+MSG_HEADER_SIZE};

  // Write payload into buffer; calculate payload length 
  write_tlv(payload_ptr, TAG_WORLD_BROADCAST, 0, NULL, 0); 
  write_tlv(payload_ptr, TAG_SENDER_USERNAME, broadcast.sender_username.length(), broadcast.sender_username.data(), 0);
  write_tlv(payload_ptr, TAG_MESSAGE_CONTENT, broadcast.message_content.length(), broadcast.message_content.data(), 0);
  
  // end of payload - start of message = total message size.
  message_len = payload_ptr - request_buffer;
  uint32_t payload_len = message_len - MSG_HEADER_SIZE;
  uint32_t net_payload_len = htonl(payload_len);

  // Write main header fields.
  *header_ptr++ = 1;
  *header_ptr++ = CHAT;
  *header_ptr++ = 0;
  memcpy(header_ptr, &net_payload_len, sizeof(net_payload_len));
  return 0;
}

int send_world_broadcast_request(int fd, const std::string& sender_username, const std::string& message_content) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  world_broadcast_t broadcast{sender_username, message_content};
  uint32_t message_len = 0;
  if (build_world_broadcast_unsafe(buffer, broadcast, message_len) != 0) {
    return -1;
  }
  return write_one_message(fd, buffer, message_len);
}

int register_and_login(user_t& out_user) {
  // Helper function that registers and logins a user, returning the socket fd for the logged in connection.
  std::string username = "user_" + random_string(8);
  std::string password = "pass_" + random_string(8);

  int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
  if (sock == -1) {
    return -1;
  }

  if (send_register_request(sock, username, password) == -1) {
    close(sock);
    return -1;
  }

  message_t response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  response.payload = payload_buffer;
  if (read_server_response(sock, response) != 0 || response.rc != RESP_OK) {
    close(sock);
    return -1;
  }

  if (send_login_request(sock, username, password) == -1) {
    close(sock);
    return -1;
  }

  if (read_server_response(sock, response) != 0 || response.rc != RESP_OK) {
    close(sock);
    return -1;
  }

  if (response.type != LOGIN || response.rc != RESP_OK) {
    close(sock);
    return -1;
  }

  parse_login_response(response, out_user);
  return sock;
}

int send_malformed_world_broadcast(int fd, bool include_sender_username, bool include_message_content, bool oversize_message_content) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint8_t* ptr = buffer;
  *ptr++ = 1;
  *ptr++ = CHAT;
  *ptr++ = 0;
  uint8_t* payload_length_ptr = ptr;
  ptr += 4;

  if (include_sender_username) {
    std::string sender_username = "sender";
    write_tlv(ptr, TAG_SENDER_USERNAME, static_cast<uint8_t>(sender_username.size()), sender_username.data(), 0);
  }
  if (include_message_content) {
    std::string message_content = oversize_message_content ? std::string(MAX_MSG_CONTENT_SIZE + 1, 'a') : "Hello, world!";
    write_tlv(ptr, TAG_MESSAGE_CONTENT, static_cast<uint8_t>(message_content.size()), message_content.data(), 0);
  }

  uint32_t payload_length = static_cast<uint32_t>(ptr - (buffer + MSG_HEADER_SIZE));
  uint32_t net_payload_length = htonl(payload_length);
  memcpy(payload_length_ptr, &net_payload_length, sizeof(net_payload_length));
  return write_one_message(fd, buffer, MSG_HEADER_SIZE + payload_length);
}

int read_server_response(int fd, message_t& response) {
  return read_one_message(fd, response);
}

// ----- P2P helpers -----
int build_p2p_broadcast_unsafe(uint8_t* request_buffer, p2p_broadcast_t& broadcast, uint32_t& message_len) {
  uint8_t* header_ptr{request_buffer};
  uint8_t* payload_ptr{request_buffer + MSG_HEADER_SIZE};

  // Write payload data into the buffer
  write_tlv(payload_ptr, TAG_P2P_BROADCAST, 0, NULL, 0); 
  write_tlv(payload_ptr, TAG_SENDER_USERNAME, broadcast.sender_username.length(), broadcast.sender_username.data(), 0);
  write_tlv(payload_ptr, TAG_RECIPIENT_USERNAME, broadcast.recipient_username.length(), broadcast.recipient_username.data(), 0);
  write_tlv(payload_ptr, TAG_MESSAGE_CONTENT, broadcast.message_content.length(), broadcast.message_content.data(), 0);
  
  // Calculate payload and write header fields into message
  message_len = payload_ptr - request_buffer;
  uint32_t payload_len = message_len - MSG_HEADER_SIZE;
  uint32_t net_payload_len = htonl(payload_len);
 
  *header_ptr++ = 1;
  *header_ptr++ = CHAT;
  *header_ptr++ = 0;
  memcpy(header_ptr, &net_payload_len, sizeof(uint32_t));
  return 0;
}

int send_p2p_broadcast_request(int fd, const std::string& sender_username, const std::string& recipient_username, const std::string& message_content) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  p2p_broadcast_t broadcast{sender_username, recipient_username, message_content};
  uint32_t message_len = 0;
  if (build_p2p_broadcast_unsafe(buffer, broadcast, message_len) != 0) {
    return -1;
  }
  return write_one_message(fd, buffer, message_len);
}

int send_malformed_p2p_broadcast(int fd, bool include_sender, bool include_recipient, bool include_content, bool oversize_content) {
  uint8_t buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint8_t* ptr = buffer;
  *ptr++ = 1;
  *ptr++ = CHAT;
  *ptr++ = 0;
  uint8_t* payload_length_ptr = ptr;
  ptr += 4;

  if (include_sender) {
    std::string sender = "sender";
    write_tlv(ptr, TAG_SENDER_USERNAME, static_cast<uint8_t>(sender.size()), sender.data(), 0);
  }
  if (include_recipient) {
    std::string recipient = "recipient";
    write_tlv(ptr, TAG_RECIPIENT_USERNAME, static_cast<uint8_t>(recipient.size()), recipient.data(), 0);
  }
  if (include_content) {
    std::string content = oversize_content ? std::string(MAX_MSG_CONTENT_SIZE + 1, 'x') : "hello";
    write_tlv(ptr, TAG_MESSAGE_CONTENT, static_cast<uint8_t>(content.size()), content.data(), 0);
  }

  uint32_t payload_length = static_cast<uint32_t>(ptr - (buffer + MSG_HEADER_SIZE));
  uint32_t net_payload_length = htonl(payload_length);
  memcpy(payload_length_ptr, &net_payload_length, sizeof(net_payload_length));
  return write_one_message(fd, buffer, MSG_HEADER_SIZE + payload_length);
}