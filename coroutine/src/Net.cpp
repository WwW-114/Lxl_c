#include <co/Net.h>
#include <co/Scheduler.h>

#include <unistd.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <cerrno>

namespace co {
  namespace {
    int closeKeepErrno(int fd) {
      const int saved = errno;
      ::close(fd);
      errno = saved;
      return -1;
    }

    Scheduler* requireScheduler() {
      Scheduler *sched = Scheduler::current();
      if(sched == nullptr) {
        errno = EPERM;
        return nullptr;
      }

      return sched;
    }
  }

  bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if(flags < 0) {
      return false;
    }

    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  int listenTcp(uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
      return -1;
    }

    int yes = 1;
    if(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
      return closeKeepErrno(fd);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      return closeKeepErrno(fd);
    }

    if(::listen(fd, backlog) < 0) {
      return closeKeepErrno(fd);
    }

    if(!setNonBlocking(fd)) {
      return closeKeepErrno(fd);
    }

    return fd;
  }

  int connectTcp(const char *ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
      return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
      ::close(fd);
      errno = EINVAL;
      return -1;
    }

    if(!setNonBlocking(fd)) {
      return closeKeepErrno(fd);
    }
    
    if(co::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      return closeKeepErrno(fd);
    }

    return fd;
  }

  int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if(::connect(fd, addr, len) == 0) {
      return 0;
    }

    if(errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
      return -1;
    }

    Scheduler *sched = requireScheduler();
    if(sched == nullptr || !sched->waitFd(fd, EPOLLOUT)) {
      return -1;
    }

    int err = 1;
    socklen_t length = sizeof(err);
    if(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &length) < 0) {
      return -1;
    }

    if(err != 0) {
      errno = err;
      return -1;
    }

    return 0;
  }

  int accept(int listen_fd, struct sockaddr *addr, socklen_t *addr_len) {
    for(; ;) {
      const int fd = ::accept(listen_fd, addr, addr_len);
      if(fd >= 0) {
        if(!setNonBlocking(fd)) {
          return closeKeepErrno(fd);
        }

        return fd;
      }

      if(errno == EINTR) {
        continue;
      }

      if(errno != EWOULDBLOCK && errno != EAGAIN) {
        return -1;
      }

      Scheduler *sched = requireScheduler();
      if(sched == nullptr || !sched->waitFd(listen_fd, EPOLLIN)) {
        return -1;
      }
      
    }
  }

  ssize_t read(int fd, void *buf, size_t size) {
    for(; ;) {
      const ssize_t n = ::read(fd, buf, size);
      if(n >= 0) {
        return n;
      }

      if(errno == EINTR) {
        continue;
      }

      if(errno != EWOULDBLOCK && errno != EAGAIN) {
        return -1;
      }

      Scheduler *sched = requireScheduler();
      if(sched == nullptr || !sched->waitFd(fd, EPOLLIN)) {
        return -1;
      }
    }
  }

  ssize_t write(int fd, const void *buf, size_t size) {
    for(; ;) {
      const ssize_t n = ::write(fd, buf, size);

      if(n >= 0) {
        return n;
      }

      if(errno == EINTR) {
        continue;
      }

      if(errno != EWOULDBLOCK && errno != EAGAIN) {
        return -1;
      }

      Scheduler *sched = requireScheduler();
      if(sched == nullptr || !sched->waitFd(fd, EPOLLOUT)) {
        return -1;
      }
    }
  }

  ssize_t readn(int fd, void *buf, size_t size) {
    char *p = static_cast<char*>(buf);
    size_t done = 0;

    while(done < size) {
      const ssize_t n = read(fd, p + done, size - done);

      if(n == 0) {
        break;
      }

      if(n < 0) {
        return -1;
      }

      done += static_cast<size_t>(n);
    }

    return static_cast<ssize_t>(done);
  }

  ssize_t writen(int fd, const void *buf, size_t size) {
    const char *p = static_cast<const char*>(buf);
    size_t done = 0;

    while(done < size) {
      const ssize_t n = write(fd, p + done, size - done);

      if(n == 0) {
        errno = EIO;
        return -1;
      }

      if(n < 0) {
        return -1;
      }

      done += static_cast<size_t>(n);
    }

    return static_cast<ssize_t>(done);
  }
}
