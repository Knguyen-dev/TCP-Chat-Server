#include "shared.h"


const char* response_messages[] = {
  [RESP_OK] = "Success",
  [RESP_ERROR_MALFORMED] = "Malformed request",
  [RESP_ERROR_USER_EXISTS] = "Username already taken",
  [RESP_ERROR_USER_NOT_FOUND] = "User not found",
  [RESP_ERROR_INVALID_CREDENTIALS] = "Invalid credentials",
  [RESP_ERROR_INTERNAL] = "Internal server error",
  [RESP_ERROR_UNKNOWN_COMMAND] = "Unknown command"
};


// Dynamically allocated array representing all connected clients on the TCP server.
// This is indexed by connfd to make things a little easier.
static uint64_t num_users = 0;
static conn_t *conn_table = NULL;
static uint32_t conn_table_capacity = 10;
static uint32_t num_conns = 0;
static pthread_mutex_t conn_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char* user_file_path = "./src/user_file_db.csv";
static pthread_mutex_t user_file_mutex = PTHREAD_MUTEX_INITIALIZER;


/**
 * Writes a TLV into a buffer.
 * 
 * @param buf A double pointer to a buffer. The motivation is that with a single pointer *buf
 *            modifications like buf += 1, doesn't update the pointer in the caller. However by 
 *            using a double pointer, we'll be able to update the buffer pointer in the caller.
 * @param tag The tag that we're giving the TLV; "what is it?".
 * @param len The size of the data (in bytes from 0-255) of the value, "how many bytes is it?".
 * @param value A pointer to the value that we want to write into the buffer
 * @param convert_to_network 1 to convert the value into network-byte-order, otherwise 0 for no conversion.
 *
 * NOTE: 
 * - Intends to mainly be a helper function to create_response.
 * - If len > 1, we know that the value ("payload") is a multi-byte type.
 * - Limitations: Since len is a uint8_t we can only represent payloads of size [0, 255] bytes. If we wanted to represent bigger payloads, 
 * we'd simply upgrade to uint16_t, allowing us to write values of size [0, 65335] bytes, which will later be useful for messages. Of course, 
 * if we decide to use uint16_t, we'd need to ensure the 16-bit integer is represented in network-byte-order and probably use memcpy to copy 
 * from bytes from the integer into the buffer.
 */
void write_tlv(uint8_t **buf, tlv_tag_t tag, uint8_t len, const void *value, int convert_to_network) {
  
  // 1. Write tag and length (1 byte each, no flipping)
  *(*buf)++ = tag; // equivalent to **buf = tag, then (*buf)++
  *(*buf)++ = len;

  // 2. Write the convered (if necessary) byte seqeunce into the buffer
  if (convert_to_network) {
    // If we want to convert a multi-byte number into network-byte-order. Three cases:
    // a. Converting a 2-byte sequence
    // b. Converting a 4-byte sequence
    // c. Converting a 1-byte sequence (no conversion needed even if the caller wants it)
    if (len == 2) {
      uint16_t val;
      memcpy(&val, value, 2);
      val = htons(val);
      memcpy(*buf, &val, 2);
    } else if (len == 4) {
      uint32_t val;
      memcpy(&val, value, 4);
      val = htonl(val);
      memcpy(*buf, &val, 4);
    } else {
      memcpy(*buf, value, len);
    }
  } else {
    // Else, the caller intends to write a string into the buffer
    memcpy(*buf, value, len);
  }

  // 3. Advance buffer pointer by the byte size of the value.
  *buf += len;
}

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
// Connection Table API
// -----------------------------------

/**
 * Initialies the dynamically allocated connection table.
 * 
 * NOTE: This should run at the start of the program.
 */
void init_conn_table() {
  conn_table = (conn_t *)malloc(sizeof(conn_t) * conn_table_capacity);
  if (conn_table == NULL) {
    fprintf(stderr, "Malloc failed on connection table: %s\n", strerror(errno));
    exit(1);
  }
  for (uint32_t i = 0; i < conn_table_capacity; i++) {
    conn_table[i].connfd = -1;
    conn_table[i].user = NULL;
    conn_table[i].ip_address = (struct sockaddr_storage){0};
  }
}

