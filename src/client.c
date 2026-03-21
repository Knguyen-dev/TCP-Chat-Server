#include "shared.h"
#include "client_utils.h"


int main(void) {
  char input_username[MAX_USERNAME_SIZE+1];
  char input_password[MAX_PASSWORD_SIZE+1];
  char input_ip[16];
  short port = 8080;

  int choice = 0;

  printf("Enter the IP address of the chat server: ");
  scanf("%s", input_ip);
  printf("Enter the port number: ");
  scanf("%d", &port);

  int clientfd = create_client_connection(input_ip, port);
  if (clientfd == -1) {
    fprintf(stderr, "create_client_connection failed!\n");
    return -1;
  }
  printf("Successfully connected to '%s:%d'!\n", input_ip, port);
  

  int continue_loop = 1;
  while (continue_loop) {

    char *prompt = "1. Create Account\n2. Contine with existing account\nEnter your choice: ";
    int choice = get_valid_input_range(prompt, 1, 2);

    switch (choice) {

      case 1:
        
    }

  }
  
  
  


  




  return 0;
}