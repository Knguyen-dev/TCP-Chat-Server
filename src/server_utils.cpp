#include "shared.hpp"
#include "server_utils.hpp"
#include "protocol.hpp"
#include "logger.hpp"
#include "db.hpp"
#include <chrono>
// #include <format> // requires C++20

// Connection tables
std::vector<conn_t *> conn_table;

/**
 * Makes a socket associated with a file descriptor nonblocking
 * @param fd The file descriptor associated with the socket.
 */
static void set_nonblocking_fd(int fd) {
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
    free(conn->user);
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
 * 
 * @param connfd Integer representing the client/user's socket connection descriptor.
 * @param user User that we're authenticating this connection with.
 * 
 * NOTE: This function will handle the dynamic memory allocation
 */
static void authenticate_connection(int connfd, user_t& user) {  
  user_t *new_user = (user_t *)malloc(sizeof(user_t));
  if (!new_user) {
    LOG_ERROR("malloc() failed in authenticate_connection()\n");
    return;
  }
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
 * @return 0 if success, otherwise an error response code.
 */
static int register_user(message_t& request) {
  registration_credentials_t credentials = {};
  int rc = parse_register_request(request, credentials);
  if (rc != 0) {
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
    return RESP_ERROR_USER_EXISTS;
  } else if (rc == -1) {
    LOG_ERROR("get_user_by_username Failed!\n");
    return RESP_ERROR_INTERNAL;
  }

  // Insert new user in the database
  user_t new_user{};
  new_user.id = 0; 
  new_user.username = credentials.username;
  new_user.password = credentials.password;
  if (insert_user(new_user) != 0) {
    LOG_ERROR("insert_user Failure!\n");
    return RESP_ERROR_INTERNAL;
  }

  return RESP_OK;
}

/**
 * Logs a user in given their credentials in the request message.
 * 
 * @param conn Connection associated with the client we're serving.
 * @param msg Request message that represents the login request.
 * @return 0 on success, otherwise a response code.
 */
static int login_user(conn_t& conn, message_t& request) {
  login_credentials_t credentials{};
  int rc = parse_login_request(request, credentials);
  if (rc != 0) {
    return rc;
  }
  
  // Does username exist in db?
  user_t user{};
  rc = get_user_by_username(string_to_lower(credentials.username), user);
  if (rc != 1) {
    if (rc == 0) {
      LOG_DEBUG("User with username '%s' not found\n", credentials.username.data());
      return RESP_ERROR_USER_NOT_FOUND;
    } else {
      LOG_ERROR("get_user_by_username() Failed!\n");
      return RESP_ERROR_INTERNAL;
    }
  }

  if (user.password != credentials.password) {
    LOG_DEBUG("Invalid password for user '%s'\n", credentials.username.data());
    return RESP_ERROR_INVALID_CREDENTIALS;
  }

  // Update/authenticate the user in the connection table
  authenticate_connection(conn.fd, user);

  return RESP_OK;
}

/**
 * Handles sending a message to remote peers.
 * @param conn Connection associated with the TCP connection socket with the client sending this broadcast.
 * @param msg Broadcast request message containing the broadcast request.
 * @return 0 on success, non-zero return code otherwise.
 */
static int handle_broadcast_message(conn_t& conn, message_t& request) {
  tlv_tag_t broadcast_tag = peek_broadcast_type(request);
  switch (broadcast_tag) {
    case TAG_WORLD_BROADCAST: {      
      // Parse request and build world broadcast; write the response message into a buffer
      world_broadcast_t broadcast{};
      message_t response{};
      if (parse_world_broadcast(request, broadcast) != 0) {
        return RESP_ERROR_INTERNAL;
      }
      build_world_broadcast_notification(response, broadcast);
      std::vector<uint8_t> serialized_response;
      write_message_to_buffer(serialized_response, response);

      // Write serialized response data into all valid buffers
      for (size_t i = 0; i < conn_table.size(); i++) {
        // If looking at the sender, connection slot is closed, or user isn't authentcated, then SKIP
        if (i == (size_t)conn.fd || conn_table[i] == nullptr || conn_table[i]->user == nullptr) {
          continue;
        }
        std::vector<uint8_t>& output_buffer = conn_table[i]->outgoing;
        output_buffer.insert(output_buffer.end(), serialized_response.begin(), serialized_response.end());
      }
      break;
    }
    case TAG_P2P_BROADCAST: {
      // Parse P2P broadcast request and build broadcast 
      p2p_broadcast_t broadcast{};
      message_t response{};
      if (parse_p2p_broadcast(request, broadcast) != 0) {
        return RESP_ERROR_INTERNAL;
      }
      build_p2p_broadcast_notification(response, broadcast);

      // Search for recipient connection in the table
      conn_t* recipient_conn = nullptr;
      for (size_t i = 0; i < conn_table.capacity(); i++) {
        // If looking at the sender, connection slot is closed, or user isn't authentcated, then SKIP
        if (i == (size_t)conn.fd || conn_table[i] == nullptr || conn_table[i]->user == nullptr) {
          continue;
        }
        if (conn.user->username == broadcast.recipient_username) {
          recipient_conn = conn_table[i];
          break;
        }
      }
      if (recipient_conn == nullptr) {
        return RESP_ERROR_USER_NOT_FOUND;
      }
      
      // Get serialized response and write it to the output buffer
      std::vector<uint8_t> serialized_response;
      write_message_to_buffer(serialized_response, response);
      std::vector<uint8_t>& output_buffer = recipient_conn->outgoing;
      output_buffer.insert(output_buffer.end(), serialized_response.begin(), serialized_response.end());
      break;
    }
    default:
      LOG_WARN("Received unknown broadcast tag '%d'. Skipping request!\n", broadcast_tag);
  }

  return RESP_OK;
}

// --------------------------------------
// Event Loop Utils and API
// --------------------------------------

/**
 * Attempts to serve one request message for the given connection
 * @param conn Connection that we're trying to serve one message from.
 * @return true when we're able to serve one request, otherwise false.
 */
static bool try_one_request(conn_t& conn) {
  // Don't proceed if we don't have message header + 1 byte of payload yet.
  if (conn.incoming.size() < MSG_HEADER_SIZE + 1) {
    return false;
  }

  // NOTE: At this point you knwo that message_t::payload points somewhere in conn_t::incoming
  message_t message;
  parse_message(conn.incoming, message);
  if (message.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    LOG_ERROR("Message payload size was too big. Closing connection!\n");
    conn.want_close = true;
    return false;
  }
  
  // Don't proceed if we don't have at least a full message worth of data.
  if (MSG_HEADER_SIZE + message.payload_length > conn.incoming.size()) {
    return false;
  }

  int result;
  switch (message.type) {
    case REGISTER:
      result = register_user(message);
      break;
    case LOGIN:
      result = login_user(conn, message);
      break;
    case CHAT:
      result = handle_broadcast_message(conn, message);
      break;
    default:
      LOG_WARN("Received unknown message type '%d'!\n", message.type);
      result = RESP_ERROR_UNKNOWN_COMMAND;
  }

  // At this point, we have successfully processed the message request.
  // We can erase the request message data from the conn_t::incoming buffer
  buf_consume(conn.incoming, MSG_HEADER_SIZE+message.payload_length);
  
  // ATP the entire response message has been 
  message_t response = build_server_response(static_cast<response_code_t>(result), NULL, 0);
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
  while (try_one_request(conn)) {};

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
    if (conn.outgoing.size() == 0) {
      set_connection_state(epollfd, conn, true);
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

int init_server(char* port) {  
  int rc = init_db();
  if (rc == -1) {
    return -1;
  }
  return open_listenfd(port);
}