/**
 * Adds a new connection to the connection table
 * @param new_conn A new connection object
 * @return 0 on success, otherwise -1
 */
int add_connection(conn_t *new_conn) {
  pthread_mutex_lock(&conn_table_mutex);
  if (new_conn->connfd > conn_table_capacity - 1) {
    conn_table_capacity *= 2; 
    conn_t* new_conn_table = realloc(conn_table, conn_table_capacity);
    if (new_conn_table == NULL) {
      fprintf(stderr, "add_connection failed: %s", strerror(errno));
      return -1;
    }
    conn_table = new_conn_table;
  }

  conn_table[new_conn->connfd] = (conn_t){
    new_conn->ip_address,
    new_conn->is_authenticated,
    new_conn->connfd,
    new_conn->user
  };

  num_conns += 1;
  pthread_mutex_unlock(&conn_table_mutex);
  return 0;
}

/**
 * Removes a connection from the connection table
 * 
 * @param connfd The connection we want to remove
 */
int remove_connection(int connfd) {
  pthread_mutex_lock(&conn_table_mutex);
  conn_table[connfd].connfd = -1;
  num_conns -= 1;  
  pthread_mutex_unlock(&conn_table_mutex);  
  return 0;
}

/**
 * Updates a connection object by populating it with user, representing it as authenticated.
 * 
 * @param connfd Connection descriptor
 * @param user Pointer to a user struct
 * @return 0s
 * 
 * NOTE: 
 * - For a given connfd, only a single thread writes the slot.
 *   Potentially you've have another read thread it.
 * - Don't think i really need a mutex for thi?
 */
int update_conn_with_user(int connfd, user_t *user) {
  conn_table[connfd].is_authenticated = 1;
  conn_table[connfd].user = user;
  return 0;
}

// -----------------------------------
// Application-Level Services e.g. Register, Login
// -----------------------------------

/**
 * Writes user credentials from message into buffers
 * 
 * @param msg Request message that we want to parse credentials from e.g., registration, logins, etc.
 * @param username Buffer to store the parsed username in.
 * @param password Buffer to store the parsed password in.
 * @return 0 on success, otherwise response code on failure.
 */
