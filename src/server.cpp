#include "shared.hpp"
#include "server_utils.hpp"
#include "db.hpp"
#include <signal.h>
#include <sys/epoll.h>


int listenfd = -1;

void sigint_handler(int sigint) {
  printf("\nGracefully shuting down TCP chat server...\n");
  if (listenfd != -1) {
    close(listenfd);
  }
  close_db();
  exit(0);
}

int main(int argc, char** argv) {
  int listenfd, epollfd;
  struct epoll_event ev, events[MAX_EVENTS];
  signal(SIGINT, sigint_handler);
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return -1;
  }

  listenfd = init_server(argv[1]);
  if (listenfd < 0) {
    fprintf(stderr, "init_server() error: Terminating program!\n");
    return -1;
  }

  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    fprintf(stderr, "epoll_create1() error: %s!\n", strerror(errno));
    return -1;
  }

  // Step 1: Register Listening descriptor; listen for input, error, and make it edge triggered
  ev.events = EPOLLIN | EPOLLERR | EPOLLET;
  ev.data.fd = listenfd;
  int rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);
  if (rc == -1) {
    fprintf(stderr, "epoll_ctl() error: %s!\n", strerror(errno));
    return -1;
  }
  

  while (true) {
    // Step 2: Poll the kernel for I/O readiness; block indefinitely
    int num_ready_fds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (num_ready_fds == -1) {
      fprintf(stderr, "epoll_wait() error: %s!\n", strerror(errno));
      return -1;
    }

    // Step 3: Iterate through all ready file descriptors
    for (int i = 0; i < num_ready_fds; i++) {
      // Step 3a: Process the fd for the TCP listening socket
      if (events[i].data.fd == listenfd) {
        if (events[i].events & EPOLLERR) {
          fprintf(stderr, "Listening descriptor had an issue!\n");
          return -1;   
        }
        accept_all_connections(listenfd, epollfd, ev);
      } else {
        // Step 3b: Process the fds for the TCP connection sockets
        // NOTE: Connection can either be currently reading or writing, not both.
        conn_t* conn = conn_table[ev.data.fd];
        if (conn->want_read) {
          handle_read_connection(*conn);
        } else if (conn->want_write) {
          handle_write_connection(*conn);
        }
        // If we had errors polling data OR the app wants to close this connection
        if ((ev.events & EPOLLERR) || conn->want_close) {
          remove_connection(conn->fd, epollfd);
        }
      }
    }   
  }
}