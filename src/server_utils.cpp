#include "shared.hpp"
#include "server_utils.hpp"
#include "protocol.hpp"
#include "db.hpp"


static std::vector<conn_t *> conn_table;

// -----------------------------------
// Connection Table API (Dynamic Array)
// -----------------------------------

/**
 * Adds an unauthenticated client connection to the connection table
 * @param fd Fd for the TCP connection socket
 * @param client_address The IP address of the remote peer.
 * @param client_len Number of bytes representing the IP address of the remote peer.
 */
void add_connection(int fd, struct sockaddr_storage client_address, socklen_t client_len) {
  set_nonblocking_fd(fd);
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
 * Closes a connection from the TCP server
 * @param fd The fd for the TCP connection socket
 */
void remove_connection(int fd) {
  // If authenticated, free malloced user and nullify to prevent dangling pointer
  conn_t* conn = conn_table[fd];
  if (conn->user) {
    free(conn->user);
    conn->user = nullptr;
  }

  // TODO: Please nullify the pointer since our event loop code depends on that

  // Zero out the corresponding conn_t struct and close the TCP connection socket
  *conn = (conn_t){0};
  close(fd);
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
  memcpy(new_user, &user, sizeof(user));
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
  registration_credentials_t credentials = {0};
  int rc = parse_register_request(request, credentials);
  if (rc != 0) {
    return rc;
  }

  // If registered username isn't unique, reject request
  // NOTE: This approach preserves the casing of the usernames, but enforces 
  // case-insensitive uniqueness. So 'Alice' and 'alice' are considered the same username, and 
  // if 'Alice' is registered first, then 'alice' will be rejected as a registration username.
  user_t user = {0};
  rc = get_user_by_username(string_to_lower(credentials.username), user);
  if (rc == 1) {
    fprintf(stderr, "Username '%s' already taken!\n", credentials.username);
    return RESP_ERROR_USER_EXISTS;
  } else if (rc == -1) {
    LOG_ERROR("get_user_by_username Failed!\n");
    return RESP_ERROR_INTERNAL;
  }

  // Insert new user in the database
  user_t new_user = {0};
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
  login_credentials_t credentials = {0};
  int rc = parse_login_request(request, credentials);
  if (rc != 0) {
    return rc;
  }
  
  // Does username exist in db?
  user_t user = {0};
  rc = get_user_by_username(string_to_lower(credentials.username), user);
  if (rc != 1) {
    if (rc == 0) {
      fprintf(stdout, "User with username '%s' not found\n", credentials.username);
      return RESP_ERROR_USER_NOT_FOUND;
    } else {
      LOG_ERROR("get_user_by_username() Failed!\n");
      return RESP_ERROR_INTERNAL;
    }
  }

  if (user.password != credentials.password) {
    fprintf(stderr, "Invalid password for user '%s'\n", credentials.username);  
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
      world_broadcast_t broadcast = {0};
      message_t response = {0};
      if (parse_world_broadcast(request, broadcast) != 0) {
        return RESP_ERROR_INTERNAL;
      }
      build_world_broadcast_notification(response, broadcast);

      for (size_t i = 0; i < conn_table.size(); i++) {
        // If connection is closed OR not authenticated OR is the client sending the broadcast, skip it.
        if (conn_table[i] == nullptr || conn_table[i]->user == nullptr || i == (size_t)conn.fd) {
          continue;
        }


        

        // TODO: fix this
        // std::vector<uint8_t> serialized_msg = response.serialize(); // or however you convert msg to bytes

        /*
        TODO: I don't know the solution
        - Trying to response the broadcast message into the outgoing buffer of each remote peer struct 
          could be quite inefficient. But the main issue would be, what if those outgoing buffers already have partial
          messages that haven't been sent. For example, let's focus on one remote peer connection C.
            1. Peer A sends a broadcast M1, and we write 75% of M1 into C's outgoing buffer.
            2. Peer B sends broadcast M2, about 0.10 seconds later, and then now 25% of M2 is written into C's outgoing buffer,
              right after the byte stream of M2.

        At this point, this buffer isn't valid. The main goal now is that we want to maintain this zero-copy efficiency
        of sending data into an outgoing buffer, whilst having data integrity. Maybe we have to have some locking mechanism
        , extra state, or maybe there's some other approaches out there that we haven't seen yet. 
        */
        



      }
      break;
    }
    case TAG_P2P_BROADCAST: {
      p2p_broadcast_t broadcast = {0};
      message_t response = {0};
      if (parse_p2p_broadcast(request, broadcast) != 0) {
        return RESP_ERROR_INTERNAL;
      }
      build_p2p_broadcast_notification(response, broadcast);

      // Search for recipient connection
      int recipient_connfd = -1;
      for (int i = 0; i < conn_table.capacity; i++) {
        conn_t conn = conn_table.items[i];
        if (conn.user == NULL || i == connfd) {
          continue;
        }
        if (strcmp(conn.user->username, broadcast.recipient_username) != 0) {
          continue;
        }
        recipient_connfd = i;
        break;
      }
    
      if (recipient_connfd == -1) {
        return RESP_ERROR_USER_NOT_FOUND;
      }
      write_one_message(recipient_connfd, &response);
      break;
    }
    default:
      fprintf(stderr, "Received unknown broadcast tag '%d'. Skipping request!\n", broadcast_tag);
  }

  return RESP_OK;
}

