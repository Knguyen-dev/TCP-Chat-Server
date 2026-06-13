#include "client_utils.hpp"

// -----------------------------
// Signal Handling and Graceful Shutdown
// -----------------------------

// Shared shutdown pipe between main and socket thread
// NOTE: Used for when the client closes the connection.
// Where g_shutdown_pipe[0] is the read fd (for peer) and g_shutdown_pipe[1] is 
// the write fd (for main).
static int g_shutdown_pipe[2] = {-1, -1};

// Atomic flag that controls the command loop.
std::atomic<bool> g_keep_running{true};

// Mutex that protects the connection struct
std::mutex g_conn_mutex;

// Represent the client's connection with the server.
// The mutex that ensures only one thread accesses the connection.
// NOTE: The connection object itself is accessed by the main and socket thread.
conn_t* g_client_conn;



/**
 * Signal handler for SIGINT (Ctrl+C).
 * 
 * IMPORTANT: Signal handlers must be "signal-safe" - they can only safely call
 * a limited set of functions (write, close, exit, etc.). Avoid C++ constructs,
 * function calls to non-async-safe functions, or anything that allocates memory.
 */
static void handle_client_sigint(int sig) { 
  const char msg[] = "\n[SIGINT] Shutting down...\n";
  ssize_t unused = write(STDERR_FILENO, msg, sizeof(msg) - 1);
  (void)unused;
  g_keep_running.store(false);
}

int setup_client_signal_handlers(conn_t* conn) {
  // Client connection pointer
  if (!conn) {
    LOG_ERROR("setup_signal_handlers called with null connection");
    return -1;
  }

  g_client_conn = conn;

  // Install signal handler
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_client_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, nullptr) == -1) {
    LOG_ERROR("Failed to install SIGINT handler: %s\n", strerror(errno));
    return -1;
  }
  LOG_DEBUG("Signal handlers installed\n");
  return 0;
}

// ---------------------------
// Client message request functions. 
// All of these will handle sending the message request to the server
// ---------------------------

/**
 * Posts a formatted message to the UI thread.
 * @param sender The username of the sender
 * @param recipient The username of the recipient (NULL for global messages)
 * @param message The content of the message being sent
 */
static void print_chat_message(std::string_view sender, std::string_view recipient, std::string_view message) {
  if (!recipient.empty()) {
    printf("[DM] %s to %s: %s\n", sender.data(), recipient.data(), message.data());
  } else {
    printf("[GLOBAL] %s: %s\n", sender.data(), message.data());
  }
}

/**
 * Handles prompting user input and sending a request for a world message to the server.
 * 
 * @param conn Connection representing the client's TCP connection with the server.
 * @return 0 on success, otherwise -1 on error.
 */
static int handle_client_registration(conn_t& conn, std::string& username, std::string& password) {
  registration_credentials_t credentials;
  credentials.username = username;
  credentials.password = password;
  uint32_t message_len{};
  uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
  if (build_register_request(request_buffer, credentials, message_len) == -1) {
    LOG_ERROR("[System] Failed to build registration request!\n");
    return -1;
  }  
  if (write_one_message(conn.fd, request_buffer, message_len) == -1) {
    LOG_ERROR("[System] Failed to write registraton request!\n");
    return -1;
  }
  return 0;
}

/**
 * Handles prompting user input, and attempting a login for the user.
 * @param conn Connection representing client's TCP connection with the server.
 * @return 0 on successful login, otherwise -1.
 */
static int handle_client_login(conn_t& conn, std::string& username, std::string& password) {
  login_credentials_t credentials;
  credentials.username = username;
  credentials.password = password;
  uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
  uint32_t message_len{};
  if (build_login_request(request_buffer, credentials, message_len) == -1) {
    LOG_ERROR("[Login] Failed to build login request!\n");
    return -1;
  }
  if (write_one_message(conn.fd, request_buffer, message_len) == -1) {
    LOG_ERROR("[Login] Failed to write login request!\n");
    return -1;
  }
  return 0;
}

