#include "client_utils.h"

// ---------------------------
// Client Service API e.g. login, registration, messaging.
// ---------------------------

/**
 * Prints a formatted message to stdout based on message type.
 * Centralizes all message output formatting.
 * 
 * @param sender_username The username of the sender
 * @param recipient_username The username of the recipient (NULL for global messages)
 * @param message_content The content of the message
 */
void print_message(const char* sender_username, const char* recipient_username, const char* message_content) {
  if (recipient_username != NULL) {
    printf("[DM] %s -> %s> %s\n", sender_username, recipient_username, message_content);
  } else {
    printf("[GLOBAL] %s> %s\n", sender_username, message_content);
  }
}

/**
 * Handles prompting user input and sending data over the wire.
 * 
 * @param clientfd The client fd associated with our TCP connection
 * @return 0 on success, otherwise -1 on error.
 */
int handle_client_registration(int clientfd) {
  registration_credentials_t credentials = {0};

  printf("Enter a username: ");
  if (fgets(credentials.username, sizeof(credentials.username), stdin) == NULL) {
    fprintf(stderr, "Error reading username\n");
    return -1;
  }
  credentials.username[strcspn(credentials.username, "\n")] = '\0';  // Remove newline

  printf("Enter a password: ");
  if (fgets(credentials.password, sizeof(credentials.password), stdin) == NULL) {
    fprintf(stderr, "Error reading password\n");
    return -1;
  }
  credentials.password[strcspn(credentials.password, "\n")] = '\0';  // Remove newline

  // 1. Create request message
  message_t request_message = {0};
  if (build_register_request(&request_message, &credentials) == -1) {
    return -1;
  }

  // 2. Send it over the wire 
  if (write_one_message(clientfd, &request_message) == -1) {
    return -1;
  }

  // 3. Read the response from the server
  message_t response = {0};
  if (read_one_message(clientfd, &response) == -1) {
    return -1;
  }

  printf("Server Response (code=%d): %s\n", response.rc, response_messages[response.rc]);
  return 0;
}

/**
 * Handles prompting user input, and attempting a login for the user.
 * @param clientfd Client fd that's used in the socket connection.
 * @param user Pointer to a user struct that we'll populate with the info of the authenticated user.
 * @return 0 on successful login, otherwise -1.
 */
int handle_client_login(int clientfd, user_t *user) {
  login_credentials_t credentials = {0};

  printf("Username: ");
  if (fgets(credentials.username, sizeof(credentials.username), stdin) == NULL) {
    fprintf(stderr, "Error reading username\n");
    return -1;
  }
  credentials.username[strcspn(credentials.username, "\n")] = '\0';

  printf("Password: ");
  if (fgets(credentials.password, sizeof(credentials.password), stdin) == NULL) {
    fprintf(stderr, "Error reading password\n");
    return -1;
  }
  credentials.password[strcspn(credentials.password, "\n")] = '\0';
  
  // 1. Create request message and send it across the wire
  message_t request = {0};
  if (build_login_request(&request, &credentials) != 0) {
    return -1;
  }
  if (write_one_message(clientfd, &request) == -1) {
    return -1;
  } 

  // 2. Read and parse server response
  message_t response = {0};
  if (read_one_message(clientfd, &response) == -1) {
    return -1;
  }
  if (response.rc != RESP_OK) {
    printf("Login Failed (code=%d): %s\n", response.rc, response_messages[response.rc]);
    return -1;
  }

  strcpy(user->username, credentials.username);
  printf("Login successful: Now joined as '%s'!\n", credentials.username);
  return 0;
}

/**
 * Handles broadcasting a message to all other users connected to the TCP server.
 * @param clientfd File descriptor linked to the TCP connection socket the client has with the server.
 * @param user User that the client is logged in as.
 * @param message_content The text or message content that we're sending to all other users.
 */
void handle_world_message(int clientfd, user_t *user, char* message_content) {
  // 1. Build world broadcast request message
  world_broadcast_t broadcast = {0};
  strcpy(broadcast.sender_username, user->username);
  strcpy(broadcast.message_content, message_content);
  message_t request = {0};
  if (build_world_broadcast(&request, &broadcast) != 0) {
    printf("Client Failure: Failed to build world broadcast!\n");
    return;
  }

  // 2. Send world broadcast message over TCP
  if (write_one_message(clientfd, &request) == -1) {
    printf("Client Failure: Failed to write world broadcast!\n");
    return;
  }

  // 3. Read response message from the server
  // a. If bad response code, output failure message
  // b. Otherwise, output message indicating user's message was broadcasted
  message_t response = {0};
  if (read_one_message(clientfd, &response) != 0) {
    printf("Client Failure: Failed to read server response message.\n");
    return;
  }
  if (response.rc != 0) {
    printf("Server Failure: Your message wasn't broadcasted.\n");
    return;
  }

  print_message(broadcast.sender_username, NULL, broadcast.message_content);
}

/**
 * Handles sending a message to a single remote peer through the TCP server.
 * @param clientfd File descriptor linked to the TCP connection socket the client has with the server.
 * @param user User that the client is logged in as.
 * @param recipient_username The username of the recipient user that the message is being sent to.
 * @param message_content The text or message content that we're sending to the recipient user.
 */
