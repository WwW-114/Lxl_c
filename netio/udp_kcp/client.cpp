#include "common.h"
#include "ikcp.h"

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <csignal>
#include <iostream>
#include <string>


static int g_sock = -1;
static ikcpcb* g_kcp = nullptr;
static volatile std::sig_atomic_t g_running = 1;

static uint32_t g_lastRecv = 0;
static uint32_t g_lastPing = 0;

static void onSignal(int) { g_running = 0; }

// 把 "127.0.0.1" 或 "localhost" 解析成 IPv4 地址
static bool resolve(const char *host, uint16_t port, sockaddr_in &out) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo *res = nullptr;
  if(getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) return false;

  out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
  out.sin_port = htons(port);
  freeaddrinfo(res);
  return true;
}

static int kcpOutput(const char *buf, int len, ikcpcb* /*kcp*/, void* /*user*/) {
  const ssize_t n = send(g_sock, buf, static_cast<size_t>(len), 0);
  return (n == len) ? 0 : -1;
}

static void display(const std::string &msg) {
  if(msg.empty()) return;

  MsgType type = msgType(msg);
  const std::string body = msgBody(msg);
  if(body.empty()) return;

  switch (type) {
    case MSG_BROADCAST:
      {
        printf("%s\n", body.c_str());
        break;
      }

    case MSG_SYSTEM:
      {
        printf("\033[33m%s\033[0m\n", body.c_str());
        break;
      }

    case MSG_PONG:
      break;
    
    default:
      break;
  }

  fflush(stdout);
}

static void drainMessages() {
  for(; ;) {
    const int size = ikcp_peeksize(g_kcp);

    if(size < 0) break;

    if(size > MAX_MSG_LEN) {
      ikcp_recv(g_kcp, nullptr, size);
      continue;
    }

    std::string msg(static_cast<size_t>(size), '\0');
    const int r = ikcp_recv(g_kcp, msg.data(), size);
    if(r < 0) break;
    msg.resize(static_cast<size_t>(r));

    display(msg); 
  }
}

static void onSocketReadable() {
  for(; ;) {
    uint8_t buf[2048];
    const ssize_t n = ::recv(g_sock, buf, sizeof(buf), MSG_DONTWAIT);

    if(n > 0) {
      g_lastRecv = iclock();
      if(ikcp_input(g_kcp, reinterpret_cast<char*>(buf), n) >= 0) {
        drainMessages();
      }
      continue;
    }

    if(n < 0 && errno == ECONNREFUSED) {
      printf("\n[!] 服务器无响应（端口不可达），请确认它已经启动。\n");
      g_running = 0;
      return;
    }

    break;
  }
}

static void onStdinReadable() {
  std::string line;
  if(!std::getline(std::cin, line)) {
    printf("\n输入结束，退出。\n");
    g_running = 0;
    return;
  }

  if(line.empty()) return;

  if (line == "/quit" || line == "/exit") {
        g_running = 0;
        return;
  }

  if(line.size() > static_cast<size_t>(MAX_MSG_LEN)) {
    printf("[!] 太长了：单条消息上限 %d 字节，你输入了 %zu 字节。\n",
               MAX_MSG_LEN, line.size());
    return;
  }

  const std::string m = pack(MSG_CHAT, line);
  if(ikcp_send(g_kcp, m.data(), m.size()) < 0) {
    printf("[!] 本地发送队列已满，这条消息没发出去。\n");
  }
}

