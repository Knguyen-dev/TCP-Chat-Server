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
#include <cmath>
#include <algorithm>

// -----------------------------------------------------------------------------
// Test Configuration & Global Tracking
// -----------------------------------------------------------------------------

// a. Target number of connections we're planning to connect
// b. Maximum number of connections that we should be doing for our tests; try not to exceed please
const int TARGET_NUM_CONNECTIONS = 10000;
const int MAX_N = 110000;

// Configuration for Spammer Phases
// a. Number of clients sending world broadcasts
// b. Number of clients sending p2p broadcasts
// c. Target number of broadcast responses to collect
const int NUM_A_WORLD_SPAMMERS = 1;
const int NUM_B_P2P_SPAMMERS = 1;
const int TARGET_R_SAMPLES = 500; 

// Adjustable payload size configuration (Must be <= 255)
const int SENDER_MESSAGE_CONTENT_SIZE = 255; 
static_assert(SENDER_MESSAGE_CONTENT_SIZE <= 255, "SENDER_MESSAGE_CONTENT_SIZE cannot exceed 255 bytes.");

/**
 * Scoped enum representing what "phase" or step in the pipeline
 * the load testing script is in.
 */
enum class TestPhase {
  CONNECTING_ALL,
  REGISTERING_ALL,
  LOGGING_IN_ALL,
  WORLD_SPAM_TEST,
  P2P_SPAM_TEST,
  REPORT_AND_DONE
};

// a. The current phase the load tester is in.
// b. The number of client connections we currently have.
// c. The number of clients that are authenticated.
TestPhase current_phase{TestPhase::CONNECTING_ALL};
int current_num_connections{0};
int num_authenticated_connections{0};

// a. Time that a given phase starts
//    This is used to calculate how long a phase actually takes.
// b. An array of latencies, where latency is the roundtrip time for a request-response 
std::chrono::high_resolution_clock::time_point phase_t0;
std::vector<double> sample_latencies_ms; 

/**
 * Structure used to help track the completion of a world or p2p broadcast
 */
struct TelemetryTracker {
  // a. Time that the broadcast was sent from the client.
  // b. Content of the client's message
  // c. Number of clients that have received the message.
  // d. Number of clients that should receive the message.
  std::chrono::high_resolution_clock::time_point t0;
  std::string message_content;
  int current_count;
  int expected_count;
};

// Maps sender usernames to the telemetry for the latest message that they've sent.
std::unordered_map<std::string, TelemetryTracker> tracker_dict;

/**
 * Enum that tracks the state of a given client. For example a client 
 */
enum class ClientState {
  CONNECTING,
  SEND_REGISTER,
  WAIT_REGISTER_ACK,
  SEND_LOGIN,
  WAIT_LOGIN_ACK,
  IDLE,
  SPAMMER_WORLD,
  SPAMMER_P2P
};

/**
 * Struct that represents a client in our load testing script
 */
struct TestClient {
  ClientState state;
  conn_t conn;
};

// a. Table of clients, index by fd. Meaning the client with fd=i, is located at client_table[i]
// b. Tracks the fd for the very last client that connects to the server
//    This specific client acts as the recipient for P2P target testing.
TestClient client_table[MAX_N];
int last_connected_fd{-1};

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------

/**
 * Gets the username of a client 
 * @note Username, password, etc. are all programmatically generated like this 
 * to make things easier for us
 */
std::string get_client_username(int fd) {
  return "user_" + std::to_string(fd);
}

std::string get_client_password() {
  return "password123";
}

/**
 * The message that a spamming client sends to other users.
 */
std::string get_client_message() {
  return std::string(SENDER_MESSAGE_CONTENT_SIZE, 'A');
}

/**
 * Sets a fd as non-blocking
 * @note This is for efficiency in our non-blocking event loop
 */
static void set_nonblocking_fd(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    LOG_ERROR("fcntl error: %s\n", strerror(errno));
    exit(-1);
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    LOG_ERROR("fcntl error: %s\n", strerror(errno));
    exit(-1);
  }
}

