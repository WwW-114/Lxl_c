#pragma once

#include <sys/types.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>

namespace co {
  bool setNonBlocking(int fd);

  int listenTcp(uint16_t port, int backlog = 128);

  int connectTcp(const char *ip, uint16_t port);

  int connect(int fd, const struct sockaddr *addr, socklen_t len);
  int accept(int listen_fd, struct sockaddr *addr, socklen_t *addr_len);

  ssize_t read(int fd, void *buf, size_t size);
  ssize_t write(int fd, const void *buf, size_t size);

  ssize_t readn(int fd, void *buf, size_t size);
  ssize_t writen(int fd, const void *buf, size_t size);
}
