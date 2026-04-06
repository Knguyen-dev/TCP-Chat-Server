#include "shared.h"
#include "server_utils.h"
#include "protocol.h"

/*
- conn_table: Dynamic array representing all connected clients
- num_users: Represents number of users registered in the application, online and offline.
- conn_table_mutex: Mutex that enforces mutual exclusion when updating the conn table.
- user_file_path: Path to the CSV file that contains all registered user information.
- user_file_mutex: Mutex that enforces mutual exclusion when updating the .csv.
*/
static Connections conn_table = {0};
static uint64_t num_users = 0;
static pthread_mutex_t conn_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char* user_file_path = "./src/user_file_db.csv";
static pthread_mutex_t user_file_mutex = PTHREAD_MUTEX_INITIALIZER;

// -----------------------------------
// Startup and Shutdown
// -----------------------------------

// TODO: Handle clean shutdown
int handle_shutdown(int SIGINT) {
  return 0;
}

// -----------------------------------
// Database (file) API
// -----------------------------------
// TODO: May change this code so that we can connect to a container running postgres server

/**
 * Loads the number of registers users in our app. 
 * 
 * NOTE: This doesn't use a mutex because we're assuming that this is called
 * there are other threads attempting to write the user file and num_users.
 */
void init_user_file_state() {
  // 1. Create file if it doesn't exist.
  FILE* fp = fopen(user_file_path, (const char *)'r');
  if (fp == NULL) {
    num_users = 0;
    return;
  }

  // 2. Get the number of users based on the IDs
  // NOTE: This assumes no one gets deleted.
  char line[512];
  uint32_t last_id = 0;
  while (fgets(line, sizeof(line), fp)) {
    uint32_t current_id;
    // We only care about the first column (the ID)
    if (sscanf(line, "%u,", &current_id) == 1) {
      if (current_id > last_id) {
       last_id = current_id;
      }
    }
  }
  num_users = last_id;
  fclose(fp);
  printf("Server init: %lu users loaded.\n", num_users);
}

/**
 * Inserts a new user into the database
 * @param user New user to write into the database
 * @return 0 if success, otherwise -1.
 * 
 * NOTE: This function is thread-safe as we use a mutex to lock access to the underlying file.
 */
int insert_user(user_t* user) {
  pthread_mutex_lock(&user_file_mutex);
  FILE* fp = fopen(user_file_path, (const char *)'a');
  if (fp == NULL) {
    pthread_mutex_unlock(&user_file_mutex);
    fprintf(stderr, "insert_user error: %s\n", strerror(errno));
    return -1;
  }

  /*
  ATTENTION:
  1. user_file_mutex is used with num_users. No other locations write 
    num_users for data integrity.
  2. No other places  updating user->id. During registration, other threads 
    should be reading user->id as the user itself is still being created. Meaning it 
    won't show up for other clients to read or something.
  */
  fprintf(fp, "%u,%s,%s\n", user->id, user->username, user->password);
  fclose(fp);
  num_users += 1;
  user->id = num_users;
  pthread_mutex_unlock(&user_file_mutex);
  return 0;
}

/**
 * Populates a user_t pointer with user data from the database
 * 
 * @param username Unique username of the user we're fetching.
 * @param user Pointer to the user_t struct that we're populating data with.
 * @return 0 on success (user found), otherwise -1 (user not found).
 */
int get_user_by_username(char* username, user_t *user) {
  pthread_mutex_lock(&user_file_mutex);
  FILE* fp = fopen(user_file_path, (const char*)'r');
  if (fp == NULL) {
    pthread_mutex_unlock(&user_file_mutex);
    return -1; 
  } 
  char line[512]; 
  int found = 0;
  while (fgets(line, sizeof(line), fp)) {
    unsigned int id;
    char current_username[MAX_USERNAME_SIZE+1];
    char current_password[MAX_PASSWORD_SIZE+1];

    // Scan for unsigned int, string, and string.
    // If found, update boolean, and populate struct with data.
    if (sscanf(line, "%u,%s,%s", &id, current_username, current_password) == 3) {
      if (strcmp(current_username, username) == 0) {
        found = 1;
        user->id = id;
        strcpy(user->username, (const char *)current_username);
        strcpy(user->password, (const char *)current_password);
        break;
      }
    }
  }
  fclose(fp);
  pthread_mutex_unlock(&user_file_mutex);
  return found;
}