/**
 * Updates the epoll interest of a given client
 * @param epollfd Fd of the epoll instance that the load testing script is using
 * @param connfd Connection fd of the test client.
 * @param events Epoll events that we're setting the client to
 */
static void update_epoll_interest(int epollfd, int connfd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = connfd;
  epoll_ctl(epollfd, EPOLL_CTL_MOD, connfd, &ev);
}

/**
 * Writes bytes from the test client to the server in a non-blocking manner
 * @param c Client that we're writing.
 * @return True when the client's outgoing buffer is empty, meaning in this case 
 * they sent their entire message. Otherwise, we return false to indicate the client 
 * still has a partial message they have to send to the server.
 */
bool send_all_nonblocking(TestClient& c) {
  while (!c.conn.outgoing.empty()) {
    ssize_t rv = write(c.conn.fd, c.conn.outgoing.data(), c.conn.outgoing.size());
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
      c.conn.flags |= ConnFlags::WANT_CLOSE;
      return false;
    }
    c.conn.outgoing.erase(c.conn.outgoing.begin(), c.conn.outgoing.begin() + rv);
  }
  return true; 
}

// -----------------------------------------------------------------------------
// Phase Transition Orchestration
// -----------------------------------------------------------------------------

/**
 * Transitions the load tester to the next phase 
 * @param epollfd Fd of the epoll instance that the load tester is using.
 * @param next_phase The next phase that the load testing script transitioning to.
 * 
 * @note When this function is called, you should assume that all connections that 
 */
