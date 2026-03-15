#include "shared.h"

/**
 * TODO: Signal handler that shuts down the application gracefully.
 */
int handle_shutdown(int SIGINT) {}

/**
 * Opens a listening socket at the given port
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

  if (listen(listenfd, LISTENQ) < 0) {
    fprintf(stderr, "open_listenfd listen error: Error opening the listening socket!\n");
    close(listenfd);
    return -1;
  }

  return listenfd;
}

int main(int argc, char** argv) {
  int listenfd, *connfdp;
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  pthread_t tid;
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(0);
  }

  // 1. Open a listening socket
  // TODO: Should print out the IP:port that we're listening on though. I plan to 
  // also have other machines connect to this server during and after development.
  int listenfd = open_listenfd(argv[1]);
  if (listenfd < 0) {
    fprintf(stderr, "open_listenfd() error: Terminating program!\n");
    return 0;
  }

  init_user_count();
  fprintf(stdout, "TCP server started listening on port '%s'!\n", argv[1]);

  // TODO: May want to initialize or open the file at the start here?
  // 2. Infinite Server Loop
  while (1) {

    // Attempt to create a connection socket from the queue of incoming connections.
    // NOTE: Reset clientlen and malloc. This avoids memory and data corruption from the previous loop.
    clientlen = sizeof(struct sockaddr_storage);
    connfdp = malloc(sizeof(int));
    if (connfdp == NULL) {
      fprintf(stderr, "malloc() error, shutting down: %s", strerror(errno));
      return 0;
    }
    *connfdp = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
    if (*connfdp < 0) {
      fprintf(stderr, "accept() error: %s", strerror(errno));
      continue;
    }

    // Create a peer thread to handle the connection.
    // NOTE: As we try to scale we may see EAGAIN when we hit resource limits. Probably good to stop the program at that 
    // point, but log out stuff.
    int thread_result = pthread_create(&tid, NULL, &serve_connection, (void *)connfdp);
    if (thread_result != 0) {
      fprintf(stderr, "pthread_create() error, shutting down: %s", strerror(thread_result));
      return 0;
    }
  }
}