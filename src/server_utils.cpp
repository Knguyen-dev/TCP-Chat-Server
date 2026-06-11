#include "shared.hpp"
#include "server_utils.hpp"
#include "protocol.hpp"
#include "logger.hpp"
#include "db.hpp"
#include <chrono>

// ----------------
// Connection Table
// ----------------

/**
 * Creates and allocates memory for N connections
 * @param N Amount of connection we'll initially allocate memory for.
 * @note This should be called when the server starts as we want to take advantage of arena allocation.
 */
static ConnectionManager create_connection_manager(int64_t N) {
  ConnectionManager manager;

  // Allocates memory for N connections
  manager.flags.reserve(N);
  manager.incoming_buffers.reserve(N);
  manager.outgoing_buffers.reserve(N);
  manager.user_ids.reserve(N);
  manager.usernames.reserve(N);

  // Creates those N connection instances within those empty slots.
  // Sets reasonable default values
  for (int64_t i = 0; i < N; i++) {
    manager.flags.push_back(ConnFlags::NONE);
    manager.incoming_buffers.emplace_back();
    manager.outgoing_buffers.emplace_back();
    manager.user_ids.push_back(0);      
    manager.usernames.push_back("user");
  }   
  return manager;
}

ConnectionManager conn_manager{create_connection_manager(INITIAL_CAPACITY)};

// -----------------------------------
// Metrics
// -----------------------------------

MemoryMetrics get_memory_usage() {
    std::ifstream statm("/proc/self/statm");
    long vm_pages{0};
    long rss_pages{0};
    if (statm >> vm_pages >> rss_pages) {
        long page_size = sysconf(_SC_PAGESIZE); // Typically 4096 bytes
        return { vm_pages * page_size, rss_pages * page_size };
    }
    return {0, 0};
}

/**
 * Calculates server metrics and logs them to the console
 */
static void log_server_metrics() {

  // 1. Get number of active and authenticated connections
  int num_connected{0};
  int num_auth{0};
  for (const ConnFlags& conn: conn_manager.flags) {
    if (!has_flag(conn, ConnFlags::IS_ACTIVE)) {
      continue;
    }
    num_connected++;
    if (!has_flag(conn, ConnFlags::IS_AUTH)) {
      continue;
    }
    num_auth++;
  }

  // 2. Get virtual and physical memory usage
  MemoryMetrics mem{get_memory_usage()};

  // 3. Get heap usage data.
  struct mallinfo2 mi = mallinfo2();
  char buf[512];
  int offset = 0;
  offset += snprintf(buf + offset, sizeof(buf) - offset,
      "Num connections = %d, num auth = %d\n",
      num_connected, num_auth);
  offset += snprintf(buf + offset, sizeof(buf) - offset,
      "Physical RAM Usage (bytes) = %zu\n", 
      mem.physical_ram_bytes);
  offset += snprintf(buf + offset, sizeof(buf) - offset,
      "Heap Used = %ld bytes, heap free = %ld bytes, total heap = %ld\n",
      mi.uordblks, mi.fordblks, mi.arena);

  LOG_INFO("%s\n", buf);
}

int handle_server_input() {
  std::string command_buffer;
  char buf[128];
  bool input_complete = false;
  while (true) {
    ssize_t bytes_read = read(STDIN_FILENO, buf, sizeof(buf)-1);
    if (bytes_read <= 0) {
      if (bytes_read == 0) {
        LOG_INFO("Stdin closed!\n");
        break; 
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // We have read all currently available data from the buffer
        break;
      } else if (errno == EINTR) {
        // Interrupted by a signal, try reading again
        continue;
      } else {
        LOG_ERROR("Read error on stdin\n");
        break;
      } 
    }
    buf[bytes_read] = '\0';
    command_buffer += buf;
    // Check if the user hit 'Enter' (newline indicates the end of the command)
    if (command_buffer.back() == '\n') {
        input_complete = true;
    }
  }

  if (!input_complete) {
    LOG_WARN("Input not complete: Aborting command pipeline!\n");
    return -1;
  }

  if (!command_buffer.empty() && command_buffer.back() == '\n') {
    command_buffer.pop_back();
  }
  
  if (command_buffer == "/metrics") {
    // Call your metrics function here!
    log_server_metrics(); 
    return 0;
  } else {
    LOG_INFO("Unknown server command: %s\n", command_buffer.c_str());
    return -1;
  }
}

