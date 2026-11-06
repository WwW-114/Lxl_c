#include <sched.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#include "reactor.h"
#include "peer.h"

static reactor_t *g_reactor = NULL;
static peer_node_t *g_node = NULL;
static const char* g_prog = "tcp_p2p";

static char g_line_buf[BUFFER_SIZE];
static size_t g_line_len = 0;
static int g_stdin_flags = -1;
static int g_stdin_registered = 0;

static void signal_handle(int sig) {
  if(sig == SIGINT || sig == SIGTERM) {
    printf("\n[Main] Shutting down....\n");
    if(g_reactor) {
      reactor_stop(g_reactor);
    }
  }
}

static int stdin_nonblock_enable(void) {
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if(flags < 0) {
    perror("fcntl(F_GETFL)");
    return -1;
  }

  g_stdin_flags = flags;

  if((fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK | flags)) < 0) {
    perror("fcntl(F_SETFL)");
    g_stdin_flags = -1;
    return -1;
  }

  return 0;
}

static void stdin_flags_restore(void) {
  if(g_stdin_flags < 0) return;

  if((fcntl(STDIN_FILENO, F_SETFL, g_stdin_flags)) < 0) perror("fcntl(restore)");

  g_stdin_flags = -1;
}

void print_usage() {
  printf("Usage:\n");
  printf("  Server mode: %s -l <port>\n", g_prog);
  printf("  Client mode: %s -c <host> <port>\n", g_prog);
  printf("  Interactive: %s                    # 启动后再用 /listen 或 /connect\n", g_prog);
  printf("\n");
  printf("Commands:\n");
  printf("  /connect <host> <port>  主动连接对端（已有连接会先断开）\n");
  printf("  /listen <port>          开始监听（可与连接并存）\n");
  printf("  /unlisten               停止监听（已建立的连接保留）\n");
  printf("  /disconnect             断开当前连接（待发数据会先写完）\n");
  printf("  /status                 查看监听与连接状态\n");
  printf("  /help                   显示本帮助\n");
  printf("  /quit                   退出程序\n");
  printf("  <其它文本>               发送给当前连接的对端\n");
  printf("\n");
  printf("Examples:\n");
  printf("  %s -l 8888              # 监听 8888 端口\n", g_prog);
  printf("  %s -c 127.0.0.1 8888    # 连接到 127.0.0.1:8888\n", g_prog);
}

static bool port_ok(int port) {
  return port > 0 && port <= 65535; 
}

static int process_line(peer_node_t *node, char *line) {
  if (line[0] == '\0') return 0;                     /* 空行忽略 */

  if (line[0] != '/') {                              /* 普通文本 = 发送 */
    if (peer_node_send(node, line, strlen(line)) >= 0) {
      printf("[Sent] %s\n", line);
      fflush(stdout);
    }
    return 0;
  }

  if (strcmp(line, "/quit") == 0) {
    printf("[Main] Quitting...\n");
    reactor_stop(g_reactor);
    return -1;
  }
  if (strcmp(line, "/help") == 0) {
    print_usage();
    return 0;
  }
  if (strcmp(line, "/status") == 0) {
    peer_node_status(node);
    return 0;
  }
  if (strcmp(line, "/disconnect") == 0) {
    peer_node_disconnect(node);
    return 0;
  }
  if (strcmp(line, "/unlisten") == 0) {
    peer_node_unlisten(node);
    return 0;
  }
  if (strncmp(line, "/listen ", 8) == 0) {
    int port = atoi(line + 8);
    if (!port_ok(port)) {
      printf("[Input] 用法: /listen <port>  端口范围 1-65535\n");
    } else {
      peer_node_listen(node, NULL, port);
    }
    return 0;
  }
  if (strncmp(line, "/connect ", 9) == 0) {
    char host[64];
    int port = 0;
    /* %63s 的限宽不能省，否则超长 host 会溢出 host[64] */
    if (sscanf(line + 9, "%63s %d", host, &port) == 2 && port_ok(port)) {
      peer_node_connect(node, host, port);
    } else {
      printf("[Input] 用法: /connect <host> <port>\n");
    }
    return 0;
  }

  printf("[Input] 未知命令 %s（/help 查看帮助）\n", line);
  return 0;
}