void transition_to_phase(int epollfd, TestPhase next_phase) {
  // Current phase has finished, record the time point for phase latency calculation.
  auto phase_t1 = std::chrono::high_resolution_clock::now();
  
  // ##### 1. Calculate and print metrics for the phase we are leaving #####
  // For example, when connecting/registering/authenticating users, we're going to output the number of 
  // clients that have connected/registered/etc, the total latency it took for everyone t ocomplete, or maybe even the average 
  // latency for a given client to do the task in that phase. For world/p2p broadcasting,, we know that we're going to calculate 
  // sample statistics such as the mean, median, stdev of latency for a world or p2p broadcast. That's why we use sample_latencies_ms
  // which is our array of latencies for each broadcast.
  if (current_phase == TestPhase::CONNECTING_ALL) {
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(phase_t1 - phase_t0).count();
    printf("\n=== Phase 1 Complete: Connected %d TCP Sockets in %lld ms ===\n", TARGET_NUM_CONNECTIONS, static_cast<long long>(dur));
  } 
  else if (current_phase == TestPhase::REGISTERING_ALL) {
    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase_t1 - phase_t0).count();
    double avg = static_cast<double>(dur_ms) / TARGET_NUM_CONNECTIONS;
    printf("=== Phase 2 Complete: Registered %d users. Total: %lld ms. Avg: %.4f ms/user ===\n", TARGET_NUM_CONNECTIONS, static_cast<long long>(dur_ms), avg);
  } 
  else if (current_phase == TestPhase::LOGGING_IN_ALL) {
    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase_t1 - phase_t0).count();
    double avg = static_cast<double>(dur_ms) / TARGET_NUM_CONNECTIONS;
    printf("=== Phase 3 Complete: Logged in %d users. Total: %lld ms. Avg: %.4f ms/user ===\n", TARGET_NUM_CONNECTIONS, static_cast<long long>(dur_ms), avg);
  }
  else if (current_phase == TestPhase::WORLD_SPAM_TEST || current_phase == TestPhase::P2P_SPAM_TEST) {
    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase_t1 - phase_t0).count();
    double total_sec = dur_ms / 1000.0;
    
    // Process statistical samples collected
    if (sample_latencies_ms.empty()) {
      printf("No samples collected during broadcast phase.\n");
    } else {
      double sum = 0;
      for (double val : sample_latencies_ms) sum += val;
      double mean = sum / sample_latencies_ms.size();

      std::sort(sample_latencies_ms.begin(), sample_latencies_ms.end());
      double median = sample_latencies_ms[sample_latencies_ms.size() / 2];

      double variance_sum = 0;
      for (double val : sample_latencies_ms) variance_sum += std::pow(val - mean, 2);
      double stdev = std::sqrt(variance_sum / std::max(1UL, sample_latencies_ms.size() - 1));

      double throughput = sample_latencies_ms.size() / (total_sec > 0 ? total_sec : 1.0);

      printf("=== %s Complete ===\n", (current_phase == TestPhase::WORLD_SPAM_TEST) ? "Phase 4 (World Broadcast)" : "Phase 5 (P2P Broadcast)");
      printf("  Samples Collected : %zu\n", sample_latencies_ms.size());
      printf("  Throughput        : %.2f msgs/sec\n", throughput);
      printf("  Mean Latency      : %.2f ms\n", mean);
      printf("  Median Latency    : %.2f ms\n", median);
      printf("  Max (p100) Latency: %.2f ms\n", sample_latencies_ms.back());
      printf("  Std Deviation     : %.2f ms\n", stdev);
    }
    sample_latencies_ms.clear();
    tracker_dict.clear();
  }

  // ##### 2. Transition into the next phase of the load test #####
  // Remember that t0 is a global variable, time point, that tracks the moment of a beginning of a new phase.
  // This is what's happening here, as we transition into a new phase, track the moment that it starts.
  // In the below for loop + many conditionals, we're going to update the internal state of our client connections
  // accordingly. For example, if we're in the REGISTERING_ALL phase, we have to update the client's so that 
  // they're prepared to send registration requests (rather than receiving them). If it's LOGGING_IN_ALL, we're 
  // preparing the clients to login.
  // NOTE: It's when these clients finish thos, login requests, or registration requests, that we'll need to update
  // them to wait for a register ACK or a login ACK. That's probably done in the handle_write_client function.
  // So the main idea:
  // 1. Phase transition prepares clients to send messages
  // 2. handle_write makes them write those messages and prepare for recieving ACKS
  // 3. handle_read and try_one_server_response reading responses, and putting clients in an idle state. 
  //    Clients stay in that idle state until a quota is reached, then try_one_server_response will transition state.
  current_phase = next_phase;
  phase_t0 = std::chrono::high_resolution_clock::now();
  if (current_phase == TestPhase::REPORT_AND_DONE) {
    printf("\nAll benchmark phases completed successfully!\n");
    exit(0);
  }

  int target_spammers = 0;
  for (int fd = 0; fd < MAX_N; fd++) {
    TestClient& client = client_table[fd];


    // If not connected, reject it
    // NOTE: This prevents us from iterating over connections that 
    // aren't even connected to the server
    if (!has_flag(client.conn.flags, ConnFlags::IS_ACTIVE)) {
      continue;
    }

    // For Example 1: if registering phase or logging in:
    // a. Update client state
    // b. Update application read and write state
    // c. Update epoll interest accordingly
    if (current_phase == TestPhase::REGISTERING_ALL) {
      client.state = ClientState::SEND_REGISTER;
      client.conn.flags |= ConnFlags::WANT_WRITE;
      client.conn.flags &= ~ConnFlags::WANT_READ;
      update_epoll_interest(epollfd, fd, EPOLLOUT);
    } else if (current_phase == TestPhase::LOGGING_IN_ALL) {
      client.state = ClientState::SEND_LOGIN;
      client.conn.flags |= ConnFlags::WANT_WRITE;
      client.conn.flags &= ~ConnFlags::WANT_READ;
      update_epoll_interest(epollfd, fd, EPOLLOUT);
    } else if (current_phase == TestPhase::WORLD_SPAM_TEST) {
    // For Example 2: When world spam OR p2p spam testing  
    // a. If we don't have enough spammers, add more
    // b. Otherwise, the rest of the connections are IDLE (listening for messages)
    // Same logic for when p2p test spamming. When a broadcast is sent, it's sent to other appropriate
    // connections and the server itself, so the spammers need to be writing and reading
      if (target_spammers < NUM_A_WORLD_SPAMMERS) {
        client.state = ClientState::SPAMMER_WORLD;
        client.conn.flags |= ConnFlags::WANT_WRITE;
        client.conn.flags |= ConnFlags::WANT_READ;
        update_epoll_interest(epollfd, fd, EPOLLIN | EPOLLOUT);
        target_spammers++;
      } else {
        client.state = ClientState::IDLE;
        client.conn.flags &= ~ConnFlags::WANT_WRITE;
        client.conn.flags |= ConnFlags::WANT_READ;
        update_epoll_interest(epollfd, fd, EPOLLIN);
      }
    } else if (current_phase == TestPhase::P2P_SPAM_TEST) {
      if (target_spammers < NUM_B_P2P_SPAMMERS) {
        client.state = ClientState::SPAMMER_P2P;
        client.conn.flags |= ConnFlags::WANT_WRITE;
        client.conn.flags |= ConnFlags::WANT_READ;
        update_epoll_interest(epollfd, fd, EPOLLIN | EPOLLOUT);
        target_spammers++;
      } else {
        client.state = ClientState::IDLE;
        client.conn.flags &= ~ConnFlags::WANT_WRITE;
        client.conn.flags |= ConnFlags::WANT_READ;
        update_epoll_interest(epollfd, fd, EPOLLIN);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Core Network Functions
// -----------------------------------------------------------------------------

/**
 * Connects a single test client to the server
 * @param epollfd Fd of the epoll instance that that the load tester is using
 * @return 0 on success, otherwise -1 on error.
 * 
 * @note Technically even if it returns 0, it doesn't mean 
 * that the client has connected yet has we're doing non-blocking clients
 */
int connect_test_client(int epollfd) {
  // a. Create non-blocking client fd 
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  set_nonblocking_fd(fd);

  // b. Fill out IP and PORT of the test server that the 
  // client is connecting to. Then attempt to connect to the server.
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(TEST_SERVER_PORT);
  inet_pton(AF_INET, TEST_SERVER_IP, &server_addr.sin_addr);

  int conn_result = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result == -1 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  
  // c. Update the client fd, state, application I/O interest, and epoll interest.
  // Also we'll write the last_connected_fd as this connection could possibley be the 
  // last client fd that connects.
  TestClient& client{client_table[fd]};
  client.conn.fd = fd;
  client.state = ClientState::CONNECTING;
  client.conn.flags = ConnFlags::WANT_WRITE;
  last_connected_fd = fd;
  struct epoll_event ev;
  ev.events = EPOLLOUT;
  ev.data.fd = fd;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    close(fd);
    return -1;
  }
  return fd;
}

/**
 * Attempts to parse and serve response message from the server.
 * @param client Test client that receiving/parsisng the server response.
 * @param epollfd Fd of the epoll instance that the load tester is using.
 * @return True if the server response was successfully processed, otherwise false. 
 * 
 * @note This function is responsible for calling the `transition_to_phase` function as 
 * it contains all the application-level logic for transitioning between phases in our 
 * load testing script.
 */
bool try_server_response(TestClient& client, int epollfd) {

  // 1. Only parse when we have the message header + 1 byte for the payload
  if (client.conn.incoming.size() < MSG_HEADER_SIZE + 1) {
    return false;
  }
  message_t response;
  parse_message(client.conn.incoming, response);

  // 2. Only continue if we have a full message of data.
  if (MSG_HEADER_SIZE + response.payload_length > client.conn.incoming.size()) {
    return false;
  }
  
  // 3. Process the type of server response
  switch(response.type) {
    case REGISTER:
    // If the server response is to a user registration request
    // a. If the response was successful AND the client is waiting for a register response
    //    1. Park the client state using the global phase shift triggers the login phase 
    //    2. Update client to be reading, i mean that is the idle state.
    //    3. Update the number of users that have been registered
    //    4. Potentially transition into the login phase
    // b. Otherwise close the connection
      if (response.rc == RESP_OK && client.state == ClientState::WAIT_REGISTER_ACK) {
        
        // NOTE: Think it's fine to not mess with the interest list or connection flags 
        // and simply just put the client in the idle state. 
        client.state = ClientState::IDLE;
        static int registered_count = 0;
        if (++registered_count == TARGET_NUM_CONNECTIONS) {
          transition_to_phase(epollfd, TestPhase::LOGGING_IN_ALL);
        }
      } else {
        client.conn.flags |= ConnFlags::WANT_CLOSE;
      }
      break;

    case LOGIN:
    // If processing a login server response
    // Update the state, application I/O readiness, and epoll interest.
    // We'll also update the number of authenticated connections 
    // which potentially causes us to transition into the WORLD SPAM test phase.
      if (response.rc == RESP_OK && client.state == ClientState::WAIT_LOGIN_ACK) {
        client.state = ClientState::IDLE; // Park until global phase shift triggers spam runs
        num_authenticated_connections++;
        client.conn.flags |= ConnFlags::IS_AUTH;
        if (num_authenticated_connections == TARGET_NUM_CONNECTIONS) {
          transition_to_phase(epollfd, TestPhase::WORLD_SPAM_TEST);
        }
      } else {
        client.conn.flags |= ConnFlags::WANT_CLOSE;
      }
      break;

    case CHAT: {
      // If we're processing a broadcast response/notification
      // then we could either be testing world or P2P broadcast spamming
      // a. Record the time we received the broadcast
      // b. Parse the broadcast to get the sender username and message content 
      auto t1 = std::chrono::high_resolution_clock::now();
      int broadcast_type = peek_broadcast_type(response);
      std::string sender = "";
      std::string content = "";
      if (broadcast_type == TAG_WORLD_BROADCAST) {
        world_broadcast_t broadcast{};
        parse_world_broadcast_notification(response, broadcast);
        sender = std::move(broadcast.sender_username);
        content = std::move(broadcast.message_content);
      } else if (broadcast_type == TAG_P2P_BROADCAST) {
        p2p_broadcast_t broadcast{};
        parse_p2p_broadcast_notification(response, broadcast);
        sender = std::move(broadcast.sender_username);
        content = std::move(broadcast.message_content);
      }

      // If the message contents don't match, this means the message we just received
      // is different from the most recent message that the sender just sent, so ignore it.
      TelemetryTracker& message_state = tracker_dict[sender];
      if (message_state.message_content != content) {
        break; 
      }

      // a. Increment the number of clients that received the message
      // b. If all recipients received the message, we can now calculate latencies
      //    1. Add broadcast latency into vector of sample latencies.
      //    2. If we received R number of broadcasts, transition to the P2P tests (if we're in world testing)
      //    3. Otherwise if we're already in p2p testing, transition to the report and done phase.
      message_state.current_count++;
      if (message_state.current_count == message_state.expected_count) {
        auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - message_state.t0);
        double latency_ms = latency_us.count() / 1000.0;
        sample_latencies_ms.push_back(latency_ms);
        
        // Evaluate Phase advancement based on target samples gathered
        if (sample_latencies_ms.size() >= static_cast<size_t>(TARGET_R_SAMPLES)) {
          if (current_phase == TestPhase::WORLD_SPAM_TEST) {
            transition_to_phase(epollfd, TestPhase::P2P_SPAM_TEST);
          } else if (current_phase == TestPhase::P2P_SPAM_TEST) {
            transition_to_phase(epollfd, TestPhase::REPORT_AND_DONE);
          }
        }
      }
      break;
    }
    default:
      break;
  }

  client.conn.incoming.erase(client.conn.incoming.begin(), client.conn.incoming.begin() + MSG_HEADER_SIZE + response.payload_length);
  return true; 
}

/**
 * Performs a non-blocking read on the client connection
 * @param c Test Client that's reading data from the server.
 * @param epollfd Fd of the epoll instance that the load testing script is using. 
 */
void handle_client_read(TestClient& c, int epollfd) {
  uint8_t buffer[16 * 1024];
  while (true) {
    ssize_t rv = read(c.conn.fd, buffer, sizeof(buffer));
    if (rv < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;

      c.conn.flags |= ConnFlags::WANT_CLOSE;
      return;
    } else if (rv == 0) {
      c.conn.flags |= ConnFlags::WANT_CLOSE;
      return;
    }
    buf_append(c.conn.incoming, buffer, rv);
  }
  while (try_server_response(c, epollfd)) {}
}

/**
 * Handles the client sending a request message to the server
 */
void handle_client_write(TestClient& c, int epollfd) {
  // If client is in the connecting phase, check whether it actually succeeded or not
  if (c.state == ClientState::CONNECTING) {

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(c.conn.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        // Connection actually failed!
        c.conn.flags |= ConnFlags::WANT_CLOSE;
        return;
    }

    // Otherwise, TCP connection completed in the background!
    c.state = ClientState::IDLE;
    c.conn.flags |= ConnFlags::IS_ACTIVE;
    return;
  }

  // If the client is sending a REGISTRATION or LOGIN request
  // Then we'll create the message request and copy the serialized bytes 
  // into the user's outgoing buffer
  if (c.state == ClientState::SEND_REGISTER) {
    registration_credentials_t credentials;
    credentials.username = get_client_username(c.conn.fd);
    credentials.password = get_client_password();
    uint32_t message_len{};
    uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
    build_register_request(request_buffer, credentials, message_len);
    buf_append(c.conn.outgoing, request_buffer, message_len);
  } 
  else if (c.state == ClientState::SEND_LOGIN) {
    login_credentials_t credentials;
    credentials.username = get_client_username(c.conn.fd);
    credentials.password = get_client_password();
    uint32_t message_len{};
    uint8_t request_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];
    build_login_request(request_buffer, credentials, message_len);
    buf_append(c.conn.outgoing, request_buffer, message_len);
  } 
  else if (c.state == ClientState::SPAMMER_WORLD) {

    // If the client is a spammer for world or P2P broadcasts
    // 1. Create/get message telemetry object for a given user
    // 2. Early return, this forces us to wait until the user's current message has been received by 
    //    everyone before sending another message.
    // 3. Create broadcast and add it to the client's outgoing buffer.
    std::string username = get_client_username(c.conn.fd);
    TelemetryTracker& message_state = tracker_dict[username];
    if (message_state.current_count < message_state.expected_count) return; 

    world_broadcast_t broadcast{};
    broadcast.sender_username = username;
    broadcast.message_content = get_client_message();
    uint8_t request_buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
    uint32_t message_len{};
    build_world_broadcast(request_buffer, broadcast, message_len);
    buf_append(c.conn.outgoing, request_buffer, message_len);
  }
  else if (c.state == ClientState::SPAMMER_P2P) {
    std::string username = get_client_username(c.conn.fd);
    TelemetryTracker& message_state = tracker_dict[username];

    if (message_state.current_count < message_state.expected_count) return;

    p2p_broadcast_t p2p{};
    p2p.sender_username = username;
    p2p.recipient_username = get_client_username(last_connected_fd); // Exploit target worst case connection
    p2p.message_content = get_client_message();
    uint8_t request_buffer[MSG_HEADER_SIZE + MSG_MAX_PAYLOAD_SIZE];
    uint32_t message_len{};
    build_p2p_broadcast(request_buffer, p2p, message_len); // Assumed wrapper function exists mapping packet definitions
    buf_append(c.conn.outgoing, request_buffer, message_len);
  }

  // If we finished writing the current message, we may transition 
  // a. If finished sending a registration request.
  //  - Wait for the registration response and put connection in read state
  // b. If finished sending login request
  //  - Wait for login response and put connection in a reading state
  // c. If it's a spammer connection, It should have both read and write flags on
  //  - Access the message state and fill it with data
  if (send_all_nonblocking(c)) {
    switch (c.state) {
      case ClientState::SEND_REGISTER:
        c.state = ClientState::WAIT_REGISTER_ACK;
        c.conn.flags |= ConnFlags::WANT_READ;
        c.conn.flags &= ~ConnFlags::WANT_WRITE;
        update_epoll_interest(epollfd, c.conn.fd, EPOLLIN);
        break;
      case ClientState::SEND_LOGIN:
        c.state = ClientState::WAIT_LOGIN_ACK;
        c.conn.flags |= ConnFlags::WANT_READ;
        c.conn.flags &= ~ConnFlags::WANT_WRITE;
        update_epoll_interest(epollfd, c.conn.fd, EPOLLIN);
        break;
      case ClientState::SPAMMER_WORLD: {
        TelemetryTracker& message_state = tracker_dict[get_client_username(c.conn.fd)];
        message_state.t0 = std::chrono::high_resolution_clock::now();
        message_state.current_count = 0;
        message_state.expected_count = num_authenticated_connections; 
        message_state.message_content = get_client_message();
        break;
      }
      case ClientState::SPAMMER_P2P: {
        TelemetryTracker& message_state = tracker_dict[get_client_username(c.conn.fd)];
        message_state.t0 = std::chrono::high_resolution_clock::now();
        message_state.current_count = 0;
        message_state.expected_count = 2; 
        message_state.message_content = get_client_message();
        break;
      }
      default:
        break;
    }
  }
}

