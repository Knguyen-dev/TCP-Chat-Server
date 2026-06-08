#include "shared.hpp"
#include "logger.hpp"
#include "../integration/test_utils.hpp"
#include <sys/epoll.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unordered_map>
#include <chrono>


bool has_spammer{false};

struct TelemetryTracker {
  std::chrono::high_resolution_clock::time_point t0;
  std::string message_content;
  int current_count;
  int expected_count;
};

std::unordered_map<std::string, TelemetryTracker> tracker_dict;

enum class ClientState {
  CONNECTING,
  SEND_REGISTER,
  WAIT_REGISTER_ACK,
  SEND_LOGIN,
  WAIT_LOGIN_ACK,
  IDLE,
  SPAMMER
};

struct TestClient {
  ClientState state;
  conn_t conn;
};


int num_authenticated_connections{0};

const int MAX_N{110000}; 
TestClient client_table[MAX_N];

/**
 * @note In our automated test suite, we're not going to use heap memory.
 * Meaning the conn::user_t pointer will be a nullpointer.
 */
std::string get_client_username(TestClient client) {
  return "user_" + std::to_string(client.conn.fd);
}

std::string get_client_password() {
  return "password123";
}

std::string get_client_message() {
  return "Hello World!";
}

/**
 * Sets a socket for a fd as nonblocking.
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
 * Returns true if all data was sent, otherwise false.
 */
bool send_all_nonblocking(TestClient& c) {
  while (c.conn.outgoing.size() > 0) {
    ssize_t rv = write(c.conn.fd, c.conn.outgoing.data(), c.conn.outgoing.size());
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }
      LOG_ERROR("write() error on fd %d: %s\n", c.conn.fd, strerror(errno));
      c.conn.want_close = true;
      return false;
    }
    c.conn.outgoing.erase(c.conn.outgoing.begin(), c.conn.outgoing.begin() + rv);
  }
  return true; 
}

/**
 * Returns fd on successful connection, otherwise -1.
 */
int connect_test_client(int epollfd) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  set_nonblocking_fd(fd);

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(TEST_SERVER_PORT);
  inet_pton(AF_INET, TEST_SERVER_IP, &server_addr.sin_addr);

  int conn_result = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result == -1 && errno != EINPROGRESS) {
    fprintf(stderr, "Failed to connect to server: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  TestClient& client{client_table[fd]};
  client.conn.fd = fd;
  client.state = ClientState::CONNECTING;
  client.conn.want_read = false;
  client.conn.want_write = true;

  // Crucial: Monitor for EPOLLOUT first to know when the TCP connection finishes
  struct epoll_event ev;
  ev.events = EPOLLOUT;
  // ev.events = EPOLLOUT | EPOLLET;
  ev.data.fd = fd;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    close(fd);
    return -1;
  }
  return fd;
}

/**
 * Modfiies the epoll interest entry for a given client connection socket.
 */
static void update_epoll_interest(int epollfd, TestClient& c, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  // ev.events = events | EPOLLET;
  ev.data.fd = c.conn.fd;
  epoll_ctl(epollfd, EPOLL_CTL_MOD, c.conn.fd, &ev);
}

/**
 * Attempts to read a response from the server. This handles 
 * application layer state updates like TestClient::state, read/write desire,
 * and epoll interest list.
 */