void handle_peer_message(int clientfd, user_t* user, char* recipient_username, char* message_content) {
  // 1. Create world broadcast request and send it over the wire
  p2p_broadcast_t broadcast = {0};
  strcpy(broadcast.sender_username, user->username);
  strcpy(broadcast.recipient_username, recipient_username);
  strcpy(broadcast.message_content, message_content);
  message_t request = {0};
  if (build_p2p_broadcast(&request, &broadcast) != 0) {
    printf("Client Failure: Failed to build P2P broadcast!\n");
    return;
  }
  if (write_one_message(clientfd, &request) == -1) {
    printf("Client Failure: Failed to write P2P broadcast!\n");
    return;
  }
  
  // 2. Read response from the server
  message_t response = {0};
  if (read_one_message(clientfd, &response) != 0) {
    printf("Client Failure: Failed to read server response!\n");
    return;
  }

  // 3. Output results to the client
  if (response.rc == 0) {
    print_message(broadcast.sender_username, broadcast.recipient_username, broadcast.message_content);
  } else {
    printf("Server Failure (code=%d): P2P message was sent, but not broadcasted! Extra details: %s\n", response.rc, response_messages[response.rc]);
  }
}

/**
 * Handles receiving broadcast notification messages from the server. 
 * 
 * @param clientfd File descriptor linked to the TCP connection socket the client has with the server.
 * @return 0 on success, otherwise -1 when the server closes connection.
 */
int handle_broadcast_notification(int clientfd) {
  message_t response = {0};

  // If failed, then return -1 to back out
  if (read_one_message(clientfd, &response) == -1) {
    printf("Server closed connection! Redirecting to main menu!\n");
    return -1;
  }

  if (response.type != CHAT) {
    // NOTE: This shouldn't really happen.
    printf("Server response wasn't a chat message!\n");
    return -1;
  }
  int broadcast_type = peek_broadcast_type(&response);
  
  switch (broadcast_type) {
    case TAG_P2P_BROADCAST: {
      p2p_broadcast_t broadcast = {0};
      parse_p2p_broadcast_notification(&response, &broadcast);
      print_message(broadcast.sender_username, broadcast.recipient_username, broadcast.message_content);
      break;
    }
    case TAG_WORLD_BROADCAST: {
      world_broadcast_t broadcast = {0};
      parse_world_broadcast_notification(&response, &broadcast);
      print_message(broadcast.sender_username, NULL, broadcast.message_content);
      break;
    }
    default: {
      // NOTE: Shouldn't really happen.
      printf("Unknown broadcast_type '%d'!\n", broadcast_type);
    }
  }

  return 0;
}

// ---------------------------
// Client Loop and user input
// ---------------------------

void shell(int clientfd, user_t* user) {
  char prompt[100] = {0};
  char command_buffer[10 + MAX_USERNAME_SIZE + MAX_MSG_CONTENT_SIZE + 1]; // Enough for "/<command>, where command=world or p2p" and additional args
  get_stdin(prompt, command_buffer, sizeof(command_buffer));

  char* command = strtok(command_buffer, " ");
  if (command == NULL) {
    return;
  }

  if (strcmp(command, "/p2p") == 0) {
    char* recipient_username = strtok(NULL, " ");
    char* message = strtok(NULL, ""); // get the remaining string
    if (!recipient_username || !message) {
      printf("Error: Usage '/p2p <recipient_username> <Your Message>'\n");
      return;
    }
    handle_peer_message(clientfd, user, recipient_username, message);    
  } else if (strcmp(command, "/world") == 0) {
    char* message = strtok(NULL, "");
    if (!message) {
      printf("Error: Usage '/world <your message>\n");
      return;
    }
    handle_world_message(clientfd, user, message);
  } else {
    printf("Unknown Command: %s\n", command);
  }  
}

/**
 * Runs an event loop for the client
 * 
 * @param clientfd fd for the TCP connection socket.
 */
void run_messaging_loop(int clientfd, user_t* user) {
  struct pollfd fds[2];
  
  while (1) {
    // Step 1: Prepare poll args for stdin and tcp connection (read and any errors)
    // a. Clear existing structs (since they've been modified by prev loop)
    // b. Create new structs that are listening for read and error events
    memset(fds, 0, sizeof(fds));
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN | POLLERR;
    fds[1].fd = clientfd;
    fds[1].events = POLLIN | POLLERR;
    nfds_t nfds = sizeof(fds) / sizeof(fds[0]);
    int ret = poll(fds, nfds, -1);
    if (ret == -1) {
      fprintf(stderr, "Poll Error (Shutting down app): %s\n", strerror(errno));
      exit(0);
    }

    // Check if the server sent something
    if (fds[1].revents & POLLIN) {
      int rc = handle_broadcast_notification(clientfd);
      if (rc == -1) {
        break;
      }
    }

    // Check if the user is typing something 
    // NOTE: I'm assuming this would check when the input stream has data?
    // I'm assuming this doesn't just trigger as I'm typing in something.
    if (fds[0].revents & POLLIN) {
      shell(clientfd, user);
    }
  }
}


// -------------------------------
// Network Connection and Setup
// -------------------------------
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

  // Verify input IP address and insert it into server_addr
  int result = inet_pton(AF_INET, ip, &server_addr.sin_addr);
  if (result <= 0) {
    close(fd);
    fprintf(stderr, "inet_ptons() failed: Invalid IP address '%s'!\n", ip);
    return -1;
  }

  // Attempt to connect to server IP 
  int conn_result = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result == -1) {
    close(fd);
    fprintf(stderr, "connect() failed: '%s'!\n", strerror(errno));
    return -1;
  }
  
  return fd;
}