/**
 * Populates a user_t pointer with user data from the database.
 * 
 * @param user_id The ID of the user in the database.
 * @param user Struct pointer that we'll populate with the user information as long as user is found.
 * @return 1 if user is found, otherwise 0.
 */
int get_user_by_userID(uint32_t user_id, user_t *user) {
    pthread_mutex_lock(&user_file_mutex);
    FILE* fp = fopen(user_file_path, (const char*)'r');
    if (fp == NULL) {
      pthread_mutex_unlock(&user_file_mutex);
      return -1; 
    } 

    char line[512]; 
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
      uint32_t id;
      char current_username[MAX_USERNAME_SIZE+1];
      char current_password[MAX_PASSWORD_SIZE+1];
      if (sscanf(line, "%u,%s,%s", &id, current_username, current_password) == 3) {
        if (id == user_id) {
          found = 1;
          user->id = id;
          strcpy(user->username, (const char *)current_username);
          strcpy(user->password, (const char *)current_password);
          break;
        }
      }
    }

    pthread_mutex_unlock(&user_file_mutex);
    return found;
}

// -----------------------------------
// Connection Table API (Dynamic Array)
// -----------------------------------

/**
 * Adds a new connection to the connection table
 * @param conn A new connection object
 * @return 0 on success, otherwise -1
 */
int add_connection(conn_t* conn) {
  pthread_mutex_lock(&conn_table_mutex);

  // Handle resizing array
  if (conn_table.count >= conn_table.capacity) {
    int new_capacity = 0;
    if (conn_table.capacity == 0) {
      new_capacity = 10;
    } else {
      new_capacity = conn_table.capacity * 2;
    }
    conn_t* temp = realloc(conn_table.items, new_capacity * sizeof(*conn_table.items));
    if (temp == NULL) {
      LOG_ERROR("Failed to resize the array to new capacity '%d'!", new_capacity);
      return -1;
    }
    conn_table.items = temp;
    conn_table.capacity = new_capacity;
  }

  // Write new connection into table and increase count
  conn_table.items[conn->connfd] = (conn_t){
    conn->addr,
    conn->user,
    conn->connfd
  };
  conn_table.count++;
  pthread_mutex_unlock(&conn_table_mutex);
  return 0;
}

/**
 * Removes a connection from the connection table
 * 
 * @param connfd Fd for the descriptor we want to remove
 * 
 * NOTE: This essentially logs someone log of the application. We won't 
 * just nullify and free the user pointer, but we'll clear all other fields.
 */
void remove_connection(int connfd) {
  pthread_mutex_lock(&conn_table_mutex);

  // If authenticated, free malloced user and nullify to prevent dangling pointer
  conn_t* conn = &conn_table.items[connfd];
  if (conn->user) {
    free(conn->user);
    conn->user = NULL;
  }

  *conn = (conn_t){0};
  conn_table.count--;
  pthread_mutex_unlock(&conn_table_mutex);  
}

/**
 * Authenticates a connection by making the user field point to a dynamically allocated user.
 * 
 * @param connfd Integer representing the client/user's socket connection descriptor.
 * @param user Pointer to dynamically allocated memory for storing a user.
 */
void authenticate_connection(int connfd, user_t* user) {
  pthread_mutex_lock(&conn_table_mutex);
  conn_table.items[connfd].user = user;
  pthread_mutex_unlock(&conn_table_mutex);  
}

// -----------------------------------
// Application-Level Services e.g. Register, Login
// -----------------------------------

