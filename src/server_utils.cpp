#include "shared.hpp"
#include "server_utils.hpp"
#include "protocol.hpp"
#include "logger.hpp"
#include "db.hpp"
#include <chrono>
// #include <format> // requires C++20

// Connection tables
std::vector<conn_t *> conn_table;

// -----------------------------------
// TODO: Metrics
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

void log_server_metrics() {

  // 1. Get number of unauth and auth connections
  int num_connected{0};
  int num_auth{0};
  for (conn_t* conn : conn_table) {
    if (conn == nullptr) {
      continue;
    }
    if (conn->user != nullptr) {
      num_auth++;
    }
    num_connected++;
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
 * @param conn Connection object whose state we're updating. The fd must be an existing fd in the epoll interest list.
 * @param state The state we're updating the connection to reflect.
 * @note Updates application state, like the want_read or want_write, but also updates the 
 * epoll's interest list to reflect the TCP 's I/O readiness.
 * @note For closing a connection, we literally set conn_t::want_close to true, which 
 * will make the connection be processed through the event loop and eventaully end up in
 * remove_connection.
 */
static void set_connection_state(int epollfd, conn_t& conn, bool is_read) {
  if (is_read) {
    conn.want_read = true;
    conn.want_write = false;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = conn.fd;  
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, conn.fd, &ev) == -1) {
      LOG_ERROR("epoll_ctl failure: %s\n", strerror(errno));
    }
  } else {
    conn.want_read = false;
    conn.want_write = true;
    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLET;
    ev.data.fd = conn.fd;  
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, conn.fd, &ev) == -1) {
      LOG_ERROR("epoll_ctl failure: %s\n", strerror(errno));
    } 
  }
}

// -----------------------------------
// Connection Table API (Dynamic Array)
// -----------------------------------
/**
 * Adds an unauthenticated client connection to the connection table and epoll instance
 * @param fd Fd for the TCP connection socket
 * @param client_address The IP address of the remote peer.
 * @param client_len Number of bytes representing the IP address of the remote peer.
 */
void add_connection(int epollfd, int fd, struct sockaddr_storage client_address, socklen_t client_len) {
  set_nonblocking_fd(fd);
  
  // Add TCP fd in the interest list; reading and edge-triggered
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLET; 
  ev.data.fd = fd;
  int rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
  if (rc == -1) {
    LOG_ERROR("epoll_ctl error: %s\n", strerror(errno));
    return;
  }

  // Create new connection socket that starts off reading as well.
  conn_t* conn = new conn_t();
  memcpy(&conn->addr, &client_address, sizeof(client_address));
  conn->user = nullptr;
  conn->fd = fd;
  conn->want_read = true;
  conn->want_write = false;
  conn->want_close = false;

  // NOTE: conn_table is index by fd. If fd = 5, then conn_table needs to be at minimum
  // size fd+1, so that conn_table[fd] is a valid index.
  if (conn_table.size() <= (size_t)conn->fd) {
    conn_table.resize(conn->fd + 1);
  }
  
  conn_table[conn->fd] = conn;
}

/**
 * Closes a connection from the TCP server, updates connection table, removes from epoll instance
 * @param fd The fd for the TCP connection socket
 * @param epollfd The fd for the epoll instance.
 */
void remove_connection(int fd, int epollfd) {

  // If authenticated, free malloced user and nullify to prevent dangling pointer
  conn_t* conn = conn_table[fd];
  if (conn->user) {
    delete conn->user;
    conn->user = nullptr;
  }

  // a. Close TCP connection with the client
  // b. Free the heap memory representing the conn_t struct
  // c. To prevent referencing a freed pointer, nullify this slot in the array
  // d. Remove the TCP connection socket's fd from the interest list.
  close(fd);
  delete conn;
  conn_table[fd] = nullptr;
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
}

/**
 * Authenticates a connection by making the user field point to a dynamically allocated user.
 * @param connfd Integer representing the client/user's socket connection descriptor.
 * @param user User that we're authenticating this connection with.
 * @note This function will handle the dynamic memory allocation
 */