/**
 * Handles broadcasting a message to all other users connected to the TCP server.
 * @param conn Connection representing client's TCP connection with the server.
 * @param message_content The text or message content that we're sending to all other users.
 * @note The user input for this request is gathered in the caller, as a stylistic choice. For example 
 * the caller can just type /world <message> to send their message, sending their command and its args in one 
 * line.
 */
static void handle_world_message(conn_t& conn, std::string& message_content) {
  world_broadcast_t broadcast;
  broadcast.sender_username = conn.user.username;
  broadcast.message_content = message_content;
  uint8_t request_buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
  uint32_t message_len{};
  if (build_world_broadcast(request_buffer, broadcast, message_len) == -1) {
    LOG_ERROR("[System] Client Failure: Failed to build world broadcast!\n");
    return;
  }
  if (write_one_message(conn.fd, request_buffer, message_len) == -1) {
    LOG_ERROR("[System] Client Failure: Failed to write world broadcast!\n");
    return;
  }
}

/**
 * Handles sending a p2p message request to the server
 * @param conn Connection representing client's TCP connection with the server.
 * @param recipient_username The username of the recipient user that the message is being sent to.
 * @param message_content The text or message content that we're sending to the recipient user.
 * 
 * @note User input for this is gathered in the caller. This is merely a stylistic choice.
 */
static void handle_peer_message(conn_t& conn, std::string& recipient_username, std::string& message_content) {
  p2p_broadcast_t broadcast;
  broadcast.sender_username = conn.user.username;  
  broadcast.recipient_username = recipient_username;
  broadcast.message_content = message_content;
  uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
  uint32_t message_len{};
  if (build_p2p_broadcast(request_buffer, broadcast, message_len) == -1) {
    LOG_ERROR("[System] Client Failure: Failed to build P2P broadcast!\n");
    return;
  }
  if (write_one_message(conn.fd, request_buffer, message_len) == -1) {
    LOG_ERROR("[System] Client Failure: Failed to write P2P broadcast!\n");
    return;
  }
}

// -------------------------------
// Client message response functions; are called in a separate thread.
// These handle processing responses from the server e.g. response to registration, login, etc.
// -------------------------------

/**
 * Handles the server's response to a client's user registration.
 * @param conn Connection representing client's TCP connection with the server.
 * @param response A parsed message object whose message_t::type is assumed to 
 * be 'REGISTER'. It contains info about the server's response to the client's 
 * registration request.
 */
static void handle_registration_response(message_t& response) {
  if (response.rc != 0) {
    LOG_WARN(
      "[Registration]: User Registraton Failed: %s\n",
      get_response_message(static_cast<response_code_t>(response.rc)).data()
    );
    return;
  }

  user_t user{};
  int ret = parse_register_response(response, user);
  if (ret == -1) {
    LOG_ERROR("parse_register_response() failed!\n");
    return;
  }
  LOG_INFO(
    "[Registration]: Successful, <User id=%d, username=%s>\n", 
    user.user_id, user.username.data()
  );
}

/**
 * Handles the server's response to a client's user login request.
 * @param conn Connection representing the client's TCP connection with the server.
 * @param response A response message whose message_t::type is assumed to be 
 * 'LOGIN'. It contains inof about the server's response to the client's login request.
 */
static void handle_login_response(conn_t& conn, message_t& response) {
  if (response.rc != 0) {
    LOG_WARN(
      "[LOGIN]: User Login Failed: %s\n",
      get_response_message(static_cast<response_code_t>(response.rc)).data()
    );
    return;
  }
  user_t user{};
  int ret = parse_login_response(response, user);
  if (ret == -1) {
    LOG_ERROR("parse_login_response() failed!\n");
    return;
  }
  LOG_INFO("[Login]: Successful, <User id=%d, username=%s>\n", user.user_id, user.username.data());
  
  std::lock_guard<std::mutex> lock(g_conn_mutex);
  g_client_conn->user.user_id = user.user_id;
  g_client_conn->user.username = user.username;
  g_client_conn->flags |= ConnFlags::IS_AUTH;
}

/**
 * Handles processing the server's response message to the client broadcast request message.
 * @param conn Connection representing the client's TCP connection with the server.
 * @param response A response message whose message_t::type is CHAT. It contains info about the 
 * server's response to the client's broadcast request.
 */
