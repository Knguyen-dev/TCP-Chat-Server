#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "shared.h"
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define BUFFER_SIZE 1024


int connect_to_server() {
  int clientfd = socket(AF_INET, SOCK_STREAM, 0);
  if (clientfd < 0) {
    perror("socket() failed");
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(SERVER_PORT);

  if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr)) {
    perror("Invalid address, couldn't copy it into address struct!\n");
    close(clientfd);
    return -1;
  }

  int conn_result = connect(clientfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (conn_result < 0) {
    perror("Connection failed!\n");
    close(clientfd);
    return -1;
  }

  printf("Client is connected to server at %s:%d\n", SERVER_IP, SERVER_PORT);
  return clientfd;
}

void test_registration() {
  printf("Test: Registration\n");
  int fd = connect_to_server();
  if (fd < 0) {
    printf("Registration FAILED: Couldn't connect!\n");
    return;
  }


  
  message_t msg = {0};
  msg.version = 1;
  msg.type = REGISTER;
  msg.flags = 0;

  // Send the message over the buffer



}

void test_login() {

}