static void authenticate_connection(int connfd, user_t& user) {
  user_t* new_user = new user_t();
  *new_user = user;
  conn_table[connfd]->user = new_user;
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
  user_t existing_user{};
  rc = get_user_by_username(string_to_lower(credentials.username), existing_user);
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
  user_t new_user{};
  new_user.id = 0; 
  new_user.username = credentials.username;
  new_user.password = credentials.password;
  if (insert_user(new_user) != 0) {
    LOG_ERROR("insert_user Failure!\n");
    response = build_server_response(REGISTER, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  if (build_register_response(response, new_user) != 0) {
    LOG_ERROR("build_register_response() failed!\n");
    response = build_server_response(REGISTER, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  return RESP_OK;
}

/**
 * Logs a user in given their credentials in the request message.
 * 
 * @param conn Connection associated with the client we're serving.
 * @param request Populated request message that represents the login request.
 * @param response Empty response message that the callee will modify with information.
 * @return 0 on success, otherwise a response code.
 */
static int login_user(conn_t& conn, message_t& request, message_t& response) {
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

  // Update/authenticate the user in the connection table
  authenticate_connection(conn.fd, user);

  if (build_login_response(response, user) != 0) {
    LOG_ERROR("build_login_response() failed!\n");
    response = build_server_response(LOGIN, RESP_ERROR_INTERNAL, NULL, 0);
    return RESP_ERROR_INTERNAL;
  }

  return RESP_OK;
}

/**
 * Handles sending a message to remote peers.
 * @param conn Connection associated with the TCP connection socket with the client sending this broadcast.
 * @param epollfd Fd associated with the epoll instance we're using.
 * @param request Request message containing the broadcast request.
 * @param response Response message that the callee will populate with data about the result of this operation.
 * @return 0 on success, non-zero return code otherwise.
 */
static int handle_broadcast_message(conn_t& conn, int epollfd, message_t& request, message_t& response) {
  // If connection isn't authenticated, reject the request
  // NOTE: Only authenticated users are allowed to send broadcasts.
  if (conn.user == nullptr) {
    LOG_DEBUG("Unauthenticated user attempted to send a broadcast message!\n");
    response = build_server_response(CHAT, RESP_ERROR_INVALID_CREDENTIALS, NULL, 0);
    return RESP_ERROR_INVALID_CREDENTIALS;
  }

  tlv_tag_t broadcast_tag = peek_broadcast_type(request);
  switch (broadcast_tag) {
    case TAG_WORLD_BROADCAST: {      
      world_broadcast_t broadcast{};
      
      // 1. Parse the user's broadcast request to get the broadcast information.
      int ret = parse_world_broadcast(request, broadcast);
      if (ret != 0) {
        LOG_ERROR("parse_world_broadcast Failure!\n");
        response = build_server_response(CHAT, static_cast<response_code_t>(ret), NULL, 0);
        return ret;
      }

      if (broadcast.message_content.empty()) {
        LOG_DEBUG("Received world broadcast with empty message content!\n");
        response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
        return RESP_ERROR_MALFORMED;
      }

      if (conn.user->username != broadcast.sender_username) {
        LOG_DEBUG("Received world broadcast, but client username doesn't match sender username!\n");
        response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
        return RESP_ERROR_MALFORMED;
      }

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
      // So skip the sender, closed connections (nullptr), and unauthenticated (conn_t::user == nullptr)
      std::vector<uint8_t> serialized_response;
      write_message_to_buffer(serialized_response, response);
      for (size_t i = 0; i < conn_table.size(); i++) {
        // If looking at the sender, connection slot is closed, or user isn't authentcated, then SKIP
        if (i == static_cast<size_t>(conn.fd) || conn_table[i] == nullptr || conn_table[i]->user == nullptr) {
          continue;
        }

        // NOTE: Need to set the recipient connections to a writing state to send them data.
        buf_append(conn_table[i]->outgoing, serialized_response.data(), serialized_response.size());
        set_connection_state(epollfd, *conn_table[i], false);
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

      if (conn.user->username != broadcast.sender_username) {
        LOG_DEBUG("Received p2p broadcast, but client username doesn't match sender username!\n");
        response = build_server_response(CHAT, RESP_ERROR_MALFORMED, NULL, 0);
        return RESP_ERROR_MALFORMED;
      }

      if (conn.user->username == broadcast.recipient_username) {
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
      // NOTE: 
      // a. Again skip the sender, closed connections (nullptr), and unauthenticated connections.
      // b. Only match the connection whose username is equal to the broadcast recipient username.
      conn_t* recipient_conn = nullptr;
      for (size_t i = 0; i < conn_table.size(); i++) {
        // If looking at the sender, connection slot is closed, or user isn't authentcated, then SKIP
        if (i == (size_t)conn.fd || conn_table[i] == nullptr || conn_table[i]->user == nullptr) {
          continue;
        }
        if (conn_table[i]->user->username == broadcast.recipient_username) {
          recipient_conn = conn_table[i];
          break;
        }
      }
      if (recipient_conn == nullptr) {
        response = build_server_response(CHAT, RESP_ERROR_USER_NOT_FOUND, NULL, 0);
        return RESP_ERROR_USER_NOT_FOUND;
      }

      // Send the broadcast to the recipient. 
      // NOTE: This function will return, allowing the caller to have the response.
      // Then the caller will handle serializing this response again and sending it to the client's outgoing buffer.
      std::vector<uint8_t> serialized_response;
      write_message_to_buffer(serialized_response, response);
      buf_append(recipient_conn->outgoing, serialized_response.data(), serialized_response.size());
      set_connection_state(epollfd, *recipient_conn, false);
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
 * @param conn Connection that we're trying to serve one message from.
 * @param epollfd Epollfd that's needed when we're handling a broadcast request.
 * @return true when we're able to serve one request, otherwise false.
 */
static bool try_one_request(conn_t& conn, int epollfd) {
  // Don't proceed if we don't have message header + 1 byte of payload yet.
  if (conn.incoming.size() < MSG_HEADER_SIZE + 1) {
    return false;
  }

  message_t request;
  parse_message(conn.incoming, request);
  if (request.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Message payload size was too big. Closing connection!\n");
    conn.want_close = true;
    return false;
  }
  
  // Don't proceed if we don't have at least a full message worth of data.
  if (MSG_HEADER_SIZE + request.payload_length > conn.incoming.size()) {
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
      login_user(conn, request, response);
      break;
    case CHAT:
      handle_broadcast_message(conn, epollfd, request, response);
      break;
    default:
      LOG_WARN("Received unknown message type '%d'!\n", request.type);
      response = build_server_response(request.type, RESP_ERROR_UNKNOWN_COMMAND, NULL, 0);
      break;
  }
  

  // At this point, we have successfully processed the message request.
  // We can erase the request message data from the conn_t::incoming buffer
  buf_consume(conn.incoming, MSG_HEADER_SIZE+request.payload_length);

  write_message_to_buffer(conn.outgoing, response);
  return true; 
}

void handle_read_connection(conn_t& conn, int epollfd) {
  uint8_t buffer[64 * 1024];

  while (true) {
    ssize_t rv = read(conn.fd, buffer, sizeof(buffer));
    if (rv < 0) {
      // Buffer is fully drained; we can safely stop and wait for next ET notification
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      // Otherwise, we had an actual read() error, close connection
      LOG_ERROR("read() error: %s\n", strerror(errno));
      conn.want_close = true;
      return;
    } else if (rv == 0) {
      if (conn.incoming.size() == 0) {
        if (conn.user) {
          LOG_DEBUG("Remote Peer (fd=%d, username=%s) disconnected!\n", conn.fd, conn.user->username.data());
        } else {
          LOG_DEBUG("Remote Peer (fd=%d) disconnected!\n", conn.fd);
        }
      } else {
        LOG_DEBUG("Remote Peer (fd=%d) unexpected EOF!\n", conn.fd);
      }
      conn.want_close = true;
      return;
    }
    buf_append(conn.incoming, buffer, rv);
  }

  // ### ASIDE: Core State Machine Logic ###
  // 1. Append bytes to the end of conn_t::incoming in handle_read_connection.
  // 2. Erase bytes from the start of conn_t::incoming in try_one_request.
  // 3. Append bytes to the end of conn_t::outgoing  in try_one_request
  // 4. Erase bytes from the start of conn_t::outgoing in handle_write_connection
  // NOTE: while loop means if we read multiple requests, then we'll try to process multiple (request pipelining).
  while (try_one_request(conn, epollfd)) {};

  // If we have outgoing messages to send (due to successfully parsing a request message), 
  // then update the state machine to indicate that we want to write to the socket.
  if (conn.outgoing.size() > 0) {
    set_connection_state(epollfd, conn, false);
  }
}

void handle_write_connection(conn_t& conn, int epollfd) {
  while (true) {
    ssize_t rv = write(conn.fd, &conn.outgoing[0], conn.outgoing.size());
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return; 
      }
      // Otherwise an actual error happened.
      LOG_ERROR("write() error. Closing connection '%d'\n", conn.fd);
      conn.want_close = true;
      return;
    }

    buf_consume(conn.outgoing, (size_t)rv);

    // NOTE: If no messages left to write, 
    // a. Set connection to read mode
    // b. Exit out of this function since we're done reading
    if (conn.outgoing.size() == 0) {
      set_connection_state(epollfd, conn, true);
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
    add_connection(epollfd, conn_fd, client_address, client_len);
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

  freeaddrinfo(listp);
  if (!p) {
    LOG_ERROR("open_listenfd error: No suitable addresses found!\n");
    return -1;
  }

  set_nonblocking_fd(listenfd);
  if (listen(listenfd, LISTENQ) < 0) {
    LOG_ERROR("open_listenfd listen error: Error opening the listening socket!\n");
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
  
  return listenfd;
}

int init_server(char* port, int is_test) {  
  int rc = 0;
  if (is_test) {
    rc = init_db(DB_TEST_PATH);
  } else {
    rc = init_db(DB_PATH);
  }
  if (rc == -1) {
    LOG_ERROR("init_db() Failed to initialize database!\n");
    return -1;
  }
  return open_listenfd(port);
}