/**
 * Registers a user into the application
 * 
 * @param connfd File descriptor associated with TCP connection socket
 * @param msg Pointer to request message struct that has the data needed to register a user.
 * @param extra_data Empty buffer that this function can use to add data to the response payload.
 * @param extra_data_len Amount of data (in bytes) that's copied into extra_data buffer, by this function.
 * @return 0 if success, otherwise an error response code.
 */
int register_user(int connfd, message_t *msg, uint8_t* extra_data, uint32_t *extra_data_len) {
  registration_credentials_t credentials = {0};
  int rc = parse_register_request(msg, &credentials);
  if (rc != 0) {
    return rc;
  }

  // If registered username isn't unique, reject request
  user_t user = {0};
  rc = get_user_by_username(credentials.username, &user);
  if (rc == 0) {
    fprintf(stderr, "Username '%s' already taken!\n", credentials.username);
    return RESP_ERROR_USER_EXISTS;
  }

  // Attempt to insert user into our file
  // NOTE: User has no ID yet, so set it to zero.
  size_t username_len = strlen(credentials.username);
  size_t password_len = strlen(credentials.password);
  user_t new_user = {0};
  new_user.id = 0; 
  strncpy(new_user.username, credentials.username, username_len);
  new_user.username[username_len] = '\0'; 
  strncpy(new_user.password, credentials.password, password_len);
  new_user.password[password_len] = '\0';
  if (insert_user(&new_user) != 0) {
    return RESP_ERROR_INTERNAL;
  }
  return RESP_OK;
}

/**
 * Logs a user in given their credentials in the request message.
 * 
 * @param connfd Connection socket associated with the client we're serving.
 * @param msg Pointer to message struct that contains client credentials.
 * @param extra_data Empty buffer that this function can use to add data to the response payload.
 * @param extra_data_len Amount of data (in bytes) that's copied into extra_data buffer, by this function.
 * @return 0 on success, otherwise a response code.
 */
int login_user(int connfd, message_t *msg, uint8_t *extra_data, uint32_t *extra_data_len) {
  login_credentials_t credentials = {0};
  int rc = parse_login_request(msg, &credentials);
  if (rc != 0) {
    return rc;
  }

  // Allocate memory for potentially new logged in user.
  user_t *user = (user_t *)malloc(sizeof(user_t));
  if (user == NULL) {
    fprintf(stderr, "malloc failed: %s\n", strerror(errno));
    return RESP_ERROR_INTERNAL;
  }

  // Does username exist in db?
  if (get_user_by_username(credentials.username, user) != 0) {
    free(user);
    fprintf(stderr, "User with username '%s' not found\n", credentials.username);
    return RESP_ERROR_USER_NOT_FOUND;
  }

  // Does password match with existing user?
  if (strcmp(user->password, credentials.password) != 0) {
    free(user);
    fprintf(stderr, "Invalid password for user '%s'\n", credentials.username);  
    return RESP_ERROR_INVALID_CREDENTIALS;
  }

  // Update/authenticate the user in the connection table
  authenticate_connection(connfd, user);

  return RESP_OK;
}

/**
 * Handles sending a message to remote peers.
 * @param connfd Socket descriptor associated with the TCP connection socket with the client sending this broadcast.
 * @param msg Pointer to the request message containing the broadcast request.
 * @param extra_data Empty buffer that this function can use to add data to the response payload.
 * @param extra_data_len Amount of data (in bytes) that's copied into extra_data buffer, by this function.
 * @return 0 on success, non-zero return code otherwise.
 */