// -----------------------------------
// Fd and Utils
// -----------------------------------

/**
 * Makes a socket associated with a file descriptor nonblocking
 * @param fd The file descriptor associated with the socket.
 */
void set_nonblocking_fd(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    LOG_ERROR("fcntl error: %s\n", strerror(errno));
    exit(-1);
  }

  int rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (rc == -1) {
    LOG_ERROR("fcntl error: %s\n", strerror(errno));
    exit(-1);
  }
}

/**
 * Updates the state of an existing connection.
 * @param epollfd File descriptor for the epoll instance that our application is using.
 * @param connfd Fd for the TCP connection
 * @param next_state The state we're updating the connection to reflect.
 */
static void set_connection_state(int epollfd, int connfd, ConnState next_state) {
  ConnFlags& current_flags = conn_manager.flags[connfd];
  struct epoll_event ev{};
  ev.data.fd = connfd;
  switch(next_state) {
    case ConnState::READING:
      current_flags |= ConnFlags::WANT_READ;
      current_flags &= ~ConnFlags::WANT_WRITE;
      ev.events = EPOLLIN | EPOLLET;
      if (epoll_ctl(epollfd, EPOLL_CTL_MOD, connfd, &ev) == -1) {
        LOG_ERROR("epoll_ctl failure: %s\n", strerror(errno));
      }
      break;
    case ConnState::WRITING:
      current_flags &= ~ConnFlags::WANT_READ;
      current_flags |= ConnFlags::WANT_WRITE;
      ev.events = EPOLLOUT | EPOLLET;
      if (epoll_ctl(epollfd, EPOLL_CTL_MOD, connfd, &ev) == -1) {
        LOG_ERROR("epoll_ctl failure: %s\n", strerror(errno));
      } 
      break;
    case ConnState::CLOSING:
      current_flags |= ConnFlags::WANT_CLOSE;

      // NOTE: We intentionally don't call epoll_ctl here. The event loop
      // will very quickly call remove_connection to cleanup the connection
      // in our epoll instance and do other stuff.
      break;
  }
}

// -----------------------------------
// Connection Table API (Dynamic Array)
// -----------------------------------
/**
 * Adds an unauthenticated client connection to the connection table and epoll instance
 * @param epollfd Fd for the epoll instance
 * @param connfd Fd for the TCP connection socket
 */
static void add_connection(int epollfd, int connfd) {

  // 1. Create nonblocking socket and fd to interest list; reading and edge-triggered
  set_nonblocking_fd(connfd);
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLET; 
  ev.data.fd = connfd;
  int rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &ev);
  if (rc == -1) {
    LOG_ERROR("epoll_ctl error: %s\n", strerror(errno));
    return;
  }

  // 2. Reallocate dynamic vector if needed
  if (conn_manager.get_size() <= (size_t)connfd) {
    conn_manager.flags.resize(connfd+1);
    conn_manager.incoming_buffers.resize(connfd+1);
    conn_manager.outgoing_buffers.resize(connfd+1);
    conn_manager.user_ids.resize(connfd+1);
    conn_manager.usernames.resize(connfd+1);
  }
  
  // 3. Record flags; connection slot is active and wants to read.
  conn_manager.flags[connfd] = ConnFlags::IS_ACTIVE | ConnFlags::WANT_READ;
}

void remove_connection(int connfd, int epollfd) {
  // NOTE: Mark connection as inactive (and therefore unauthenticated) 
  // Setting it to none, is probably the easiest thing as 
  // we don't need connection specific information anymore.
  ConnFlags& current_flags = conn_manager.flags[connfd];
  current_flags = ConnFlags::NONE;

  // Remove TCP connection fd from epoll interest list
  epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, nullptr);
}

