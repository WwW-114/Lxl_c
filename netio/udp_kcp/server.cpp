#include "ikcp.h"
#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <csignal>

struct Session {
  sockaddr_in         addr{};
  ikcpcb       *kcp = nullptr;
  uint32_t     conv = 0;
  std::string         name;
  bool loggedIn     = false;
  bool dead         = false;
  uint32_t lastRecv = 0; // 最后一次收到它任何数据的时刻（毫秒）

  Session() = default;
  Session(const Session &) = delete;
  Session& operator=(Session &) = delete;
  ~Session() { if(kcp) ikcp_release(kcp); }
};

static int g_sock = -1;
static volatile std::sig_atomic_t g_running = 1;

std::unordered_map<uint64_t, std::unique_ptr<Session>> g_sessions;

struct OutMsg {
  MsgType     type;
  std::string text;
};

static std::vector<OutMsg> g_outbox;   

static inline uint64_t sessionKey(const sockaddr_in &a) {
  return ((static_cast<uint64_t>(a.sin_addr.s_addr) << 16) |
          static_cast<uint64_t>(ntohs(a.sin_port))); 
}

static std::string peerName(const sockaddr_in &a) {
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

// 过滤控制字符。聊天用不到它们，但 ANSI 转义序列（\x1b[...）能让别人的
// 终端清屏、乱跳光标，甚至伪造出别人的发言，所以在转发前统一清掉。
// 注意这里用 unsigned char 遍历：UTF-8 的多字节字符每个字节都 >= 0x80，
// 用 char 会变成负数而被误判成控制字符。
static std::string sanitize(const std::string &in) {
  std::string out;
  out.reserve(in.size());

  for(unsigned char c : in) {
    if(c == '\t')                   out.push_back(c);
    else if(c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
  }
  
  return out;
}

// ---------------------------------------------------------------------------
//  发送
// ---------------------------------------------------------------------------
// 给单个会话发一条应用层消息。注意 ikcp_send 只是把数据放进 KCP 的发送
// 队列，真正发到网线上要等主循环里的 ikcp_update 来驱动（最长 10ms）。
static void sendTo(Session *s, MsgType type, const std::string &body = std::string()) {
  const std::string m = pack(type, body);
  ikcp_send(s->kcp, m.data(), static_cast<int>(m.size()));
}

static void broadcast(MsgType type, const std::string &text) {
  g_outbox.push_back(OutMsg{type, text});
}

static size_t onlineCount() {
  size_t n = 0;
  for(const auto &kv : g_sessions) if(kv.second->loggedIn && !kv.second->dead) ++n;
  return n;
}

static bool nameTaken(const std::string &name) {
  for(const auto &kv : g_sessions) {
    const Session *s = kv.second.get();
    if(s->loggedIn && !s->dead && s->name == name) return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
//  KCP 的输出回调
// ---------------------------------------------------------------------------
// KCP 每次想往网线上发包都会回调这里。ikcp_create(conv, user) 的第二个参数
// 会原样传进来，我们把 Session* 放在那儿，于是同一份回调代码就能服务任意
// 多个会话——这正是 KCP 的 user 指针存在的意义。
static int kcpOutput(const char* buf, int len, ikcpcb* /*kcp*/, void *user) {
  Session *s = static_cast<Session*>(user);
  const ssize_t n = sendto(g_sock, buf, static_cast<size_t>(len), 0,
                           reinterpret_cast<const sockaddr*>(&s->addr), sizeof(s->addr));

  return (n == len) ? 0 : -1;
}

static Session* createSession(const sockaddr_in &addr, uint32_t conv) {
  auto sp = std::make_unique<Session>();
  sp->addr = addr;
  sp->conv = conv;
  sp->kcp = ikcp_create(conv, sp.get());
  if(!sp->kcp) {
    fprintf(stderr, "ikcp_create 失败\n");
    return nullptr;
  }

  ikcp_setoutput(sp->kcp, kcpOutput);

  // 聊天室要的是低延迟，不是吞吐量：
  //   nodelay  = 1   有数据就发，不等攒够一批
  //   interval = 10  内部刷新间隔 10ms（默认 100ms，对聊天来说太钝了）
  //   resend   = 2   收到 2 个重复 ACK 就快速重传，不必干等超时
  //   nc       = 1   关掉拥塞控制（聊天流量很小，拥塞窗口只会添乱）
  ikcp_nodelay(sp->kcp, 1, 10, 2, 1);
  ikcp_wndsize(sp->kcp, 128, 128);

  sp->lastRecv = iclock();
  
  Session *raw = sp.get();
  g_sessions.emplace(sessionKey(addr), std::move(sp));
  printf("[*] 新连接 %s (conv=%u)\n", peerName(addr).c_str(), conv);
  return raw;
}

// 标记一个会话离线。这里只做标记和广播，真正的内存回收交给 reapSessions()
// ——免得在遍历会话表的过程中把它删掉。
static void leaveSession(Session *s, const char *reason) {
  if(s->dead) return;
  s->dead = true;
  if(!s->loggedIn) return;

  printf("[-] %s 离开了聊天室（%s）\n", s->name.c_str(), reason);
  broadcast(MSG_SYSTEM, s->name + "离开了聊天室 (" + reason + ")");
  s->loggedIn = false;
}

static void handleMessage(Session *s, const std::string &msg) {
  if(msg.empty()) return;

  const MsgType type = msgType(msg);
  const std::string body = msgBody(msg);

  switch (type) {
    case MSG_LOGIN:
      {
        if(s->loggedIn) return;

        const std::string name = trim(sanitize(body));
        if(name.empty() || name.size() > static_cast<size_t>(MAX_NAME_LEN)) {
          sendTo(s, MSG_SYSTEM,
                 "昵称不能为空，且不超过 " + std::to_string(MAX_NAME_LEN) + " 字节，请重新输入。");
          return;
        }
        if(nameTaken(name)) {
          sendTo(s, MSG_SYSTEM, "昵称「" + name + "」已经被占用了，换一个吧。");
          return;
        }

        s->name = name;
        s->loggedIn = true;

        sendTo(s, MSG_SYSTEM, "欢迎你，" + name + "！当前在线 " + std::to_string(onlineCount()) +
               " 人。直接输入文字回车即可发言，输入 /quit 退出。");
        broadcast(MSG_SYSTEM, name + " 加入了聊天室");
        printf("[+] %s 加入了聊天室，来自 %s，当前在线 %zu 人\n",
               name.c_str(), peerName(s->addr).c_str(), onlineCount());
        break;
      }

    case MSG_CHAT:
      {
        if(!s->loggedIn) {
          sendTo(s, MSG_SYSTEM, "请先设置昵称再发言。");
          return;
        }

        const std::string text = trim(sanitize(body));
        if(text.empty()) return;

        broadcast(MSG_BROADCAST, "[" + s->name + "] " + text);
        printf("[%s] %s\n", s->name.c_str(), text.c_str());

        break;
      }

    case MSG_LOGOUT:
      {
        leaveSession(s, "主动退出");
        break;
      }

    case MSG_PING:
      {
        sendTo(s, MSG_PONG);
        break;
      }
      
    default:
      break;
  }
}

static void onPacket(const uint8_t *data, int len, const sockaddr_in &src) {
  if(len < KCP_OVERHEAD) return;

  auto it = g_sessions.find(sessionKey(src));
  Session *s = nullptr;

  if(it == g_sessions.end()) {
    s = createSession(src, ikcp_getconv(data));
    if(!s) return;
  } else {
    s = it->second.get();
    if(ikcp_getconv(data) != s->conv) return;
  }

  s->lastRecv = iclock();

  if(ikcp_input(s->kcp, reinterpret_cast<const char*>(data), len) < 0) return;
  
  for(; ;) {
    const int size = ikcp_peeksize(s->kcp);
    if(size < 0) break;

    if(size > MAX_MSG_LEN) {
      ikcp_recv(s->kcp, nullptr, size);
      printf("[!] 丢弃一条来自 %s 的超长消息（%d 字节）\n", peerName(src).c_str(), size);
      continue;
    }

    std::string msg(static_cast<size_t>(size), '\0');
    int r = ikcp_recv(s->kcp, msg.data(), size);
    if(r < 0) break;
    msg.resize(static_cast<size_t>(r));
    
    handleMessage(s, msg);
    if(s->dead) break;
  }
}

static void reapSessions() {
  const uint32_t now = iclock();
  
  for(auto it = g_sessions.begin(); it != g_sessions.end(); ) {
    Session *s = it->second.get();

    // 超过应用层上限。必须把它取走丢掉，否则它会一直堵在接收队列
    // 里，后面的消息全过不来。ikcp_recv 允许 buffer 传 nullptr，
    // 正好用来丢弃数据（ikcp.c:395 只在 buffer 非空时才 memcpy）。
    if(!s->dead && static_cast<uint32_t>(now - s->lastRecv) > TIMEOUT_SEC * 1000u) {
      leaveSession(s, "心跳超时");
    }

    if(s->dead) {
      it = g_sessions.erase(it);
    } else {
      ++it;
    }
  }
}

static void dispatchOutbox() {
  if(g_outbox.empty()) return;

  for(const auto &kv : g_sessions) {
    Session *s = kv.second.get();

    if(!s->loggedIn || s->dead) continue;

    // 超过 TIMEOUT_SEC 没收到它的任何 UDP 包（包括 KCP 自己在发的 ACK
    // 和重传）就认定它掉线了。客户端正常退出时会发 LOGOUT，走不到这里。
    if(ikcp_waitsnd(s->kcp) > 1024) continue;

    for(const auto &m : g_outbox) {
      sendTo(s, m.type, m.text);
    }
  }

  g_outbox.clear();
}

static void onSignal(int) { g_running = 0; }

int main(int argc, char **argv) {
  uint16_t port = DEFAULT_PORT;

  // (1) 决定 poll 最多睡多久。
  //     KCP 不是"有数据才动"的：它必须被定时驱动，才能重传超时的包、
  //     把攒下的 ACK 回出去、把发送队列里的数据 flush 上路。ikcp_check
  //     会告诉我们下一次必须调用 ikcp_update 的时刻，拿它当 poll 的
  //     超时，既不忙等，也不会发迟。
  if(argc > 1) {
    const int p = atoi(argv[1]);
    if(p <= 0 || p > 65535) {
      fprintf(stderr, "端口不合法：%s\n", argv[1]);
      return 1;
    }
    port = static_cast<uint16_t>(p);
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGPIPE, SIG_IGN);

  g_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if(g_sock < 0) {
    perror("socket");
    return 1;
  }

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if(::bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    ::close(g_sock);
    return 1;
  }

  printf("=================================================\n");
    printf("  KCP 聊天室服务器\n");
    printf("  监听 UDP 端口 %u\n", static_cast<unsigned>(port));
    printf("  单条消息上限 %d 字节\n", MAX_MSG_LEN);
    printf("  心跳间隔 %d 秒，%d 秒收不到数据判定掉线\n", PING_INTERVAL, TIMEOUT_SEC);
    printf("  Ctrl+C 退出\n");
    printf("=================================================\n");

    while (g_running) {
        const uint32_t now = iclock();
        int timeout = 100;

        for(const auto &kv : g_sessions) {
          int diff = static_cast<int>(ikcp_check(kv.second->kcp, now) - now);
          if(diff < timeout) timeout = (diff < 0) ? 0 : diff;
        }

        pollfd pfd{};
        pfd.fd = g_sock;
        pfd.events = POLLIN;

        int n = ::poll(&pfd, 1, timeout);
        if(n < 0) {
          if(errno == EINTR) continue;
          perror("poll");
          break;
        }

        if(n > 0 && (pfd.revents & POLLIN)) {
          for(; ;) {
            uint8_t buf[2048];
            sockaddr_in src{};
            socklen_t slen = sizeof(src);
            const ssize_t r = ::recvfrom(g_sock, buf, sizeof(buf), MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&src), &slen);
            if(r <= 0) break;
            onPacket(buf, static_cast<int>(r), src);
          }
        }

        // (4) 驱动所有会话的 KCP：重传、回 ACK、把数据真正发出去
        const uint32_t t = iclock();
        for(const auto &kv : g_sessions) {
          Session *s = kv.second.get();
          ikcp_update(s->kcp, t);
        }

        reapSessions();

        dispatchOutbox();
    }

    printf("\n正在关闭……\n");
    for(auto & kv : g_sessions) {
      Session *s = kv.second.get();
      if(s->loggedIn && !s->dead) {
        sendTo(s, MSG_SYSTEM, "服务器已关闭，再见。");
      }
    }

    // KCP 的发送是异步的，给它们一点时间把道别发出去
    const uint32_t deadline = iclock() + 200;
    while(static_cast<int32_t>(iclock() - deadline) < 0) {
      for(auto &kv : g_sessions) ikcp_update(kv.second->kcp, iclock());
      ::usleep(10000);
    }

    g_sessions.clear();
    ::close(g_sock);
    printf("服务器已退出。\n");
    return 0;
}
