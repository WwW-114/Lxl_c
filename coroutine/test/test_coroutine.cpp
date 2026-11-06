#include <co/Fiber.h>
#include <co/Scheduler.h>
#include <co/Net.h>

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {
  bool g_switch_ok = false;
  bool g_echo_ok = false;
  bool g_server_ok = false;

  void testFiberSwitch() {
    std::printf("=== test 1: fiber switch ===\n");

    int counter = 0;
    co::Scheduler sched;
    for(int i = 0; i < 3; ++i) {
      sched.spawn([&counter, i] {
                    for(int step = 0; step < 3; ++step) {
                      ++counter;
                      std::printf("  fiber %d, step %d\n", i, step);
                      co::yield();
                    }
                  });
    }

    sched.run();

    g_switch_ok = (counter == 9);
    std::printf("counter = %d (expected 9): %s\n\n", counter,
                g_switch_ok ? "OK" : "FAILED");
  }

  void testEcho() {
    std::printf("=== test 2: coroutine echo ===\n");

    uint16_t port = 0;
    co::Scheduler sched;

    sched.spawn([&] {
                  const int listen_fd = co::listenTcp(0);
                  if(listen_fd < 0) {
                    std::perror("listenTcp");
                    sched.stop();
                    return;
                  }

                  sockaddr_in addr{};
                  socklen_t len = static_cast<socklen_t>(sizeof(addr));
                  if(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
                    std::perror("getsockname");
                    ::close(listen_fd);
                    sched.stop();
                    return;
                  }

                  port = ntohs(addr.sin_port);
                  std::printf("  server listening on 127.0.0.1:%u\n",
                    static_cast<unsigned>(port));

                  const int conn_fd = co::accept(listen_fd, nullptr, nullptr);
                  if(conn_fd < 0) {
                    std::perror("co::accept");
                    ::close(listen_fd);
                    sched.stop();
                    return;
                  }

                  std::printf("  server accepted a connection\n");

                  char buf[256];
                  ssize_t n = 0;
                  while((n = co::read(conn_fd, buf, sizeof(buf))) > 0) {
                    if(co::writen(conn_fd, buf, static_cast<size_t>(n)) < 0) {
                      std::perror("co::writen");
                      break;
                    }
                  }

                  ::close(listen_fd);
                  ::close(conn_fd);
                  g_server_ok = (n == 0);
                  std::printf("  server finished (n = %zd)\n", n);
                });

    sched.spawn([&] {
                    if(port == 0) {
                      std::printf("  client: server port is not ready\n");
                      sched.stop();
                      return;
                    }

                    const int fd = co::connectTcp("127.0.0.1", port);
                    if(fd < 0) {
                      std::perror("connectTcp");
                      sched.stop();
                      return;
                    }

                    const char* msg = "hello, coroutine!";
                    const size_t msg_len = std::strlen(msg);
                    if(co::writen(fd, msg, msg_len) < 0) {
                      std::perror("co::writen");
                      ::close(fd);
                      sched.stop();
                      return;
                    }

                    char buf[256] = {0};
                    const ssize_t n = co::readn(fd, buf, msg_len);
                    g_echo_ok = (n == static_cast<ssize_t>(msg_len) &&
                                 std::memcmp(buf, msg, msg_len) == 0);
                    std::printf("  client received %zd bytes: %s\n", n, buf);
                    ::close(fd);
                  });

    sched.run();
    std::printf("server ok: %s, echo ok: %s\n\n",
                g_server_ok ? "YES" : "NO", g_echo_ok ? "YES" : "NO");
  }    
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  testFiberSwitch();
  testEcho();

  bool ok = g_switch_ok && g_echo_ok && g_server_ok;
  std::printf("=== %s ===\n", ok ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
  return ok ? 0 : 1; 
}
