#include "test_utils.hpp"
#include <poll.h>

// ##### World Broadcast Tests Below #####

/**
 * Test that the world broadcast is rejected when the client isn't authenticated.
 */
int test_world_broadcast_unauthenticated() {
    int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(sock != -1);

    std::string sender_username = "sender_" + random_string(6);
    std::string message_content = "Hello, world!";
    ASSERT_TRUE(send_world_broadcast_request(sock, sender_username, message_content) != -1);

    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(sock, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_INVALID_CREDENTIALS);
    close(sock);
    return 0;
}

/**
 * Tests that the world broadcast is rejected if the message content exceeds 
 * the maximum allowed length.
 */
int test_world_broadcast_message_too_long() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);

    // Message too long
    std::string too_long(MAX_MSG_CONTENT_SIZE + 1, 'a');
    ASSERT_TRUE(send_world_broadcast_request(fd, user.username, too_long) != -1);

    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(fd);
    return 0;
}

/**
 * Tests that the world broadcast is rejected if the message 
 * content field is an empty string.
 */
int test_world_broadcast_empty_message() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);

    ASSERT_TRUE(send_world_broadcast_request(fd, user.username, std::string()) != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(fd);
    return 0;
}

/**
 * Tests that the world broadcast is rejected if the message content field is 
 * missing from the request.
 */
int test_world_broadcast_missing_message() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);

    ASSERT_TRUE(send_malformed_world_broadcast(fd, true, false, false) != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(fd);
    return 0;
}

/**
 * Tests that the world broadcast is rejected if the sender username field is missing.
 */
int test_world_broadcast_missing_sender_username() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);
    ASSERT_TRUE(send_malformed_world_broadcast(fd, false, true, false) != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(fd);
    return 0;
}

/**
 * Tests that the server rejects a request when the sender 
 * username doesn't match the username of the authenticated client
 * making the request.
 */
int test_world_broadcast_sender_username_incorrect() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);

    std::string bad_sender = user.username + "_wrong";
    ASSERT_TRUE(send_world_broadcast_request(fd, bad_sender, "Hello!") != -1);

    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(fd);
    return 0;
}

/**
 * Tests that the server rejects a p2p broadcast 
 * when the sender username doesn't match the username
 * of the authenticated client making the request
 */
int test_p2p_broadcast_sender_username_incorrect() {
    user_t sender;
    user_t recipient;
    int fd_sender = register_and_login(sender);
    int fd_recipient = register_and_login(recipient);
    ASSERT_TRUE(fd_sender != -1);
    ASSERT_TRUE(fd_recipient != -1);

    std::string bad_sender = sender.username + "_wrong";
    ASSERT_TRUE(send_p2p_broadcast_request(fd_sender, bad_sender, recipient.username, "Hello") != -1);

    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd_sender, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);

    struct pollfd pfd{fd_recipient, POLLIN, 0};
    int pr = poll(&pfd, 1, 1000);
    ASSERT_TRUE(pr == 0);

    close(fd_sender);
    close(fd_recipient);
    return 0;
}


/**
 * Tests that a valid world broadcast request results in 
 * all authenticated clients receiving the broadcast whilst 
 * unauthenticated clients don't receive the broadcast.
 */
int test_world_broadcast_successful() {
    user_t user_A, user_B;
    int fd_a = register_and_login(user_A);
    int fd_b = register_and_login(user_B);
    int fd_c = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(fd_a != -1);
    ASSERT_TRUE(fd_b != -1);
    ASSERT_TRUE(fd_c != -1);

    std::string msg = "Hello everyone!";
    ASSERT_TRUE(send_world_broadcast_request(fd_a, user_A.username, msg) != -1);

    // Sender receives a CHAT response (ACK)
    message_t resp_a{};
    uint8_t buf_a[MSG_MAX_PAYLOAD_SIZE];
    resp_a.payload = buf_a;
    ASSERT_TRUE(read_server_response(fd_a, resp_a) == 0);
    ASSERT_TRUE(resp_a.type == CHAT);
    ASSERT_TRUE(resp_a.rc == RESP_OK);

    // Other authenticated client should receive the broadcast notification
    message_t notif_b{};
    uint8_t buf_b[MSG_MAX_PAYLOAD_SIZE];
    notif_b.payload = buf_b;
    ASSERT_TRUE(read_server_response(fd_b, notif_b) == 0);
    ASSERT_TRUE(notif_b.type == CHAT);
    ASSERT_TRUE(notif_b.rc == RESP_OK);
    world_broadcast_t parsed{};
    parse_world_broadcast_notification(notif_b, parsed);
    ASSERT_TRUE(parsed.sender_username == user_A.username);
    ASSERT_TRUE(parsed.message_content == msg);

    // Unauthenticated client should not receive any data within 1s
    struct pollfd pfd{fd_c, POLLIN, 0};
    int pr = poll(&pfd, 1, 1000);
    ASSERT_TRUE(pr == 0);

    close(fd_a);
    close(fd_b);
    close(fd_c);
    return 0;
}