bool try_server_response(TestClient& client, int epollfd) {

  // Do we have the header plus 1 byte of the payload yet?
  // NOTE: Allows for our parse_message to parse the header and get a pointer to payload
  if (client.conn.incoming.size() < MSG_HEADER_SIZE + 1) {
    return false;
  }

  message_t response;
  parse_message(client.conn.incoming, response);

  // Only proceed when we have a full message worth of data
  if (MSG_HEADER_SIZE + response.payload_length > client.conn.incoming.size()) {
    return false;
  }
  
  switch(response.type) {
    case REGISTER:
      // If registration worked:
      // a. Transition into login state.
      // b. Transition to writing since we're going to write login bytes
      // c. Update epoll interest to watch for write readiness.
      // Otherwise, registration failed, just close the connection so we don't waste time.
      if (response.rc == RESP_OK && client.state == ClientState::WAIT_REGISTER_ACK) {
        client.state = ClientState::SEND_LOGIN;
        client.conn.want_write = true;
        client.conn.want_read = false;
        update_epoll_interest(epollfd, client, EPOLLOUT);
        LOG_INFO("User registered fd=%d\n", client.conn.fd);
      } else {
        LOG_ERROR("Registration failed for fd %d\n", client.conn.fd);
        client.conn.want_close = true;
      }
      break;
    case LOGIN:
      if (response.rc != RESP_OK || client.state != ClientState::WAIT_LOGIN_ACK) {
        LOG_ERROR("Login failed for fd %d\n", client.conn.fd);
        client.conn.want_close = true;
        break;
      }      
      if (has_spammer) {
        client.state = ClientState::IDLE;
        client.conn.want_write = false;
        client.conn.want_read = true;
        uint32_t events = EPOLLIN;
        update_epoll_interest(epollfd, client, events);
      } else {
        client.state = ClientState::SPAMMER;
        client.conn.want_write = true;
        client.conn.want_read = true;
        uint32_t events = EPOLLIN | EPOLLOUT;
        update_epoll_interest(epollfd, client, events);
        LOG_INFO("User Login fd=%d\n", client.conn.fd);
      }
      num_authenticated_connections++;
      break;
    case CHAT: {
      auto t1 = std::chrono::high_resolution_clock::now();
      int broadcast_type = peek_broadcast_type(response);
      std::string sender = "";
      std::string content = "";
      switch (broadcast_type) {
        case TAG_WORLD_BROADCAST: {
          world_broadcast_t broadcast{};
          parse_world_broadcast_notification(response, broadcast);
          sender = std::move(broadcast.sender_username);
          content = std::move(broadcast.message_content);
          break;
        }
        case TAG_P2P_BROADCAST: {
          p2p_broadcast_t broadcast{};
          parse_p2p_broadcast_notification(response, broadcast);
          sender = std::move(broadcast.sender_username);
          content = std::move(broadcast.message_content);
          break;
        }
        default:
          break;
      }

      TelemetryTracker& message_state = tracker_dict[sender];
      // If we're receiving a response that's not associated with the most 
      // recent CHAT message that the sender has sent.
      if (message_state.message_content != content) {
        break;
      }

      message_state.current_count++;
      if (message_state.current_count == message_state.expected_count) {
        auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - message_state.t0);
        printf("Broadcast took: %lld us\n", static_cast<long long>(latency_us.count()));
        tracker_dict.erase(sender);
      }
    }
    default:
      break;
  }

  // Remove the processed message bytes from incoming buffer
  client.conn.incoming.erase(client.conn.incoming.begin(), client.conn.incoming.begin() + MSG_HEADER_SIZE + response.payload_length);
  return true; 
}

/**
 * Client reads bytes from the server and attempts to process a message(s)
 */
void handle_client_read(TestClient& c, int epollfd) {
  uint8_t buffer[16 * 1024];
  while (true) {
    ssize_t rv = read(c.conn.fd, buffer, sizeof(buffer));
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      c.conn.want_close = true;
      return;
    } else if (rv == 0) {
      c.conn.want_close = true;
      return;
    }
    buf_append(c.conn.incoming, buffer, rv);
  }

  // No pipelining to make it easier to follow in debug
  while (try_server_response(c, epollfd)) {}
}

/**
 * Handles the writing of requests to the server.
 */
