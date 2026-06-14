#include "client_utils.hpp"
#include "logger.hpp"
#include "shared.hpp"

int main(void) {
  char input_ip[16]{"0.0.0.0"};
  short port{8080};
  conn_t conn{};
  if (setup_client_signal_handlers(&conn) == -1) {
    return 1;
  }

  conn.fd = create_client_connection(input_ip, port, conn);
  if (conn.fd == -1) {
    LOG_ERROR("create_client_connection failed\n");
    return 1;
  }
  LOG_INFO("[System] Connected to %s:%d\n", input_ip, port);

  run_messaging_loop(conn);
  return 0;
}