int handle_broadcast_message(int connfd, message_t *msg, uint8_t *extra_data, uint32_t *extra_data_len) {
  int rc = peek_broadcast_type(msg);
  if (rc == -1) {
    return RESP_ERROR_INTERNAL;
  }

  switch (rc) {
    case TAG_WORLD_BROADCAST: {
      world_broadcast_t broadcast = {0};
      message_t response = {0};
      rc = parse_world_broadcast(msg, &broadcast);
      if (rc != 0) {
        return RESP_ERROR_INTERNAL;
      }
      build_world_broadcast_notification(&response, &broadcast);

      // Send response message to all authenticated users that aren't the sender
      pthread_mutex_lock(&conn_table_mutex);
      for (int i = 0; i < conn_table.capacity; i++) {
        if (conn_table.items[i].user == NULL || i == connfd) {
          continue;
        }
        write_one_message(i, &response);
      }
      pthread_mutex_unlock(&conn_table_mutex);
      break;
    }
    case TAG_P2P_BROADCAST: {
      p2p_broadcast_t broadcast = {0};
      message_t response = {0};
      rc = parse_p2p_broadcast(msg, &broadcast);
      if (rc != 0) {
        return RESP_ERROR_INTERNAL;
      }
      build_p2p_broadcast_notification(&response, &broadcast);

      // Search for recipient connection
      int recipient_connfd = -1;
      pthread_mutex_lock(&conn_table_mutex);
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
      pthread_mutex_unlock(&conn_table_mutex);

      if (recipient_connfd == -1) {
        return RESP_ERROR_USER_NOT_FOUND;
      }
      write_one_message(recipient_connfd, &response);
      break;
    }
    default:
      fprintf(stderr, "Received unknown broadcast tag '%d'. Skipping request!\n", rc);
  }

  return RESP_OK;
}

/**
 * Processes one complete message cycle: read, handle, and respond.
 * 
 * @param connfd Integer representing the connection descriptor
 * @return 0 on success, otherwise -1
 */
int serve_one_message(int connfd) {

  // 1. Read user request
  message_t request = {0};
  int read_result = read_one_message(connfd, &request);
  if (read_result == -1) {
    return -1; 
  }

  // 2. Serve request/handle the request
  uint8_t extra_data[MSG_MAX_PAYLOAD_SIZE] = {0};
  uint32_t extra_data_len = 0;
  int result;
  switch (request.type) {
    case REGISTER:
      result = register_user(connfd, &request, extra_data, &extra_data_len);
      break;
    case LOGIN:
      result = login_user(connfd, &request, extra_data, &extra_data_len);
      break;
    case CHAT:
      result = handle_broadcast_message(connfd, &request, extra_data, &extra_data_len);
    default:
      fprintf(stderr, "Received unknown message type '%d'!\n", request.type);
      result = RESP_ERROR_UNKNOWN_COMMAND;
  }

  // 3. Bulid server response/ACK for the client and write it over TCP
  message_t response;

  if (build_server_response(&response, result, extra_data, extra_data_len) != 0) {
    return -1;
  }
  if (write_one_message(connfd, &response) == -1) {
    LOG_ERROR("Error writing one message: %s\n", strerror(errno));
  }

  return 0;  
}

// ----------------------------------------------
// Network Connection API
// ----------------------------------------------
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

void *serve_connection(void *vargp) {
  int connfd = *((int*)vargp);
  pthread_detach(pthread_self());
  free(vargp);

  // 1. Create new connection and add it to the connection table.
  struct sockaddr_storage ip_address;
  socklen_t addr_len = sizeof(ip_address);
  if (getpeername(connfd, (struct sockaddr *)&ip_address, &addr_len) == -1) {
    fprintf(stderr, "getpeername failed, closing '%d': %s\n", connfd, strerror(errno));
    close(connfd);
    return NULL;
  }
  conn_t new_conn = {ip_address, NULL, connfd};
  int result = add_connection(&new_conn);
  if (result == -1) {
    fprintf(stderr, "add_connection failure: closing '%d'!\n", connfd);
    close(connfd);
    return NULL;
  }


  

  // 2. Infinite Connection Loop: Continue processing messages until disconnect or error
  while (serve_one_message(connfd) == 0) {}

  // 3. Connection Closure
  fprintf(stdout, "Connection '%d' closed.\n", connfd);
  remove_connection(connfd);
  close(connfd);
  return NULL;
}