/**
 * Closes client connection
 * @param epollfd The fd of the epoll instance
 * @param connfd The fd of the client connection
 */
void close_client(int epollfd, int connfd) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, nullptr);
  close(connfd);
  TestClient& client{client_table[connfd]};
  client.conn.incoming.clear();
  client.conn.outgoing.clear();
  client.conn.flags = ConnFlags::NONE;
}

// -----------------------------------------------------------------------------
// Main Runtime Loop
// -----------------------------------------------------------------------------
int main() {
  // Create epoll instance and start phase 1
  // a. Create epoll instance
  // b. Record time-point for phase 2.
  // c. For lloop connects TARGET_NUM_CONNECTIONS to the server 
  const int MAX_EVENTS{64};
  struct epoll_event events[MAX_EVENTS];
  int epollfd = epoll_create1(0);
  if (epollfd == -1) return -1;
  printf("Starting Phase 1: Allocating and Connecting Sockets...\n");
  phase_t0 = std::chrono::high_resolution_clock::now();

  // Non-blocking mass-injection for Phase 1
  for (int i = 0; i < TARGET_NUM_CONNECTIONS; i++) {

    // TODO: Technically not 
    if (connect_test_client(epollfd) >= 0) {
      current_num_connections++;
    }
  }
 
  while (true) {
    int num_ready_fds = epoll_wait(epollfd, events, MAX_EVENTS, 1); 
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
      if ((events[i].events & EPOLLOUT) && !has_flag(client.conn.flags, ConnFlags::WANT_CLOSE)) {
        handle_client_write(client, epollfd);
      }
      if (has_flag(client.conn.flags, ConnFlags::WANT_CLOSE) || (events[i].events & (EPOLLHUP | EPOLLERR))) {
        current_num_connections--;
        if (has_flag(client.conn.flags, ConnFlags::IS_AUTH)) {
          num_authenticated_connections--;
        }
        close_client(epollfd, fd);
      }
    }

    // Evaluate Phase 1 completion explicitly inside the event loop
    if (current_phase == TestPhase::CONNECTING_ALL) {
      int actively_connected = 0;
      for (int fd = 0; fd < MAX_N; fd++) {
        if (client_table[fd].conn.fd > 0 && client_table[fd].state == ClientState::IDLE) {
          actively_connected++;
        }
      }
      if (actively_connected == TARGET_NUM_CONNECTIONS) {
        transition_to_phase(epollfd, TestPhase::REGISTERING_ALL);
      }
    }
  }
  return 0;
}