static void handle_broadcast_response(conn_t& conn, message_t& response) {
  if (response.rc != 0) {
    LOG_WARN("[CHAT] Message failed to send: %s\n", get_response_message(static_cast<response_code_t>(response.rc)).data());
    return;
  }
  int broadcast_type = peek_broadcast_type(response);
  switch (broadcast_type) {
    case TAG_P2P_BROADCAST: {
      p2p_broadcast_t broadcast{};
      parse_p2p_broadcast_notification(response, broadcast);
      print_chat_message(broadcast.sender_username, broadcast.recipient_username, broadcast.message_content);
      break;
    }
    case TAG_WORLD_BROADCAST: {
      world_broadcast_t broadcast{};
      parse_world_broadcast_notification(response, broadcast);
      print_chat_message(broadcast.sender_username, "", broadcast.message_content);
      break;
    }
    default: {
      LOG_WARN("Unknown broadcast_type '%d'!\n", broadcast_type);
    }
  }
}

/**
 * Handles processing a response from the server.
 * @param conn Connection representing client's TCP connection with the server.
 * @return true on successful reading/processing, false when the servver stops responding 
 */
static bool handle_server_response(conn_t& conn) {
  // General Workflow
  // 1. Read one and parse one full message into response
  // 2. Based on human-readable response message handle the response message with one of our handlers
  message_t response{};
  uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
  response.payload = payload_buffer;
  int rc = read_one_message(conn.fd, response);
  if (rc == -1) {
    return false;
  }
  switch (response.type) {
    case REGISTER:
      handle_registration_response(response);
      break;
    case LOGIN:
      handle_login_response(conn, response);
      break;
    case CHAT:
      handle_broadcast_response(conn, response);
      break;
    default:
      LOG_WARN("Server response type unknown: '%d'\n", response.type);
  }
  return true;
}

/**
 * Handler function for the background thread that listens for asynchronous broadcasts from the server.
 * @param conn Connection representing the client's TCP connection with the server.
 */
static void socket_thread_handler(conn_t& conn) {
  // ##### Two ways this thread can finish #####
  // 1. Server closes connection: Sends a FIN packet, which should trigger the POLL readiness API, then 
  // the handle_broadcast_notification will return false. Then the user quits on their own.
  // 2. User types '/quit', where main thread signals peer to break out of loop and terminate.
  struct pollfd pfds[2]{};
  while (true) {
    memset(pfds, 0, sizeof(pfds));
    pfds[0].fd = conn.fd;
    pfds[0].events = POLLIN | POLLERR;
    pfds[1].fd = g_shutdown_pipe[0];
    pfds[1].events = POLLIN;

    int ret = poll(pfds, 2, -1);
    if (ret == -1) {
      LOG_ERROR("Poll Error: %s\n", strerror(errno));
      break;
    }

    // Main thread signalled shutdown, break out of loop.
    if (pfds[1].revents & POLLIN) {
      break;
    }

    // Handle a broadcast notification.
    // If we failed to handle it e.g., server closed connection, 
    // then we'll break out of the loop.
    if (pfds[0].revents & (POLLIN | POLLERR)) {
      if (!handle_server_response(conn)) {
        break;
      }
    }
  }
}


// ----------------------
// Main command loop
// ----------------------

/**
 * Runs the shell for a given client connection.
 * @param conn Connection representing the client's TCP connection with the server.
 * @param input_line String representing the inputted line.
 * @return true on success, false when the user wants to exit the message loop!
 */