int parse_credentials(message_t *msg, char *username, char *password) {
  uint8_t* buf_ptr = (uint8_t *)msg->payload;
  uint8_t* end_ptr = buf_ptr + msg->payload_length;
  int has_username = 0;
  int has_password = 0;

  while (buf_ptr < end_ptr) {
    if (buf_ptr + 2 > end_ptr) {
      // Check if we have at least tag (1byte) + length (1byte).
      fprintf(stderr, "Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }
    uint8_t tag = *buf_ptr++;
    uint8_t length = *buf_ptr++;
    if (buf_ptr + length > end_ptr) {
      // Check to see if we have the full value
      fprintf(stderr, "Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }
    
    switch(tag) {
      case TAG_USERNAME:
        if (has_username) {
          fprintf(stderr, "Duplicate username field\n");
          return RESP_ERROR_MALFORMED;
        }
        if (length < 1 || length > MAX_USERNAME_SIZE) {
          fprintf(stderr, "Username length must be 1-%d characters\n", MAX_USERNAME_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        memcpy(username, buf_ptr, length); 
        username[length] = '\0';
        buf_ptr += length;
        has_username = 1;
        break;
      case TAG_PASSWORD:
        if (has_password) {
          fprintf(stderr, "Duplicate password field\n");
          return RESP_ERROR_MALFORMED;
        }
        if (length < 1 || length > MAX_PASSWORD_SIZE) {
          fprintf(stderr, "Password length must be 1-%d characters\n", MAX_PASSWORD_SIZE);
          return RESP_ERROR_MALFORMED;
        }
        memcpy(password, buf_ptr, length);
        password[length] = '\0';
        buf_ptr += length;
        has_password = 1;
        break;
      default:
        // Unknown tag - skip it (forward compatibility?)
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, length);
        buf_ptr += length;
        break;
    }
  }

  // Validate that both fields were sent
  if (!has_username || !has_password) {
    fprintf(stderr, "Missing required fields (username: %d, password %d)\n", has_username, has_password);
    return RESP_ERROR_MALFORMED; 
  }

  return 0;
}

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
  char username[MAX_USERNAME_SIZE+1] = {0};
  char password[MAX_PASSWORD_SIZE+1] = {0};
  int result = parse_credentials(msg, username, password);
  if (result != 0) {
    return result;
  }

  user_t user = {0};
  result = get_user_by_username(username, &user);
  if (result == 0) {
    fprintf(stderr, "Username '%s' already taken!\n", username);
    return RESP_ERROR_USER_EXISTS;
  }
  
  // Else, username is unique, so insert a user
  user_t new_user = {0};
  new_user.id = 0; 
  strncpy(new_user.username, username, MAX_USERNAME_SIZE);
  new_user.username[MAX_USERNAME_SIZE] = '\0'; // Ensure null-termination
  strncpy(new_user.password, password, MAX_PASSWORD_SIZE);
  new_user.password[MAX_PASSWORD_SIZE] = '\0';

  if (insert_user(&user) != 0) {
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
  char username[MAX_USERNAME_SIZE+1] = {0};
  char password[MAX_PASSWORD_SIZE+1] = {0};
  int result = parse_credentials(msg, username, password);
  if (result != 0) {
    return result;
  }

  // Create connection structure; dynamic memory for conn_t has been
  // allocated in connection table; here allocate memory for user_t
  user_t *user = (user_t *)malloc(sizeof(user_t));
  if (user == NULL) {
    fprintf(stderr, "malloc failed: %s\n", strerror(errno));
    return RESP_ERROR_INTERNAL;
  }

  // TODO: Should probably update get_user_by_username to 
  // differentiate between expected errors and user not found
  if (get_user_by_username(username, user) != 0) {
    free(user);
    fprintf(stderr, "User '%s' not found\n", username);
    return RESP_ERROR_USER_NOT_FOUND;
  }

  if (strcmp(user->password, password) != 0) {
    free(user);
    fprintf(stderr, "Invalid password for user '%s'\n", username);  
    return RESP_ERROR_INVALID_CREDENTIALS;
  }

  update_conn_with_user(connfd, user);
  return RESP_OK;
}

/**
 * Lists all desired users in the application.
 * @param connfd Descriptor associated with a connection socket.
 * @param msg Pointer to the request message.
 * @param extra_data Empty buffer that this function can use to add data to the response payload.
 * @param extra_data_len Amount of data (in bytes) that's copied into extra_data buffer, by this function.
 * @return 0 on success, otherwise -1.
 */
int list_users(int connfd, message_t *msg, uint8_t *extra_data, uint32_t *extra_data_len) {
  uint8_t* buf_ptr = (uint8_t *)msg->payload;
  uint8_t* end_ptr = buf_ptr + msg->payload_length;
  int option = -1;     // Either TAG_WORLD, TAG_USER_ID, or TAG_ROOM_ID. If 

  uint32_t user_id;

  while (buf_ptr < end_ptr) { 
    if (buf_ptr + 2 > end_ptr) {
      // Check if we have at least tag (1byte) + length (1byte).
      fprintf(stderr, "Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }

    tlv_tag_t tag = *buf_ptr++;
    uint8_t num_bytes = *buf_ptr++;
    if (buf_ptr + num_bytes > end_ptr) {
      // Check to see if we have the full value from the TLV
      fprintf(stderr, "Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }

    switch (tag) {
      case TAG_WORLD:
        // NOTE: TAG_WORLD will be of zero length, so no value.
        option = tag;
        break;
      case TAG_USER_ID:
        option = tag;

        // NOTE: Typically the user_id will be represented in 1 byte, but when we do I/O multiplexing this 
        // will grow to 2 and eventually 4 bytes to represent the user ID.
        memcpy(&user_id, buf_ptr, num_bytes);
        user_id = ntohl(user_id);
        break;
      default:
        // Unknown tag - skip it (forward compatibility?)
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
        break;
    }
    buf_ptr += num_bytes;
  }

  
  // Return all authenticated users to the client
  // TODO: You'll definitely run out of space for big lists of users
  if (option == TAG_WORLD) {
    uint8_t **buf_double_ptr = &extra_data;
    for (int i = 0; i < num_conns; i++) {
      if (conn_table[i].is_authenticated == 1) {
        user_t *user = conn_table[i].user;
        write_tlv(buf_double_ptr, TAG_USER_ID, sizeof(user->id), &user->id, 1);
        write_tlv(buf_double_ptr, TAG_USERNAME, strlen(user->username), user->username, 0);
        write_tlv(buf_double_ptr, TAG_PASSWORD, strlen(user->password), user->password, 0);
      }
    }
    return RESP_OK;
  } else if (option == TAG_USER_ID) {
    user_t found_user = {0};
    int result = get_user_by_userID(user_id, &found_user);
    if (result == 0) {
      return RESP_ERROR_USER_NOT_FOUND;
    }

    // Write user TLV into the extra_data buffer
    uint8_t **buf_double_ptr = &extra_data;
    write_tlv(buf_double_ptr, TAG_USER_ID, sizeof(found_user.id), &found_user.id, 1);
    write_tlv(buf_double_ptr, TAG_USERNAME, strlen(found_user.username), found_user.username, 0);
    write_tlv(buf_double_ptr, TAG_PASSWORD, strlen(found_user.password), found_user.password, 0);
    return RESP_OK;
  } else {
    fprintf(stderr, "list_users failed: No context to list users from e.g., world, group, or a user ID\n");
    return RESP_ERROR_MALFORMED;
  }
}

/**
 * Handles sending a message to remote peers.
 * @param connfd Descriptor associated with a connection socket.
 * @param msg Pointer to the request message.
 * @param extra_data Empty buffer that this function can use to add data to the response payload.
 * @param extra_data_len Amount of data (in bytes) that's copied into extra_data buffer, by this function.
 * @return 0 on success, otherwise -1.
 */
int send_message(int connfd, message_t *msg, uint8_t *extra_data, uint32_t *extra_data_len) {
  uint8_t* buf_ptr = (uint8_t *)msg->payload;
  uint8_t* end_ptr = buf_ptr + msg->payload_length;
  
  int option = -1; // TAG_WORLD, TAG_USER_ID, or TAG_ROOM_ID
  uint32_t user_id;
  uint8_t msg_content[MAX_MSG_CONTENT_SIZE];
  uint8_t has_content = 0;
  
  while (buf_ptr < end_ptr) {
    if (buf_ptr + 2 > end_ptr) {
      // Check if we have at least tag (1byte) + length (1byte).
      fprintf(stderr, "Malformed TLV: incomplete header\n");
      return RESP_ERROR_MALFORMED;
    }
    uint8_t tag = *buf_ptr++;
    uint8_t num_bytes = *buf_ptr++;
    if (buf_ptr + num_bytes > end_ptr) {
      // Check to see if we have the full value from the TLV
      fprintf(stderr, "Malformed TLV: incomplete value!\n");
      return RESP_ERROR_MALFORMED;
    }
    switch (tag) {
      case TAG_WORLD:
        if (option != -1) {
          fprintf(stderr, "Messaging option already set to '%d', but encountered '%d'.\n", option, tag);
          return RESP_ERROR_MALFORMED;
        }
        option = tag;
        break;
      case TAG_USER_ID:
        if (option != -1) {
          fprintf(stderr, "Messaging option already set to '%d', but encountered '%d'.\n", option, tag);
          return RESP_ERROR_MALFORMED;
        }
        option = tag;
        memcpy(&user_id, buf_ptr, num_bytes);
        user_id = ntohl(user_id);
        break;
      case TAG_MESSAGE_CONTENT:
        if (has_content == 1) {
          fprintf(stderr, "Duplicate message content encountered!\n");
          return RESP_ERROR_MALFORMED;
        }
        has_content = 1;
        memcpy(msg_content, buf_ptr, num_bytes);
        break;
      default:
        // Unknown tag - skip it (forward compatibility?)
        fprintf(stderr, "Unknown TLV tag '%d', skipping %d bytes\n", tag, num_bytes);
    }
    buf_ptr += num_bytes;
  }


  // TODO: Complete this implementation
  // if (option == TAG_WORLD) {
  //   // All auth. connections
  // } else if (option == TAG_USER_ID) {
  //   // To a specific user
  // } else {
  //   fprintf(stderr, "No message context set!\n");
  //   return RESP_ERROR_MALFORMED;
  // }

  return RESP_OK;
}

// ------------------------------------------
// Request-Response, read/write network I/O handling
// ------------------------------------------

/**
 * Creates and populates a response message with response code and data.
 * 
 * @param msg Pointer to the message that we're sending as a response
 * @param rc Response code indicating the result of the user's request.
 * @param data_buf A pointer to a stream of TLVs that we have to insert into the payload. 
 * @param buf_len The length of data_buf in bytes.
 * @return 0 on successful creation, -1 otherwise
 * 
 * NOTE: This function has a couple of goals or responsibilities:
 * (1). Byte Order Handling: For multi-byte fields, this function will handle translating 
 * those fields into network-byte-order. 
 * (2). Byte Handling: It'll also be the main stage for constructing a response message, abstracting 
 *   away more complex or lower level ideas from our other business-related functions.
 */
int create_response(message_t *msg, response_code_t rc, uint8_t *data_buf, uint32_t buf_len) {
  
  // ----- Build message header fields -----
  msg->version = 1;
  msg->type = ACK;
  msg->flags = 42; // TODO: Placeholder for now

  // ----- Build message payload -----
  // 1. Write response code TLV
  // 2. Write response message TLV
  const char* response_message = response_messages[rc];
  uint8_t msg_len = (uint8_t)strlen(response_message);
  uint8_t *payload_start = (uint8_t *)msg->payload;
  uint8_t **payload_double_ptr = &payload_start;
  write_tlv(payload_double_ptr, TAG_RESPONSE_CODE, 1, &rc, 0);
  write_tlv(payload_double_ptr, TAG_RESPONSE_MESSAGE, msg_len, response_message, 0);

  // 3. Write response data (stream of TLVs) into our message payload
  uint8_t *data_buf_ptr = data_buf;
  uint8_t *data_buf_end = data_buf_ptr+buf_len;
  while (data_buf_ptr < data_buf_end) {

    // a. Read the tag (1 byte as always)
    // b. Read the length of the TLV value, which is represented by 1 byte.
    // c. Then read the value from the data buffer
    tlv_tag_t tag = *data_buf_ptr;
    data_buf_ptr++; 
    uint8_t length = *data_buf_ptr;
    data_buf_ptr++;
    uint8_t value[length];
    memcpy(value, data_buf, length);
    data_buf_ptr += length;

    // d. write TLV into the payload, represented by buf_double_ptr
    // TODO: It's probably best to have the caller, or whoever created the data_buf functions handle 
    // the endianness of these TLVs, the values in particular.
    write_tlv(payload_double_ptr, tag, length, (const void*)value, 0);
  }

  // 4. After storing the payload update the payload_length
  //  a. Read payload length into a temp buffer and convert the byte-sequence into network-byte-order
  //  b. Write temp buffer into payload length
  uint32_t payload_len = (uint32_t)(*payload_double_ptr - msg->payload);
  uint32_t net_payload_len = htonl(payload_len);
  memcpy(&msg->payload_length, &net_payload_len, sizeof(uint32_t));
  if (payload_len > MSG_MAX_PAYLOAD_SIZE) {
    fprintf(stderr, "create_response error: Creating payload of size '%d', but maximum is '%d'\n", payload_len, MSG_MAX_PAYLOAD_SIZE);
    return -1;
  }
  return 0;
}

/**
 * Fully read one message from a connection socket 
 * 
 * @param connfd File descriptor for the connection socket we're reading from.
 * @param msg Pointer to message_t struct that we're storing the message information in.
 * @return 0 on success, -1 otherwise
 */
int read_one_message(int connfd, message_t* msg) {
  
  // 1. Read message header
  uint8_t header[MSG_HEADER_SIZE];
  int bytes_to_read = MSG_HEADER_SIZE;
  uint8_t *buf_ptr = header;
  while (bytes_to_read > 0) {
    int bytes_read = read(connfd, buf_ptr, bytes_to_read);
    if (bytes_read == 0) {
      fprintf(stderr, "Unexpected EOF when reading msg header!\n");
      return -1;
    }
    if (bytes_read == -1) {
      fprintf(stderr, "read error when reading msg header: %s\n", strerror(errno));
      return -1;
    }
    buf_ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }

  // 2. Parse Header Fields into msg_t
  // a. bytes 0-2 are version, type, and flags
  // b. bytes 3-6 represent the payload length. A sequence of bytes like this
  // needs to be converted into host byte order.
  msg->version = msg->payload[0];
  msg->type = msg->payload[1];
  msg->flags = msg->payload[2];
  uint32_t payload_length;
  memcpy(&payload_length, &header[3], 4); 
  msg->payload_length = ntohl(payload_length);

  // 3. Validate payload size
  if (msg->payload_length > MSG_MAX_PAYLOAD_SIZE) {
    fprintf(stderr, "read_one_message: Payload of size '%d' is bigger than maximum!\n", msg->payload_length);
    return -1;
  }

  // 4. Read the message payload into msg->payload
  bytes_to_read = msg->payload_length;
  buf_ptr = (uint8_t *)msg->payload;
  while (bytes_to_read > 0) {
    int bytes_read = read(connfd, buf_ptr, bytes_to_read);
    if (bytes_read == 0) {
      fprintf(stderr, "Unexpected EOF when reading msg payload!\n");
      return -1;
    }
    if (bytes_read == -1) {
      fprintf(stderr, "read error when reading msg payload: %s\n", strerror(errno));
      return -1;
    }    
    buf_ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }
  return 0;
}

/**
 * Writes one message across the TCP socket 
 * 
 * @param connfd Descriptor for the connection socket 
 * @param response Response message that we want to write to the remote peer.
 * @return Number of bytes sent, otherwise -1 on error
 * 
 * NOTE: 
 * It's probably best for this function to assume that 
 * the fields and payload in the response messaeg to already be 
 * in network byte order.
 */
int write_one_message(int connfd, message_t* response) { 
  uint8_t message_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];

  // Copy fields; for multi-byte fields like payload_length ensure they're in big-endian
  uint8_t *buf_ptr = message_buffer;
  *buf_ptr++ = response->version;
  *buf_ptr++ = response->type;
  uint32_t net_len = htonl(response->payload_length);
  memcpy(buf_ptr, &net_len, sizeof(uint32_t));
  buf_ptr += sizeof(uint32_t);

  // Copy the main payload
  memcpy(buf_ptr, response->payload, response->payload_length);
  buf_ptr += response->payload_length;

  // Send it across socket
  size_t total_bytes = buf_ptr - message_buffer;
  int num_bytes_sent = send(connfd, message_buffer, total_bytes, 0);
  return num_bytes_sent;
}

/**
 * Processes one complete message cycle: read, handle, and respond.
 * 
 * @param connfd Integer representing the connection descriptor
 * @return 0 on success, otherwise -1
 */
int serve_one_message(int connfd) {
  message_t request = {0};
  int read_result = read_one_message(connfd, &request);
  if (read_result == -1) {
    return -1; 
  }

  // These are passed to service functions to get data (a stream of TLVs) from them and the size of it.
  // NOTE: Can't use all of MSG_MAX_PAYLOAD_SIZE. Expect around 30 bytes for response message and response code tlvs
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
    default:
      fprintf(stderr, "Received unknown message type '%d'!\n", request.type);
      result = RESP_ERROR_UNKNOWN_COMMAND;
  }

  message_t response;
  result = create_response(&response, result, extra_data, extra_data_len);
  if (result == -1) {
    fprintf(stderr, "create_response failure!\n");
  }

  // d. Write it over TCP
  int write_result = write_one_message(connfd, &response);
  if (write_result == -1) {
    fprintf(stderr, "write_one_message failed: %s\n", strerror(errno));
  }

  return 0;  
}

// ----------------------------------------------
// Network Connection Stuff
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
  conn_t new_conn = {ip_address, 0, connfd, NULL};
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