// -----------------------------------
// Application-Level Services e.g. Register, Login
// -----------------------------------

/**
 * Registers a user into the application
 * 
 * @param request Request message struct that has the data needed to register a user.
 * @param response Empty response message that this function will modify/populate with data based
 * based on the status of the current request.
 * @return 0 if success, otherwise an error response code.
 */
static int register_user(message_t& request, message_t& response) {
  registration_credentials_t credentials = {};
  int rc = parse_register_request(request, credentials);
  if (rc != 0) {
    LOG_ERROR("parse_register_request Failure!\n");
    response = build_server_response(REGISTER, static_cast<response_code_t>(rc), NULL, 0);
    return rc;
  }

  // If registered username isn't unique, reject request
  // NOTE: This approach preserves the casing of the usernames, but enforces 
  // case-insensitive uniqueness. So 'Alice' and 'alice' are considered the same username, and 
  // if 'Alice' is registered first, then 'alice' will be rejected as a registration username.
  user_t user{};
  rc = get_user_by_username(string_to_lower(credentials.username), user); 
  if (rc == 1) {
    LOG_DEBUG("Username '%s' already taken!\n", credentials.username.data());
    response = build_server_response(REGISTER, RESP_ERROR_USER_EXISTS, NULL, 0);
    return RESP_ERROR_USER_EXISTS;
  } else if (rc == -1) {
    LOG_ERROR("get_user_by_username Failed!\n");
    response = build_server_response(REGISTER, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  // Insert new user in the database
  user.user_id = 0; 
  user.username = credentials.username;
  user.password = credentials.password;
  if (insert_user(user) != 0) {
    LOG_ERROR("insert_user Failure!\n");
    response = build_server_response(REGISTER, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  if (build_register_response(response, user) != 0) {
    LOG_ERROR("build_register_response() failed!\n");
    response = build_server_response(REGISTER, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  return RESP_OK;
}

/**
 * Authenticates a user given their credentials in the request message
 * 
 * @param connfd Fd for the TCP connection
 * @param request Populated request message that represents the login request.
 * @param response Empty response message that the callee (this function) will modify with information.
 * @return 0 on success, otherwise a response code.
 */
static int login_user(int connfd, message_t& request, message_t& response) {
  login_credentials_t credentials{};
  int rc = parse_login_request(request, credentials);
  if (rc != 0) {
    response = build_server_response(LOGIN, static_cast<response_code_t>(rc), NULL, 0);
    return rc;
  }
  
  // Does username exist in db? 
  user_t user{};
  rc = get_user_by_username(string_to_lower(credentials.username), user);
  if (rc != 1) {
    if (rc == 0) {
      LOG_DEBUG("User with username '%s' not found\n", credentials.username.data());
      response = build_server_response(LOGIN, RESP_ERROR_USER_NOT_FOUND, NULL, 0);
      return RESP_ERROR_USER_NOT_FOUND;
    } else {
      LOG_ERROR("get_user_by_username() Failed!\n");
      response = build_server_response(LOGIN, RESP_ERROR_INTERNAL, NULL, 0);
      return RESP_ERROR_INTERNAL;
    }
  }

  if (user.password != credentials.password) {
    LOG_DEBUG("Invalid password for user '%s'\n", credentials.username.data());
    response = build_server_response(LOGIN, RESP_ERROR_INVALID_CREDENTIALS, NULL, 0);
    return RESP_ERROR_INVALID_CREDENTIALS;
  }

  // Update connection table with user info and authenticate the connection.
  conn_manager.user_ids[connfd] = user.user_id;
  conn_manager.usernames[connfd] = user.username;
  conn_manager.flags[connfd] |= ConnFlags::IS_AUTH;

  if (build_login_response(response, user) != 0) {
    LOG_ERROR("build_login_response() failed!\n");
    response = build_server_response(LOGIN, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  return RESP_OK;
}

/**
 * Handles sending a message to remote peers.
 * @param connfd Fd for the TCP connection sending this broadcast.
 * @param epollfd Fd associated with the epoll instance we're using.
 * @param request Request message containing the broadcast request.
 * @param response Response message that the callee will populate with data about the result of this operation.
 * @return 0 on success, non-zero return code otherwise.
 */
static int handle_broadcast_message(int connfd, int epollfd, message_t& request, message_t& response) {
  
  // If connection isn't authenticated, reject the request
  // NOTE: Only authenticated users are allowed to send broadcasts.
  if (!has_flag(conn_manager.flags[connfd], ConnFlags::IS_AUTH)) {
    LOG_DEBUG("Unauthenticated user attempted to send a broadcast message!\n");
    response = build_server_response(CHAT, RESP_ERROR_INVALID_CREDENTIALS, NULL, 0);
    return RESP_ERROR_INVALID_CREDENTIALS;
  }
  
  tlv_tag_t broadcast_tag = peek_broadcast_type(request);
  switch (broadcast_tag) {
    case TAG_WORLD_BROADCAST: {      
      // 1. Parse the user's broadcast request to get the broadcast information.
      world_broadcast_t broadcast{};
      int ret = parse_world_broadcast(request, broadcast);
      if (ret != 0) {
        LOG_ERROR("parse_world_broadcast Failure!\n");
        response = build_server_response(CHAT, static_cast<response_code_t>(ret), NULL, 0);
        return ret;
      }

      // 2. If message content is empty, reject the request
      if (broadcast.message_content.empty()) {
        LOG_DEBUG("Received world broadcast with empty message content!\n");
        response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
        return RESP_ERROR_MALFORMED;
      }

      // If the sender username in the broadcast doesn't match the actual TCP client
      // TODO: 
      // In reality, the server should autofill the username server side rather than relying on user input.
      // That's why I did this. You should modify the protocol.cpp and corresponding client code so that we 
      // don't send over sender_username in a message.
      broadcast.sender_username = conn_manager.usernames[connfd];

      // Use broadcast to create a broadcast response
      // NOTE: Broadcast response sent to all other clients will also be the response for the sender.
      ret = build_world_broadcast_notification(response, broadcast);
      if (ret != 0) {
        LOG_ERROR("build_world_broadcast_notification Failure!\n");
        response = build_server_response(CHAT, static_cast<response_code_t>(ret), NULL, 0);
        return ret;
      }

      // Write serialized response data into recipient outgoing buffers.
      // NOTE: Skip the sender, because after this function returns, we'll add this response to the 
      // sender's outgoing buffer. If you don't skip the sender you're going to send the world-message twice.
      std::vector<uint8_t> serialized_response;
      write_message_to_buffer(serialized_response, response);
      for (size_t i{0}; i < conn_manager.get_size(); i++) {
        // a. Recipient is only valid if it's authenticated AND it isn't the sender
        bool is_valid_recipient = has_flag(conn_manager.flags[i], ConnFlags::IS_AUTH) & (i != static_cast<size_t>(connfd));
        if (!is_valid_recipient) {
          continue;
        }

        // b. Copy serialized message into recipient's outgoing buffer
        // c. Set the recipient to writing state so the event loop writes the data we stored in their outgoing buffer.
        buf_append(conn_manager.outgoing_buffers[i], serialized_response.data(), serialized_response.size());
        set_connection_state(epollfd, i, ConnState::WRITING);
      }
      break;
    }
    case TAG_P2P_BROADCAST: {
      // 1. Parse the sender's broadcast request into something readable.
      p2p_broadcast_t broadcast{};
      int ret = parse_p2p_broadcast(request, broadcast);
      if (ret != 0) {
        response = build_server_response(CHAT, static_cast<response_code_t>(ret), NULL, 0);
        return ret;
      }

      if (broadcast.message_content.empty()) {
        LOG_DEBUG("Received p2p broadcast with empty message content!\n");
        response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
        return RESP_ERROR_MALFORMED;
      }

      broadcast.sender_username = conn_manager.usernames[connfd];
      if (broadcast.sender_username == broadcast.recipient_username) {
        LOG_DEBUG("Received p2p broadcast with recipient username the same as the sender username!\n");
        response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
        return RESP_ERROR_MALFORMED;
      }

      // 2. Create the broadcast response message that we'll send to the peer.
      // NOTE: The response message sent to the peer will also act as the response message to the 
      // client's P2P broadcast request.
      ret = build_p2p_broadcast_notification(response, broadcast);
      if (ret != 0) {
        LOG_ERROR("build_p2p_broadcast_notification Failure!\n");
        response = build_server_response(CHAT, static_cast <response_code_t>(ret), NULL, 0);
        return ret;
      }

      // Search for the recipient connection in the table
      int recipient_conn_fd{-1};
      const std::string& recipient_username = broadcast.recipient_username;
      for (size_t i{0}; i < conn_manager.get_size(); i++) {
        // a. Recipient is only valid if it's authenticated AND it isn't the sender
        bool is_valid_recipient = has_flag(conn_manager.flags[i], ConnFlags::IS_AUTH) & (i != static_cast<size_t>(connfd));
        if (!is_valid_recipient) {
          continue;
        }
        // b. If connection's username matches the recipient username, record and break out of loop
        if (conn_manager.usernames[i] == recipient_username) {
          recipient_conn_fd = i;
          break;
        }
      }
      if (recipient_conn_fd == -1) {
        response = build_server_response(CHAT, RESP_ERROR_USER_NOT_FOUND, NULL, 0);
        return RESP_ERROR_USER_NOT_FOUND;
      }
      
      // a. Copy message into recipient's outgoing buffer
      // b. Set recipient to writing state so the event loop knows to send to the recipient. 
      // TODO: Seems like an unnecessary copy.
      std::vector<uint8_t> serialized_response;
      write_message_to_buffer(serialized_response, response);
      buf_append(conn_manager.outgoing_buffers[recipient_conn_fd], serialized_response.data(), serialized_response.size());
      set_connection_state(epollfd, recipient_conn_fd, ConnState::WRITING);
      break;
    }
    default:
      LOG_WARN("Received unknown broadcast tag '%d'. Rejecting request!\n", broadcast_tag);
      response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
      return RESP_ERROR_MALFORMED;
  }

  return RESP_OK;
}

// --------------------------------------
// Event Loop Utils and API
// --------------------------------------

/**
 * Attempts to serve one request message for the given connection
 * @param connfd Fd of the TCP connection
 * @param epollfd Epollfd that's needed when we're handling a broadcast request.
 * @return true when we're able to serve one request, otherwise false.
 */
static bool try_one_request(int connfd, int epollfd) {

  // Don't proceed if we don't have message header + 1 byte of payload yet.
  std::vector<uint8_t>& incoming_buffer = conn_manager.incoming_buffers[connfd];
  if (incoming_buffer.size() < MSG_HEADER_SIZE + 1) {
    return false;
  }

  message_t request;
  parse_message(incoming_buffer, request);
  if (request.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Message payload size was too big. Closing connection!\n");
    set_connection_state(epollfd, connfd, ConnState::CLOSING);
    return false;
  }
  
  // Don't proceed if we don't have at least a full message worth of data.
  if (MSG_HEADER_SIZE + request.payload_length > incoming_buffer.size()) {
    return false;
  }

  // Setup storage for the response message
  // NOTE: At this point you know that message_t::payload points somewhere in conn_t::incoming
  // due to parse_message handling the heavy lifting.
  uint8_t response_payload[MSG_MAX_PAYLOAD_SIZE];
  message_t response{};
  response.payload = response_payload;
  response.payload_length = 0;
  switch (request.type) {
    case REGISTER:
      register_user(request, response);
      break;
    case LOGIN:
      login_user(connfd, request, response);
      break;
    case CHAT:
      handle_broadcast_message(connfd, epollfd, request, response);
      break;
    default:
      LOG_WARN("Received unknown message type '%d'!\n", request.type);
      response = build_server_response(request.type, RESP_ERROR_UNKNOWN_COMMAND, NULL, 0);
      break;
  }

  // a. Successfully processed message, erase message data from incoming buffer.
  // b. Write server response to client's outgoing buffer.
  buf_consume(incoming_buffer, MSG_HEADER_SIZE+request.payload_length);
  write_message_to_buffer(conn_manager.outgoing_buffers[connfd], response);
  return true; 
}

void handle_read_connection(int connfd, int epollfd) {
  uint8_t buffer[64 * 1024];
  while (true) {
    ssize_t rv = read(connfd, buffer, sizeof(buffer));
    if (rv < 0) {
      // Buffer is fully drained; we can safely stop and wait for next ET notification
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      // Otherwise, we had an actual read() error, close connection
      LOG_ERROR("read() error: %s\n", strerror(errno));
      set_connection_state(epollfd, connfd, ConnState::CLOSING);
      return;
    } else if (rv == 0) {
      if (conn_manager.incoming_buffers[connfd].size() == 0) {
        if (has_flag(conn_manager.flags[connfd], ConnFlags::IS_AUTH)) {
          LOG_DEBUG("Remote Peer (fd=%d, username=%s) disconnected!\n", connfd, conn_manager.usernames[connfd].data());
        } else {
          LOG_DEBUG("Remote Peer (fd=%d) disconnected!\n", connfd);
        }
      } else {
        LOG_DEBUG("Remote Peer (fd=%d) unexpected EOF!\n", connfd);
      }
      set_connection_state(epollfd, connfd, ConnState::CLOSING);
      return;
    }
    buf_append(conn_manager.incoming_buffers[connfd], buffer, rv);
  }

  // ### ASIDE: Core State Machine Logic ###
  // 1. Append bytes to the end of conn_t::incoming in handle_read_connection.
  // 2. Erase bytes from the start of conn_t::incoming in try_one_request.
  // 3. Append bytes to the end of conn_t::outgoing  in try_one_request
  // 4. Erase bytes from the start of conn_t::outgoing in handle_write_connection
  // NOTE: while loop means if we read multiple requests, then we'll try to process multiple (request pipelining).
  while (try_one_request(connfd, epollfd)) {};

  // If we have outgoing messages to send (due to successfully parsing a request message), 
  // then update the state machine to indicate that we want to write to the socket.
  std::vector<uint8_t>& outgoing_buffer = conn_manager.outgoing_buffers[connfd];
  if (outgoing_buffer.size() > 0) {
    set_connection_state(epollfd, connfd, ConnState::WRITING);
  }
}

void handle_write_connection(int connfd, int epollfd) {

  std::vector<uint8_t>& outgoing_buffer = conn_manager.outgoing_buffers[connfd];
  while (true) {
    ssize_t rv = write(connfd, outgoing_buffer.data(), outgoing_buffer.size());
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return; 
      }
      // Otherwise an actual error happened.
      LOG_ERROR("write() error. Closing connection '%d'\n", connfd);
      set_connection_state(epollfd, connfd, ConnState::CLOSING);
      return;
    }
    buf_consume(outgoing_buffer, (size_t)rv);

    // If no messages left to write, 
    // a. Set connection to read mode
    // b. Exit out of this function since we're done reading
    if (outgoing_buffer.size() == 0) {
      set_connection_state(epollfd, connfd, ConnState::READING);
      return;
    }
  }
}

void accept_all_connections(int listenfd, int epollfd) {
  socklen_t client_len;
  struct sockaddr_storage client_address;
  while (true) {
    client_len = sizeof(struct sockaddr_storage);
    int conn_fd = accept(listenfd, (struct sockaddr *)&client_address, &client_len);
    if (conn_fd < 0) {

      // If blocking, then no connections are left in the kernel, exit
      // NOTE: Accepting one connection at a time, and there could be multiple requests
      // which is why we're in a while loop
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      
      // Otherwise, we had a real accept() error
      LOG_ERROR("accept() error: %s", strerror(errno));
      exit(-1);
    }
    
    // Add client connection to epoll instance and connection table
    add_connection(epollfd, conn_fd);
    LOG_DEBUG("Client connected (fd=%d)\n", conn_fd);
  }
}

// ----------------------------------------------
// Network Connection API
// ----------------------------------------------

/**
 * Opens a non-blocking listening socket at the given port
 * 
 * @param port Port number that we're launching the server at.
 * @return The fd of the listening socket we've opened. Otherwise -1 on error.
 */
int open_listenfd(char *port) {
  struct addrinfo hints, *listp, *p;
  int listenfd, return_code, optval = 1;

  /*
  ##### Query Configs for potential server addresses #####
  - ai_socktype: Here we're looking for connection-oriented communication.
  - ai_flags: 
    a. AI_PASSIVE: Configured to let it know we want addresses that can be used by servers (passive/listening sockets).
      Equivalent to INADDR_ANY as well.
    b. AI_ADDRCONFIG: Returns IPv4 or IPv6 addresses, depending on what the host is configured to use.
    c. AI_NUMERICSERV: Indicates that 'service' parameter will be a port number.
  */
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
  hints.ai_flags |= AI_NUMERICSERV;
  return_code = getaddrinfo(NULL, port, &hints, &listp);
  if (return_code != 0) {
    LOG_ERROR("Getaddrinfo Error: %s\n", gai_strerror(return_code));
    return -1;
  }

  /*
  ##### Open Listening Socket #####
  1. Iterate through linked list of addrinfo structs to find an address to bind to
      a. Attempt to create a listening socket for the following protocol (IPv4, IPv6, etc.)
        socket type (connection-based or datagram based), and transport layer protocol.
      b. If listenfd < 0, we failed to create a socket for that addrinfo struct. Not a 
        critical error, so attempt the next addrinfo struct.
      c. Configure the socket s.t. we can reuse it immediately after closing it. 
        Good for fast restarts.
      d. Attempt to bind the socket to an IP:port, if it works, then break out of loop 
        since we got a success.
      e. Otherwise, we failed to bind the socket, attempt to close the socket to regain
        resources. If that also fails, exit out of function.
  2. Free address info linked list since we're done with it.
  3. If p is undefined, then we didn't even iterate through the list. This indicates 
    the OS didn't find any suitable addresses for the configurations applied.
  4. At this point we have a suitable listening socket. Attempt to open the listening socket
    which will cause our server to start buffering incoming connections. 
  */
  for (p = listp; p; p = p->ai_next) {
    listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (listenfd < 0) {
      continue;
    }
    
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    
    if (close(listenfd) < 0) {

      LOG_ERROR("open_listenfd close error: %s\n", strerror(errno));
      return -1;
    }
  }

  if (!p) {
    LOG_ERROR("open_listenfd error: No suitable addresses found!\n");
    freeaddrinfo(listp);
    return -1;
  }

  // Fix our listening socket on that port
  set_nonblocking_fd(listenfd);
  if (listen(listenfd, LISTENQ) < 0) {
    LOG_ERROR("open_listenfd listen error: Error opening the listening socket!\n");
    freeaddrinfo(listp);
    close(listenfd);
    return -1;
  }

  // Get the raw IP address (instead of hostname) and give port number as a string
  char host[1025];
  char service[32];
  int s = getnameinfo(p->ai_addr, p->ai_addrlen, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
  if (s == 0) {
    printf("TCP server running on %s:%s\n", host, service);
  } else {
    printf("Couldn't get server IP:port, but server should be running!\n");
  }
  
  freeaddrinfo(listp);
  return listenfd;
}

int init_server(char* port, int is_test) { 
  int rc = 0;
  if (is_test) {
    rc = init_db(DB_TEST_PATH);
  } else {
    // NOTE: If not we're in "test mode", then that means we're probably running
    // this process in the foreground where stdin is defined
    rc = init_db(DB_PATH);
  }
  if (rc == -1) {
    LOG_ERROR("init_db() Failed to initialize database!\n");
    return -1;
  }

  return open_listenfd(port);
}