static bool command_shell(conn_t& conn, std::string& input_line) {
  std::stringstream ss{input_line};
  std::string command;
  if (!(ss >> command)) return true;  // Extract first word and lowercase it; this is the command
  command = string_to_lower(command);

  if (command == "/p2p") {
    if (!has_flag(conn.flags, ConnFlags::IS_AUTH)) {
      LOG_INFO("[System] You must log in before messaging!\n");
      return true;
    }
    std::string recipient;
    std::string message;
    ss >> recipient;
    std::getline(ss >> std::ws, message);
    if (recipient.empty() || message.empty()) {
      LOG_INFO("[System] Usage '/p2p <recipient_username> <Your Message>'\n");
      return true;
    }
    handle_peer_message(conn, recipient, message);    
  } else if (command == "/world") {
    if (!has_flag(conn.flags, ConnFlags::IS_AUTH)) {
      LOG_INFO("[System] You must log in before messaging!\n");
      return true;
    }
    std::string message;
    std::getline(ss >> std::ws, message);
    if (message.empty()) {
      LOG_INFO("[System] Usage '/world <your message>'\n");
      return true;
    }

    handle_world_message(conn, message);
  } else if (command == "/login") {
    if (has_flag(conn.flags, ConnFlags::IS_AUTH)) {
      LOG_INFO("[System] Can't login since already logged in as '%s'!\n", conn.user.username.data());
      return true;
    }

    std::string username;
    std::string password; 
    ss >> username;
    ss >> password;
    if (username.empty() || password.empty()) {
      LOG_INFO("[Login] Usage '/login <username> <password>'\n");
      return true;
    }

    handle_client_login(conn, username, password);
  } else if (command == "/register") {
    if (has_flag(conn.flags, ConnFlags::IS_AUTH)) {
      LOG_INFO("[System] Can't register since already logged in as '%s'!\n", conn.user.username.data());
      return true;
    }

    std::string username;
    std::string password;
    ss >> username;
    ss >> password;
    if (username.empty() || password.empty()) {
      LOG_INFO("[Register] Usage '/register <username> <password>'\n");
      return true;
    }
    handle_client_registration(conn, username, password);
  } else if (command == "/quit") {
    printf("[System] User quit! Thanks for using TCP-Chat!\n");
    return false;
  } else {
    LOG_WARN("Unknown command: %s\n", command.data());
  }
  return true;
}

/**
 * Runs an event loop for the client
 * @param conn Connection representing the client's TCP connection with the server.
 */
void run_messaging_loop(conn_t& conn) {

  // NOTE: Create a pipe for IPC between the main and peer thread. 
  // This allows the main thread to write a signal to the peer thread 
  // for manually breaking out the loop.
  if (pipe(g_shutdown_pipe) == -1) {
    LOG_ERROR("Failed to create shutdown pipe: %s\n", strerror(errno));
    return;
  }

  // Run main thread and peer thread
  std::thread socket_thread(socket_thread_handler, std::ref(conn)); 
  while (g_keep_running) {
    std::string buffer{};
    if (!std::getline(std::cin, buffer)) {
      LOG_ERROR("std::getline got an EOF or error\n");
      return;
    }
    if (!command_shell(conn, buffer)) {
      break;
    }    
  }

  // Main thread tells peer thread to shutdown
  // NOTE: Done first so that any incoming messages are stored/processed.
  // before we just completely close the connection in the next section.
  uint8_t signal = 1;
  ssize_t ret = write(g_shutdown_pipe[1], &signal, 1);
  if (ret == -1) {
    LOG_ERROR("write() failure: %s!\n", strerror(errno));
    return;
  }
  socket_thread.join();
  close(g_shutdown_pipe[0]);
  close(g_shutdown_pipe[1]);

  // lock the connection mutex
  // 1. Free the connection
  // 2. Free the heap allocated user pointer if it exists 
  std::lock_guard<std::mutex> lock(g_conn_mutex);
  if (g_client_conn->fd != -1) {
    close(g_client_conn->fd);  
  }
  
  LOG_INFO("See you again someday, somewhere!\n");
}

// -------------------------------
// Network Connection and Setup
// -------------------------------

int create_client_connection(char* ip, short port, conn_t& conn) {  
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("socket() error: %s!\n", strerror(errno));
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
    LOG_ERROR("inet_ptons() failed: Invalid IP address '%s'!\n", ip);
    return -1;
  }

  // Attempt to connect to server IP:port
  int conn_result = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result == -1) {
    close(fd);
    LOG_ERROR("connect() failed: '%s'!\n", strerror(errno));
    return -1;
  }

  conn.fd = fd;
  return fd;
}
