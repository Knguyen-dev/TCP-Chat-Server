#include "client_utils.h"

int create_registration_message(char *username, char *password, message_t *request) {
  request->version = 1;
  request->type = REGISTER;
  request->flags = 0;
  uint8_t *payload_start = (uint8_t *)request->payload;
  uint8_t **cursor = &payload_start;
  write_tlv(cursor, TAG_USERNAME, strlen(username), username, 0);
  write_tlv(cursor, TAG_PASSWORD, strlen(password), password, 0);
  uint32_t payload_len = (uint32_t)(*cursor-payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    printf("Registration Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

int handle_client_registration(int clientfd) {
  char username[MAX_USERNAME_SIZE+1];
  char password[MAX_PASSWORD_SIZE+1];

  printf("Enter a username (max %d characters): ", MAX_USERNAME_SIZE);
  if (fgets(username, sizeof(username), stdin)) {
    username[strcspn(username, "\n")] = '\0'; // strip newline character from fgets string
  } else {
    fprintf(stderr, "fgets() Failed: Error reading username!\n");
    return -1;
  }

  printf("Enter a password (max %d characters): ", MAX_PASSWORD_SIZE);
  if (fgets(password, sizeof(password), stdin)) {
    password[strcspn(password, "\n")] = '\0';
  } else {
    fprintf(stderr, "fgets() Failed: Error reading password!\n");
    return -1;
  }

  // 1. Create request message
  message_t request_message = {0};
  int result = create_registration_message(username, password, &request_message);
  if (result == -1) {
    return -1;
  }

  // 2. Send it over the wire 
  result = write_one_message(clientfd, &request_message);
  if (result == -1) {
    return -1;
  }

  // 3. Read the response from the server
  message_t response_message = {0};
  result = read_one_message(clientfd, &response_message);
  if (result == -1) {
    return -1;
  }

  // 4. Parse payload
  // a. Response code tlv; should have a 1-byte value.
  // b. Response message tlv
  response_code_t rc = 0;
  char message[48];
  uint32_t bytes_to_read = response_message.payload_length;
  uint8_t* payload_ptr = response_message.payload;

  while (bytes_to_read > 0) {
    // Parse 1 byte Tag, 1 byte Length, and variable-length Value
    tlv_tag_t tag = *payload_ptr;
    payload_ptr++;
    uint8_t num_bytes = *payload_ptr;
    payload_ptr++;
    switch (tag) {
      case TAG_RESPONSE_CODE:
        memcpy(&rc, payload_ptr, num_bytes);
        break;
      case TAG_RESPONSE_MESSAGE:
        memcpy(message, payload_ptr, num_bytes);
        message[num_bytes] = '\0';
        break;
      default:
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, length);
    }
    payload_ptr += num_bytes;
  }

  printf("Server Says (code=%d): %s", rc, message);
  return 0;
}

int create_login_message(char *username, char* password, message_t *request) {
  request->version = 1;
  request->type = LOGIN;
  request->flags = 0;
  uint8_t *payload_start = (uint8_t *)request->payload;
  uint8_t **cursor = &payload_start;
  write_tlv(cursor, TAG_USERNAME, strlen(username), username, 0);
  write_tlv(cursor, TAG_PASSWORD, strlen(password), password, 0);
  uint32_t payload_len = (uint32_t)(*cursor-payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    printf("Login Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

int handle_client_login(int clientfd, user_t *user) {
  char input_username[MAX_USERNAME_SIZE+1];
  char input_password[MAX_PASSWORD_SIZE+1];

  printf("Enter your username: ");
  scanf("%s", &input_username);
  printf("Enter your password: ");
  scanf("%d", &input_password);

  // 1. Create request message
  message_t request_message = {0};
  int result = create_login_message(input_username, input_password, &request_message);
  if (result == -1) {
    return -1;
  }

  // 2. Send it over the wire 
  result = write_one_message(clientfd, &request_message);
  if (result == -1) {
    return -1;
  }

  // 3. Read a response from the server
  message_t response_message = {0};
  result = read_one_message(clientfd, &response_message);
  if (result == -1) {
    return -1;
  }

  // 4. Parse payload
  // a. Response code tlv; should have a 1-byte value.
  // b. Response message tlv
  response_code_t rc = 0;
  char message[48];
  uint32_t bytes_to_read = response_message.payload_length;
  uint8_t* payload_ptr = response_message.payload;
  while (bytes_to_read > 0) {
    // Parse 1 byte Tag, 1 byte Length, and variable-length Value
    tlv_tag_t tag = *payload_ptr;
    payload_ptr++;
    uint8_t num_bytes = *payload_ptr;
    payload_ptr++;
    switch (tag) {
      case TAG_RESPONSE_CODE:
        memcpy(&rc, payload_ptr, num_bytes);
        break;
      case TAG_RESPONSE_MESSAGE:
        memcpy(message, payload_ptr, num_bytes);
        message[num_bytes] = '\0';
        break;
      case TAG_USER_ID:
        memcpy(&user->id, payload_ptr, num_bytes);
        user->id = ntohl(user->id);
        break;
      case TAG_USERNAME:
        memcpy(user->username, payload_ptr, num_bytes);
        user->username[num_bytes] = '\0';
        break;
      case TAG_PASSWORD:
        memcpy(user->password, payload_ptr, num_bytes);
        user->password[num_bytes] = '\0';
        break;
      default:
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, length);
    }
    payload_ptr += num_bytes;
  }

  printf("Server Says (code=%d): %s", rc, message);
  return 0;
}

int create_world_message(char* message_content, message_t *request) {
  request->version = 1;
  request->type = CHAT;
  request->flags = 0;
  uint8_t *payload_start = (uint8_t *)request->payload;
  uint8_t **cursor = &payload_start;

  // NOTE: TLV with TAG_WORLD won't have a value, so we'll say the length is 0 bytes.
  write_tlv(cursor, TAG_WORLD, 0, 0, 0); 
  write_tlv(cursor, TAG_MESSAGE_CONTENT, strlen(message_content), message_content, 0);
  uint32_t payload_len = (uint32_t)(*cursor-payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    printf("World Message Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

int handle_world_message(int clientfd, user_t *user) {

  char message_content[MAX_MSG_CONTENT_SIZE+1];
  printf("Enter your message (max '%d' characters): ", MAX_MSG_CONTENT_SIZE);
  if (fgets(message_content, sizeof(message_content), stdin)) {
    message_content[strcspn(message_content, "\n")] = '\0';
  } else {
    fprintf(stderr, "fgets() Failed: Error reading password!\n");
    return -1;
  }

  message_t request_message = {0};
  int result = create_world_message(message_content, &request_message);
  if (result == -1) {
    return -1;
  }
  result = write_one_message(clientfd, &request_message);
  if (result == -1) {
    fprintf(stderr, "write_one_message() failed: %s\n", strerror(errno));
    return -1;
  }

  // TODO: Almost done

}





int create_peer_message(char* recipient_username, char* message_content, message_t *request) {
  request->version = 1;
  request->type = CHAT;
  request->flags = 0;
  uint8_t *payload_start = (uint8_t *)request->payload;
  uint8_t **cursor = &payload_start;
  write_tlv(cursor, TAG_USERNAME, strlen(recipient_username), recipient_username, 0);
  write_tlv(cursor, TAG_MESSAGE_CONTENT, strlen(message_content), message_content, 0);
  uint32_t payload_len = (uint32_t)(*cursor-payload_start);
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    printf("Peer Message Failed: Payload of size %d bytes exceeds maximum of %d!\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

int create_client_connection(char* ip, short port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fprintf(stderr, "socket() error: %s!\n", strerror(errno));
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  int result = inet_ptons(AF_INET, ip, &server_addr.sin_addr);
  if (result <= 0) {
    close(fd);
    fprintf(stderr, "inet_ptons() failed: Invalid IP address '%s'!\n", ip);
    return -1;
  }

  int conn_result = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result == -1) {
    close(fd);
    fprintf(stderr, "connect() failed: '%s'!\n", strerror(errno));
  }
  
  return fd;
}