int main(int argc, char **argv) {
  if(argc < 2) {
    fprintf(stderr,
            "用法: %s <服务器IP或域名> [端口] [昵称]\n"
            "例如: %s 127.0.0.1 8888 alice\n",
            argv[0], argv[0]);
    return 1;
  }

  const char *host = argv[1];
  const uint16_t port = (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : DEFAULT_PORT;
  std::string name = (argc > 3) ? argv[3] : std::string();

  if(name.empty()) {
    printf("请输入昵称: ");
    fflush(stdout);
    if(!std::getline(std::cin, name)) return 1;
    name = trim(name);
  }

  if(name.empty() || name.size() > static_cast<size_t>(MAX_MSG_LEN)) {
    fprintf(stderr, "昵称不能为空，且不超过 %d 字节。\n", MAX_NAME_LEN);
    return 1;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGPIPE, SIG_IGN);

  sockaddr_in server{};
  if(!resolve(host, port, server)) {
    fprintf(stderr, "无法解析主机名: %s\n", host);
    return 1;
  }

  g_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if(g_sock < 0) { perror("socket"); return 1; }
  if(::connect(g_sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) < 0) {
    perror("connect");
    ::close(g_sock);
    return 1;
  }

  std::random_device rd{};
  int conv = rd();
  if(conv == 0) conv = 1;

  g_kcp = ikcp_create(conv, nullptr);
  if(!g_kcp) {
    fprintf(stderr, "ikcp_create 失败\n");
    ::close(g_sock);
    return 1;
  }

  ikcp_setoutput(g_kcp, kcpOutput);
  ikcp_nodelay(g_kcp, 1, 10, 2, 1);
  ikcp_wndsize(g_kcp, 128, 128);

  g_lastRecv = iclock();
  g_lastPing = g_lastRecv;

  printf("正在连接 %s:%u，昵称「%s」……\n",
           host, static_cast<unsigned>(port), name.c_str());

  const std::string m = pack(MSG_LOGIN, name);
  ikcp_send(g_kcp, m.data(), static_cast<int>(m.size()));

  while(g_running) {
    const uint32_t now = iclock();

    int timeout = 100;
    {
      const int diff = static_cast<int>(ikcp_check(g_kcp, now) - now);
      if(diff < timeout) timeout = (diff < 0) ? 0 : diff;
    }

    if(std::cin.rdbuf()->in_avail() > 0) timeout = 0;

    pollfd pfd[2];
    pfd[0].fd = STDIN_FILENO; pfd[0].events = POLLIN; pfd[0].revents = 0;
    pfd[1].fd = g_sock;       pfd[1].events = POLLIN; pfd[1].revents = 0;

    const int n = ::poll(pfd, 2, timeout);
    if(n < 0) {
      if(errno == EINTR) continue;
      perror("poll");
      break;
    }

    if(pfd[1].revents & POLLIN) onSocketReadable();

    if(pfd[0].revents & (POLLIN | POLLHUP)) onStdinReadable();

    if(!g_running) break;

    const uint32_t t = iclock();
    if(static_cast<uint32_t>(t - g_lastPing) >= PING_INTERVAL * 1000u) {
      g_lastPing = t;
      const std::string ping = pack(MSG_PING);
      ikcp_send(g_kcp, ping.data(), ping.size());
    }

    if(static_cast<uint32_t>(t - g_lastRecv) > TIMEOUT_SEC * 1000u) {
      printf("\n[!] 已经 %d 秒没收到服务器的任何数据，判定为掉线。\n", TIMEOUT_SEC);
      break;
    }
    
    ikcp_update(g_kcp, iclock());
  }

  if(g_kcp) {
    const std::string bye = pack(MSG_LOGOUT);
    ikcp_send(g_kcp, bye.data(), static_cast<size_t>(bye.size()));
    
    const uint32_t deadline = iclock() + 300;

    while(static_cast<int32_t>(iclock() - deadline) < 0) {
      pollfd pfd{};
      pfd.fd = g_sock;
      pfd.events = POLLIN;
      if(::poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
        uint8_t buf[2048];
        ssize_t n = 0;
        while((n = ::recv(g_sock, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
          ikcp_input(g_kcp, reinterpret_cast<const char*>(buf), n);    
        }
      }
      
      ikcp_update(g_kcp, iclock());
      if(ikcp_waitsnd(g_kcp)) break;
    }
   ikcp_release(g_kcp);
   g_kcp = nullptr; 
  }

  if(g_sock >= 0) ::close(g_sock);
  printf("已断开。\n");
  return 0;
}
