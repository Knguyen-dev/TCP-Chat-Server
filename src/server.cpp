#include "db.hpp"
#include "logger.hpp"
#include "server_utils.hpp"
#include "shared.hpp"
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

int main(int argc, char **argv) {
  int listenfd, epollfd;
  struct epoll_event ev, events[MAX_EVENTS];
  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  if (argc != 4) {
    LOG_ERROR("usage: %s <port> <is_test> <enable_logging>\n", argv[0]);
    return -1;
  }
  if (init_logger(atoi(argv[3])) != 0) {
    LOG_ERROR("init_logger() error: Terminating program!\n");
    return -1;
  }

  // Step 1: Create listenfd, epollfd, and register listenfd
  listenfd = init_server(argv[1], atoi(argv[2]));
  if (listenfd < 0) {
    LOG_ERROR("init_server() error: Terminating program!\n");
    return -1;
  }
  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    LOG_ERROR("epoll_create1() error: %s!\n", strerror(errno));
    return -1;
  }
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = listenfd;
  int rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);
  if (rc == -1) {
    LOG_ERROR("epoll_ctl() error with listenfd: %s!\n", strerror(errno));
    return -1;
  }

  // Only register stdin in non-test mode since background test server has no
  // controlling terminal
  if (!atoi(argv[2])) {
    set_nonblocking_fd(STDIN_FILENO);
    ev.data.fd = STDIN_FILENO;
    rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev);
    if (rc == -1) {
      LOG_ERROR("epoll_ctl() error with stdin_fileno: %s!\n", strerror(errno));
      return -1;
    }
  }

  while (true) {
    // Step 2: Poll the kernel for I/O readiness; block indefinitely
    int num_ready_fds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (num_ready_fds == -1) {
      LOG_ERROR("epoll_wait() error: %s!\n", strerror(errno));
      return -1;
    }

    // Step 3: Iterate through all ready file descriptors
    for (int i = 0; i < num_ready_fds; i++) {
      // Step 3a: Process the fd for the TCP listening socket
      if (events[i].data.fd == listenfd) {
        if (events[i].events & EPOLLERR) {
          LOG_ERROR("Listening descriptor had an issue!\n");
          return -1;
        }
        accept_all_connections(listenfd, epollfd);
        continue;
      } else if (events[i].data.fd == STDIN_FILENO) {
        if (events[i].events & EPOLLERR) {
          LOG_ERROR("Stdin descriptor had an issue!\n");
          return -1;
        }
        int rc_stdin = handle_server_input();
        if (rc_stdin != 0) {
          LOG_WARN("handle_server_input returned %d\n", rc_stdin);
        }
        continue;
      }

      // Step 3b: Process the fds for the TCP connection sockets
      // NOTE: After reading, the connection may be in write state as well.
      // Though, it hasn't been given the "ok" by the epoll to write, so it
      // could block immediately, or we could have a lucky break to be able to
      // write immediately. NOTE 2: current_flags needs to be a pointer or
      // reference as handle_read_connection might update the flags and make it
      // writeable all within one iteration of the for loop.
      ConnFlags &current_flags = conn_manager.flags[events[i].data.fd];
      int current_fd = events[i].data.fd;
      if (has_flag(current_flags, ConnFlags::WANT_READ)) {
        handle_read_connection(current_fd, epollfd);
      }
      if (has_flag(current_flags, ConnFlags::WANT_WRITE)) {
        handle_write_connection(current_fd, epollfd);
      }
      if ((events[i].events & EPOLLERR) ||
          has_flag(current_flags, ConnFlags::WANT_CLOSE)) {
        remove_connection(current_fd, epollfd);
      }
    }
  }
}