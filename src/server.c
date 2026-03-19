#include "shared.h"


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
  listenfd = open_listenfd(argv[1]);
  if (listenfd < 0) {
    fprintf(stderr, "open_listenfd() error: Terminating program!\n");
    return 0;
  }

  // 2. Infinite Server Loop
  while (1) {
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