void handle_client_write(TestClient& c, int epollfd) {
  
  // TCP Handshake completed! Ready to register.
  if (c.state == ClientState::CONNECTING) {
    c.state = ClientState::SEND_REGISTER;
  }

  // 1. Add data to buffers depending on what state the connection is in
  if (c.state == ClientState::SEND_REGISTER) {
    registration_credentials_t credentials;
    credentials.username = get_client_username(c);
    credentials.password = get_client_password();
    uint32_t message_len{};
    uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
    if (build_register_request(request_buffer, credentials, message_len) == -1) {
      LOG_ERROR("[System] Failed to build registration request!\n");
      c.conn.want_close = true;
      return;
    } 
    buf_append(c.conn.outgoing, request_buffer, message_len);
  } 
  else if (c.state == ClientState::SEND_LOGIN) {
    login_credentials_t credentials;
    credentials.username = get_client_username(c);
    credentials.password = get_client_password();
    uint32_t message_len{};
    uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
    if (build_login_request(request_buffer, credentials, message_len) == -1) {
      LOG_ERROR("[System] Failed to login request!\n");
      c.conn.want_close = true;
      return;
    }
    buf_append(c.conn.outgoing, request_buffer, message_len);
  } else if (c.state == ClientState::SPAMMER) {
    
    // Access the state for tracking a sender's messaging; creates one if none exists
    std::string username{get_client_username(c)};
    TelemetryTracker& message_state = tracker_dict[get_client_username(c)];

    // If the previous message hasn't been processed yet, wait.
    if (message_state.current_count < message_state.expected_count) {
      return;
    }

    world_broadcast_t broadcast{};
    broadcast.sender_username = username;
    broadcast.message_content = get_client_message();
    uint8_t request_buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
    uint32_t message_len{};
    if (build_world_broadcast(request_buffer, broadcast, message_len) == -1) {
      LOG_ERROR("[System] Client Failure: Failed to build world broadcast!\n");
      return;
    }
    buf_append(c.conn.outgoing, request_buffer, message_len);
  }

  // If we finished writing the current message, we may transition 
  // a. If finished sending a registration request.
  //  - Wait for the registration response and put connection in read state
  // b. If finished sending login request
  //  - Wait for login response and put connection in a reading state
  // c. If it's a spammer connection, It should have both read and write flags on
  //  - Access the message state and fill it since we're doing a world broadcast.
  if (send_all_nonblocking(c)) {
    switch (c.state) {
      case ClientState::SEND_REGISTER:
        c.state = ClientState::WAIT_REGISTER_ACK;
        c.conn.want_write = false;
        c.conn.want_read = true;
        update_epoll_interest(epollfd, c, EPOLLIN);
        break;
      case ClientState::SEND_LOGIN:
        c.state = ClientState::WAIT_LOGIN_ACK;
        c.conn.want_write = false;
        c.conn.want_read = true;
        update_epoll_interest(epollfd, c, EPOLLIN);
        break;
      case ClientState::SPAMMER: {
        // The spammer finished writing its world broadcast
        TelemetryTracker& message_state = tracker_dict[get_client_username(c)];
        message_state.current_count = 0;
        message_state.expected_count = num_authenticated_connections;
        message_state.t0 = std::chrono::high_resolution_clock::now();
        message_state.message_content = get_client_message();
        break;
      }
      // Other states we don't care about
      case ClientState::CONNECTING:
      case ClientState::WAIT_LOGIN_ACK:
      case ClientState::WAIT_REGISTER_ACK:
      case ClientState::IDLE:
        break;
      default:
        break;
    }
  }
}

/**
 * Closes a test client connection
 */
void close_client(int epollfd, TestClient& c) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, c.conn.fd, nullptr);
  close(c.conn.fd);
  c.conn.incoming.clear();
  c.conn.outgoing.clear();
  c.state = ClientState::CONNECTING; // Reset slots
}

int main() {
  const int MAX_EVENTS{64};
  struct epoll_event events[MAX_EVENTS];
  int epollfd = epoll_create1(0);
  if (epollfd == -1) return -1;


  int current_num_connections = 0;
  int TARGET_NUM_CONNECTIONS = 10000;
  int BATCH_SIZE = 150;
  int EPOLL_DELAY_MS = 10; // paces our program

  // Seed initial batch of connection requests
  for (int i = 0; i < TARGET_NUM_CONNECTIONS; i++) {
    if (connect_test_client(epollfd) >= 0) current_num_connections++;
  }
  
  while (true) {
    int num_ready_fds = epoll_wait(epollfd, events, MAX_EVENTS, EPOLL_DELAY_MS); 
    if (num_ready_fds == -1) {
      if (errno == EINTR) continue;
      return -1;
    }

    for (int i = 0; i < num_ready_fds; i++) {
      int fd = events[i].data.fd;
      TestClient& client = client_table[fd];
      if (events[i].events & EPOLLIN) {
        handle_client_read(client, epollfd);
      }
      if ((events[i].events & EPOLLOUT) && !client.conn.want_close) {
        handle_client_write(client, epollfd);
      }
      if (client.conn.want_close || (events[i].events & (EPOLLHUP | EPOLLERR))) {
        close_client(epollfd, client);
        current_num_connections--;
        // Decrement number of authenticated connections if needed.
        if (client.state == ClientState::IDLE || client.state == ClientState::SPAMMER) {
          num_authenticated_connections--;
        }
      }
    }
  
    // Gradually add more connections to meet the target number
    if (current_num_connections < TARGET_NUM_CONNECTIONS) {
      int remaining_num_connections = TARGET_NUM_CONNECTIONS - current_num_connections;
      int num_to_inject = std::min(remaining_num_connections, BATCH_SIZE);
      for (int i{0}; i < num_to_inject; i++) {
        if (connect_test_client(epollfd) >= 0) {
          current_num_connections++;
        }
      }
      printf("Current Number of Connection: %d\n", current_num_connections);
    }
  }
}