static void handle_stdin(reactor_t *reactor, int fd, uint32_t events, void *arg) {
  peer_node_t *node = (peer_node_t*)arg;
  char buf[512];
  (void)events;

  for(; ;) {
    ssize_t n = read(fd, buf, sizeof(buf));

    if(n > 0) {
      for(ssize_t i = 0; i < n; ++i) {
        char c = buf[i];

        if(c == '\r') continue;

        if(c == '\n') {
          g_line_buf[g_line_len] = '\0';
          int r = process_line(node, g_line_buf);
          g_line_len = 0;
          if(r < 0) return;
          continue;
        }

        if(g_line_len + 1 >= sizeof(g_line_buf)) {
          printf("[Input] 行过长，已丢弃\n");
          g_line_len = 0;
          continue;
        }

        g_line_buf[g_line_len++] = c;
      }
      continue;    
    }

    if(n == 0) {
      if(isatty(fd)) {
        printf("\n[Input] 收到 EOF（Ctrl+D），退出\n");
        reactor_stop(reactor);
      } else {
        printf("[Input] stdin 已关闭，转入无交互模式（服务继续运行）\n");
        reactor_remove(reactor, fd);
        stdin_flags_restore();
        g_stdin_registered = 0;
      }
      return;
    }

    if(errno == EINTR) continue;
    if(errno == EWOULDBLOCK || errno == EAGAIN) return;

    perror("read");
    reactor_remove(reactor, fd);
    stdin_flags_restore();
    g_stdin_registered = 0;
    return;
  }
}

int main(int argc, char* argv[]) {
  g_prog = argv[0];
  
  signal(SIGINT, signal_handle);
  signal(SIGTERM, signal_handle);
  signal(SIGPIPE, SIG_IGN);

  g_reactor = reactor_create();
  if(g_reactor == NULL) {
    fprintf(stderr, "Failed to create reactor\n");
    return 1;
  }

  g_node = peer_node_create(g_reactor);
  if(g_node == NULL) {
    fprintf(stderr, "Failed to create peer\n");
    reactor_destory(g_reactor);
    return 1;
  }

  if(argc >= 3 && strcmp(argv[1], "-l") == 0) {
    int port = atoi(argv[2]);
    if(!port_ok(port)) {
      fprintf(stderr, "Invalid port %s\n", argv[2]);
    } else if(peer_node_listen(g_node, NULL, port) < 0) {
      fprintf(stderr, "监听失败，可在交互模式用 /listen 重试\n");
    }
  } else if(argc >= 4 && strcmp(argv[1], "-c") == 0) {
    const char *host = argv[2];
    int port = atoi(argv[3]);
    if(!port_ok(port)) {
      fprintf(stderr, "Invalid port : %s\n", argv[3]);
    } else if(peer_node_connect(g_node, host, port) < 0) {
      fprintf(stderr, "发起连接失败，可在交互模式用 /connect 重试\n");
    }
  } else if(argc != 1) {
    print_usage();
    return 1;
  }

  if(stdin_nonblock_enable() == 0 &&
     reactor_add(g_reactor, STDIN_FILENO, EPOLLIN, handle_stdin, g_node) == 0) {
    g_stdin_registered = 1;
  } else {
    fprintf(stderr, "[Main] 无法把 stdin 注册进事件循环，将只处理网络事件\n");
  }

  printf("[Main] 事件循环已启动（Ctrl+C 退出，/help 查看命令）\n");
  reactor_run(g_reactor);

  printf("[Main] Cleaning up...\n");

  if(g_stdin_registered) stdin_flags_restore();

  peer_node_destory(g_node);
  reactor_destory(g_reactor);

  printf("[Main] Goodbye!\n");
  return 0; 
}
