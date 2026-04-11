#include "shared.h"
#include "client_utils.h"
#include <signal.h>

int clientfd = -1;

// TODO: Please handle memory errors here
void sigint_handler(int sigint) {
  printf("\nGracefully shutting down...\n");
  if (clientfd != -1) {
    close(clientfd);
  }
  exit(0);
}



int main(void) {
  char input_ip[16] = "0.0.0.0";
  short port = 8080;
  user_t user = {0}; 
  signal(SIGINT, sigint_handler);

  clientfd = create_client_connection(input_ip, port);
  if (clientfd == -1) {
    fprintf(stderr, "create_client_connection failed!\n");
    return -1;
  }
  printf("Successfully connected to '%s:%d'!\n", input_ip, port);
  

  int continue_loop = 1;
  while (continue_loop) {
    int choice = get_valid_input_range("1. Create Account\n2. Continue with existing account\n3. Quit\nEnter your choice: ", 1, 3);
    while (getchar() != '\n');
    switch (choice) {
      case 1:
        handle_client_registration(clientfd);
        break;
      case 2:
        if (handle_client_login(clientfd, &user) == 0) {
          continue_loop = 0;
        }
        break;
      case 3:
        continue_loop = 0;
        break;
      default:
        fprintf(stderr, "Out of range input for menu in main: %d\n", choice);
        break;
    }
  }

  // At this point they've been authenticated
  // Start the messaging loop
  run_messaging_loop(clientfd, &user);
  
  return 0;
}