// --------------------------------------
// Event Loop Utils and API
// --------------------------------------

/**
 * Makes a socket associated with a file descriptor nonblocking
 * @param fd The file descriptor associated with the socket.
 */
static void set_nonblocking_fd(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    fprintf(stderr, "fcntl error: %s\n", strerror(errno));
    exit(-1);
  }

  int rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (rc == -1) {
    fprintf(stderr, "fcntl error: %s\n", strerror(errno));
    exit(-1);
  }
}

/**
 * Append bytes to the back of a given buffer
 * @param buf Reference variable to the buffer we're appending data into.
 * @param data Pointer to the a buffer we'll read data from and copy into said buffer.
 * @param len Number of bytes we're copying from data into buf.
 */
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

/**
 * Remove data from the start of the buffer
 * @param buf Reference to the buffer that we're erasing data from.
 * @param n Number of bytes we want to remove from the start of the buffer.
 */
static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

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

  message_t message;
  parse_message(conn.incoming, message);
  if (message.payload_length > MSG_MAX_PAYLOAD_SIZE) {
    fprintf(stderr, "Message payload size was too big. Closing connection!\n");
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
      // TODO: Place replace usage of extra_data buffer and extra_data_len
      // with the conn_t::outgoing vector. I think with our FSM, we wouldn't be be currently
      // reading data if we already had data to write. But verify this.
      register_user(message);
      break;
    case LOGIN:
      login_user(conn, message);
      break;
    case CHAT:
      result = handle_broadcast_message(connfd, &request, extra_data, &extra_data_len);
      break;
    default:
      fprintf(stderr, "Received unknown message type '%d'!\n", message.type);
      result = RESP_ERROR_UNKNOWN_COMMAND;
  }


  // TODO: Build server response/ACK for the client.  
  // Write the response message into the outgoing buffer, which will the state change to writing
}

void handle_read_connection(conn_t* conn) {
  uint8_t buffer[64 * 1024];
  ssize_t rv = read(conn->fd, buffer, sizeof(buffer));
  if (rv < 0) {
    // No more data, early return.
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    // Otherwise, we had an actual read() error 
    fprintf(stderr, "read() error: %s\n", strerror(errno));
    conn->want_close = true;
    return;
  } else if (rv == 0) {
    if (conn->incoming.size() == 0) {
      msg("Remote peer closed the connection!");
    } else {
      msg("Remote peer unexpected EOF!");
    }
    conn->want_close = true;
    return;
  }

  
  // Write data into incoming buffer. 
  buf_append(conn->incoming, buffer, rv);

  // TODO: Later see and verify pipelining
  try_one_request(conn);

  // If we have outgoing messages to send (due to successfully parsing a request message), 
  // then update the state machine to indicate that we want to write to the socket.
  if (conn->outgoing.size() > 0) {
    conn->want_read = false;
    conn->want_write = true;
  }
}

// TODO: Implement this
void handle_write_connection(conn_t* conn) {

}

void accept_all_connections(int listenfd) {
  socklen_t client_len;
  struct sockaddr_storage client_address;
  while (true) {
    client_len = sizeof(struct sockaddr_storage);
    int conn_fd = accept(listenfd, (struct sockaddr *)&client_address, &client_len);
    if (conn_fd < 0) {

      // If blocking, then no connections are left in the kernel, exit
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      
      // Otherwise, we had a real accept() error
      fprintf(stderr, "accept() error: %s", strerror(errno));
      exit(-1);
    }

    // Add client connection to the connection table
    add_connection(conn_fd, client_address, client_len);
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
    fprintf(stderr, "Getaddrinfo Error: %s\n", gai_strerror(return_code));
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
      fprintf(stderr, "open_listenfd close error: %s\n", strerror(errno));
      return -1;
    }
  }

  freeaddrinfo(listp);
  if (!p) {
    fprintf(stderr, "open_listenfd error: No suitable addresses found!\n");
    return -1;
  }

  set_nonblocking_fd(listenfd);
  if (listen(listenfd, LISTENQ) < 0) {
    fprintf(stderr, "open_listenfd listen error: Error opening the listening socket!\n");
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