// ##### P2P Broadcast Tests Below #####

/**
 * Tests that the p2p broadcast is rejected when the client isn't authenticated.
 */
int test_p2p_broadcast_unauthenticated() {
    int sock = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(sock != -1);
    ASSERT_TRUE(send_p2p_broadcast_request(sock, "anon", "someone", "hey") != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(sock, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_INVALID_CREDENTIALS);
    close(sock);
    return 0;
}

/**
 * Tests that the p2p broadcast is rejected if the recipient username isn't 
 * associated with a currently authenticated client.
 */
int test_p2p_broadcast_recipient_does_not_exist() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);
    ASSERT_TRUE(send_p2p_broadcast_request(fd, user.username, "no_such_user", "hello") != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_USER_NOT_FOUND);
    close(fd);
    return 0;
}

/**
 * Tests that the server rejects a p2p broadcast when the sender tries 
 * to send the broadcast to themselves (i.e. the sender and recipient usernames match).
 */
int test_p2p_broadcast_sender_username_is_recipient_username() {
    user_t user;
    int fd = register_and_login(user);
    ASSERT_TRUE(fd != -1);
    ASSERT_TRUE(send_p2p_broadcast_request(fd, user.username, user.username, "hello") != -1);
    message_t response{};
    uint8_t payload_buffer[MSG_MAX_PAYLOAD_SIZE];
    response.payload = payload_buffer;
    ASSERT_TRUE(read_server_response(fd, response) == 0);
    ASSERT_TRUE(response.type == CHAT);
    ASSERT_TRUE(response.rc == RESP_ERROR_MALFORMED);
    close(fd);
    return 0;
}

/**
 * Test that the behavior for a successful p2p broadcast is correct, where 
 * the server should forward the broadcast to the recipient, and only the recipient, 
 * and the sender should receive an ACK response from the server.
 */
int test_p2p_broadcast_successful() {
    user_t user_A, user_B, user_C;
    int fd_a = register_and_login(user_A);
    int fd_b = register_and_login(user_B);
    int fd_c = register_and_login(user_C);
    int fd_d = connect_to_server(TEST_SERVER_IP, TEST_SERVER_PORT);
    ASSERT_TRUE(fd_a != -1);
    ASSERT_TRUE(fd_b != -1);
    ASSERT_TRUE(fd_c != -1);
    ASSERT_TRUE(fd_d != -1);

    std::string msg = "private hello";
    ASSERT_TRUE(send_p2p_broadcast_request(fd_a, user_A.username, user_B.username, msg) != -1);

    // Sender gets ACK
    message_t ack{}; uint8_t buf_ack[MSG_MAX_PAYLOAD_SIZE]; ack.payload = buf_ack;
    ASSERT_TRUE(read_server_response(fd_a, ack) == 0);
    ASSERT_TRUE(ack.type == CHAT && ack.rc == RESP_OK);

    // Recipient receives notification
    message_t notif{}; uint8_t buf_notif[MSG_MAX_PAYLOAD_SIZE]; notif.payload = buf_notif;
    ASSERT_TRUE(read_server_response(fd_b, notif) == 0);
    ASSERT_TRUE(notif.type == CHAT && notif.rc == RESP_OK);
    p2p_broadcast_t parsed; parse_p2p_broadcast_notification(notif, parsed);
    ASSERT_TRUE(parsed.sender_username == user_A.username);
    ASSERT_TRUE(parsed.recipient_username == user_B.username);
    ASSERT_TRUE(parsed.message_content == msg);

    // Other authenticated client doesn't receive anything
    struct pollfd pfd{fd_c, POLLIN, 0};
    int pr = poll(&pfd, 1, 1000);
    ASSERT_TRUE(pr == 0);

    // Other unauthenticated client doesn't receive anything
    pfd.fd = fd_d;
    pr = poll(&pfd, 1, 1000);
    ASSERT_TRUE(pr == 0);

    close(fd_a); 
    close(fd_b);
    close(fd_c);
    close(fd_d);
    return 0;
}

int main() {
  RUN_TEST(test_world_broadcast_unauthenticated); // works
  RUN_TEST(test_world_broadcast_message_too_long); // works
  RUN_TEST(test_world_broadcast_empty_message); // works
  RUN_TEST(test_world_broadcast_missing_message); // works
  RUN_TEST(test_world_broadcast_missing_sender_username); // works
  RUN_TEST(test_world_broadcast_sender_username_incorrect); // works
  RUN_TEST(test_world_broadcast_successful); // works

  RUN_TEST(test_p2p_broadcast_unauthenticated); // works
  RUN_TEST(test_p2p_broadcast_recipient_does_not_exist); // works
  RUN_TEST(test_p2p_broadcast_sender_username_incorrect); // works
  RUN_TEST(test_p2p_broadcast_sender_username_is_recipient_username); // works
  RUN_TEST(test_p2p_broadcast_successful); // works
  std::cout << "All broadcast integration tests passed.\n";
  return 0;
}