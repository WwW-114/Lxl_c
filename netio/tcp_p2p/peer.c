#include "peer.h"
#include "reactor.h"
#include <netinet/in.h>
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
#include <errno.h>
#include <arpa/inet.h>

#define PEER_SEND_INIT_CAP 1024
#define PEER_SEND_MAX_CAP (1024 * 1024)
#define PEER_DRAIN_MAX (512 * 1024) /*关掉连接之前最多能读取的数据*/

//前向声明
static void handle_accept(reactor_t *reactor, int fd, uint32_t events, void *arg);
static void handle_connect(reactor_t *reactor, int fd, uint32_t events, void *arg);
static void handle_peer_events(reactor_t *reactor, int fd, uint32_t events, void *arg);

static peer_t* peer_alloc(peer_node_t* node);
static void peer_close(peer_node_t *node, peer_t *conn, const char* reason);
static int do_read(peer_node_t *node, peer_t *conn);
static int do_write(peer_node_t *node, peer_t *conn);
static int peer_queue(peer_t *conn, const char *data, size_t length);
static void peer_update_interest(peer_t *conn, uint32_t events);
static void reject_connect(int fd, const char *msg);
static void peer_flush_before_exit(peer_node_t *node);

peer_node_t* peer_node_create(reactor_t *reactor) {
  peer_node_t *node = calloc(1, sizeof(peer_node_t));
  if(!node) return NULL;
  
  node->listen_fd = -1;
  node->reactor = reactor;
  node->conn = NULL;

  return node;
}

void peer_node_destory(peer_node_t *node) {
  if(!node) return;

  if(node->conn) {
    peer_flush_before_exit(node);
    if(node->conn) peer_close(node, node->conn, "processing exit");
  }

  if(node->listen_fd >= 0) {
    reactor_remove(node->reactor, node->listen_fd);
    close(node->listen_fd);
    node->listen_fd = -1;
  }

  free(node);
}

static peer_t* peer_alloc(peer_node_t *node) {
  peer_t *conn = calloc(1, sizeof(peer_t));
  if(!conn) return NULL;

  conn->node = node;
  conn->reactor = node->reactor;
  conn->state = PEER_DISCONNECTED;
  conn->fd = -1;

  return conn;
}

static void peer_close(peer_node_t *node, peer_t *conn, const char* reason) {
  if(!conn) return;

  int fd = conn->fd;
  int port = 0;
  char ip[INET_ADDRSTRLEN];

  if(fd >= 0) {
    inet_ntop(AF_INET, &conn->peer_addr.sin_addr, ip, sizeof(ip));
    port = htons(conn->peer_addr.sin_port);

    reactor_remove(conn->reactor, fd);

    char temp[1024];
    int drained = 0;

    while(drained < PEER_DRAIN_MAX - 1) {
      ssize_t n = read(fd, temp, sizeof(temp));
      if(n > 0) { drained += (int)n; continue; }
      else if(n < 0 && errno == EINTR) continue;
      break;
    }

    close(fd);
  }

  if(node->conn == conn) node->conn = NULL;
  free(conn->send_buff);
  free(conn);

  printf("connection is disconnect");
  if(port) printf("%s:%d", ip, port);
  if(reason) printf(" (%s) ", reason);
  printf("\n");
  fflush(stdout);
}

static void peer_flush_before_exit(peer_node_t *node) {
  peer_t *conn = node->conn;
  if(!conn || conn->state != PEER_CONNECTED) return;

  for(int i = 0; i < 5 && conn->send_sent < conn->send_len; ++i) {
    fd_set write_fd;
    FD_ZERO(&write_fd);
    FD_SET(conn->fd, &write_fd);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100 * 1000;

    if(select(conn->fd + 1, NULL, &write_fd, NULL, &tv) <= 0) break;
    if(do_write(node, conn) < 0) return; 
  }
}

static int do_read(peer_node_t *node, peer_t *conn) {
  for(; ;) {
    size_t space = sizeof(conn->recv_buff) - conn->recv_len - 1;

    if(space == 0) {
      printf("[Warning] Buffer full, processing as message\n");
      printf("[Received] %.*s\n",(int)conn->recv_len, conn->recv_buff);
      fflush(stdout);
      conn->recv_len = 0;
      conn->recv_buff[0] = '\0';
      space = sizeof(conn->recv_buff) - 1;
    }

    ssize_t n = read(conn->fd, conn->recv_buff + conn->recv_len, space);

    if(n > 0) {
      conn->recv_len += (size_t)n;
      conn->recv_buff[conn->recv_len] = '\0';

      char *newline;
      while((newline = memchr(conn->recv_buff, '\n', conn->recv_len)) != NULL) {
        printf("[Received] %.*s\n", (int)(newline - conn->recv_buff), conn->recv_buff);
        fflush(stdout);

        size_t remaining = conn->recv_len - (newline - conn->recv_buff + 1);
        if(remaining > 0) {
          memmove(conn->recv_buff, newline + 1, remaining);
        }

        conn->recv_len = remaining;
        conn->recv_buff[conn->recv_len] = '\0';
      }

      continue;
    } else if(n == 0) {
      peer_close(node, conn, "aside close");
      return -1;
    }

    if(errno == EINTR) continue;
    if(errno == EAGAIN || errno == EWOULDBLOCK) return 0;

    peer_close(node, conn, strerror(errno));
    return -1; 
  }
}

static int do_write(peer_node_t *node, peer_t *conn) {
  while(conn->send_sent < conn->send_len) {
    ssize_t n = write(conn->fd, conn->send_buff + conn->send_sent, conn->send_len - conn->send_sent);

    if(n > 0) {
      conn->send_sent += n;
      continue;
    }

    if(n < 0 && errno == EINTR) continue;

    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      peer_update_interest(conn, EPOLLIN | EPOLLOUT);
      return 0;
    }

    if(n == 0) {
      peer_close(node, conn, "(write) return zero");
      return -1;
    }

    peer_close(node, conn, strerror(errno));
    return -1;
  }

  conn->send_len = 0;
  conn->send_sent = 0;

  peer_update_interest(conn, EPOLLIN);
  
  if(conn->closing) {
    peer_close(node, conn, "proactive close");
    return -1;
  }
  
  return 0;
}

static void peer_update_interest(peer_t *conn, uint32_t events) {
  if(conn->interest == events) return;
  conn->interest = events;
  reactor_modify(conn->reactor, conn->fd, events, handle_peer_events, conn);
}

static void reject_connect(int fd, const char *msg) {
  char temp[256];
  int drained = 0;

  while(drained < 64 * 1024) {
    ssize_t n = read(fd, temp, sizeof(temp));
    if(n > 0) { drained += (int)n; continue; }
    else if(n < 0 && errno == EINTR) continue;
    break;
  }

  if(msg) {
    ssize_t ignored = write(fd, msg, strlen(msg));
    (void)ignored;
  }

  close(fd);
}

static int peer_queue(peer_t *conn, const char *data, size_t length) {
  if(conn->send_sent > 0) {
    size_t remain = conn->send_len - conn->send_sent;
    if(remain > 0) {
      memcpy(conn->send_buff, conn->send_buff + conn->send_sent, remain);
    }

    conn->send_len = remain;
    conn->send_sent = 0;
  }

  size_t need = conn->send_len + length + 1;
  if(need > PEER_SEND_MAX_CAP) {
    fprintf(stderr, "[Error] 发送缓冲已满（上限 %d 字节），本条消息未发送\n", PEER_SEND_MAX_CAP);
    return -1;
  }

  if(need > conn->send_cap) {
    size_t cap = conn->send_cap ? conn->send_cap : PEER_SEND_INIT_CAP;
    while(cap <= need) cap *= 2;
    if(cap > PEER_SEND_MAX_CAP) cap = PEER_SEND_MAX_CAP;

    char *new_send_buff = realloc(conn->send_buff, cap);
    if(!new_send_buff) {
      fprintf(stderr, "[Error] 内存不足，本条消息未发送\n");
      return -1;
    }

    conn->send_cap = cap;
    conn->send_buff = new_send_buff;
  }

  memcpy(conn->send_buff + conn->send_len, data, length);
  conn->send_len += length;
  conn->send_buff[conn->send_len++] = '\n';

  return do_write(conn->node, conn);
}


static void handle_accept(reactor_t *reactor, int fd, uint32_t events, void *arg) {
  peer_node_t *node = (peer_node_t*)arg;
  (void)events;

  for(; ;) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(fd, (struct sockaddr*)&client_addr, &addr_len);
    if(client_fd < 0) {
      if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        perror("accept");
      }
      return;
    }

    set_noblocking(client_fd);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

    if(node->conn) {
      reject_connect(client_fd, "Have already connected\n");
      printf("[Node] 拒绝来自 %s:%d 的连接（已有一对端）\n",
             ip, ntohs(client_addr.sin_port));
      fflush(stdout);
      continue;
    }

    peer_t *conn = peer_alloc(node);
    if(!conn) { close(client_fd); continue; }

    conn->fd = client_fd;
    conn->inbound = true;
    conn->peer_addr = client_addr;
    conn->state = PEER_CONNECTED;
    conn->interest = EPOLLIN;

    if(reactor_add(reactor, client_fd, EPOLLIN, handle_peer_events, conn) < 0) {
      fprintf(stderr, "[Node] 注册连接 fd 失败\n");
      close(client_fd);
      free(conn);
      continue;
    }

    node->conn = conn;
    printf("[node] 新连接来自 %s:%d（入向）\n", ip, ntohs(client_addr.sin_port));
    fflush(stdout);
  }
}

static void handle_connect(reactor_t *reactor, int fd, uint32_t events, void *arg) {
  peer_t *conn = (peer_t*)arg;
  peer_node_t *node = conn->node;
  (void)events;

  int error = 0;
  socklen_t len = sizeof(error);

  if((getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) < 0 || error != 0) {
    char buf[128];
    snprintf(buf, sizeof(buf), "连接失败: %s",
             strerror(error ? error : errno));
    peer_close(node, conn, buf);
    return;
  }

  conn->state = PEER_CONNECTED;
  reactor_modify(reactor, fd, EPOLLIN, handle_peer_events, conn);

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &conn->peer_addr.sin_addr, ip, sizeof(ip));
  
  printf("[Node] 已连接到 %s:%d（出向）\n", ip, ntohs(conn->peer_addr.sin_port));
  fflush(stdout);
}

static void handle_peer_events(reactor_t *reactor, int fd, uint32_t events, void *arg) {
  peer_t* conn  = (peer_t*)arg;
  peer_node_t *node = conn->node;
  (void)reactor;
  (void)fd;

  if((events & (EPOLLIN | EPOLLERR | EPOLLHUP)) && do_read(node, conn) < 0) return;
  if((events & EPOLLOUT) && do_write(node, conn) < 0) return;
}

int peer_node_listen(peer_node_t *node, const char *host, int port) {
  if(node->listen_fd >= 0) {
    fprintf(stderr, "[Node] 已在监听中，请先 /unlisten\n");
    return -1;
  }
  
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) {
    perror("socket");
    return -1;
  }

  int opt = 1;
  if((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
    perror("setsockopt");
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if(host) {
    if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
      perror("inet_pton");
      close(fd);
      return -1;
    }
  } else {
    addr.sin_addr.s_addr = INADDR_ANY;
  }

  if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }

  if(listen(fd, SOMAXCONN) < 0) {
    perror("listen");
    close(fd);
    return -1;
  }

  set_noblocking(fd);

  if(reactor_add(node->reactor, fd, EPOLLIN, handle_accept, node) < 0) {
    fprintf(stderr, "[Node] 注册监听 fd 失败\n");
    close(fd);
    return -1;
  }

  node->listen_fd = fd;
  node->listen_addr = addr;

  printf("[Node] 正在监听 %s:%d\n", host ? host : "0.0.0.0", port);
  fflush(stdout);
      
  return 0;
}

int peer_node_unlisten(peer_node_t *node) {
  if(node->listen_fd < 0) {
    fprintf(stderr, "[Node] 当前未监听\n");
    return -1;
  }

  reactor_remove(node->reactor, node->listen_fd);
  close(node->listen_fd);
  node->listen_fd = -1;

  printf("[Node] 已停止监听（已有连接不受影响）\n");
  fflush(stdout);
  
  return 0;
}

int peer_node_connect(peer_node_t *node, const char *host, int port) {
  if(node->conn) {
    peer_node_disconnect(node);
    printf("[Node] 先断开已有连接...\n");
    
    if(node->conn) {
      fprintf(stderr, "[Node] 上一条连接仍在断开中，请稍后再试 /connect\n");
      return -1;
    }
  }
  
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);

  if(!host || inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    fprintf(stderr, "[Node] 地址非法: %s\n", host ? host : "(null)");
    close(fd);
    return -1;
  }

  peer_t *conn = peer_alloc(node);
  if(!conn) {
    close(fd);
    return -1;
  }

  conn->fd = fd;
  conn->inbound = false;
  conn->interest = EPOLLOUT;
  conn->peer_addr = addr;
  conn->state = PEER_CONNECTIING;

  set_noblocking(fd);
  
  if((connect(fd, (struct sockaddr*)&addr, sizeof(addr))) < 0 && errno != EINPROGRESS) {
    char buf[128];
    snprintf(buf, sizeof(buf), "connect : %s", strerror(errno));

    node->conn = conn;
    peer_close(node, conn, buf);
    return -1;
  }
  
  if(reactor_add(node->reactor, fd, EPOLLOUT, handle_connect, conn) < 0) {
    fprintf(stderr, "[Node] 注册连接 fd 失败\n");
    close(fd);
    free(conn);
    return -1;
  }

  node->conn = conn;

  printf("[Node] 正在连接 %s:%d ...\n", host, port);  
  fflush(stdout);
  
  return 0;
}

int peer_node_disconnect(peer_node_t *node) {
  peer_t *conn = node->conn;
  if(!conn) {
    fprintf(stderr, "[Node] 当前没有连接\n");
    return -1;
  }

  if(conn->state != PEER_CONNECTED) {
    peer_close(node, conn, "Cut connection");
    return 0;
  }

  conn->closing = true;

  if(do_write(node, conn) < 0) return 0;

  printf("[Node] 仍有 %zu 字节待发送，写完即断开...\n",
         conn->send_len - conn->send_sent);
  fflush(stdout);

  return 0;
}

int peer_node_send(peer_node_t *node, const char *data, size_t length) {
  peer_t *conn = node->conn;
  
  if(!conn || conn->state != PEER_CONNECTED) {
    fprintf(stderr, "[Error] 尚未建立连接（先 /connect 或 /listen 等对端连入）\n");
    return -1;
  }

  if(conn->closing) {
    fprintf(stderr, "[Error] 正在断开，无法发送\n");
    return -1;
  }

  return peer_queue(conn, data, length);
}

void peer_node_status(const peer_node_t *node) {
  char buf[INET_ADDRSTRLEN];

  if(node->listen_fd >= 0) {
    inet_ntop(AF_INET, &node->listen_addr.sin_addr, buf, sizeof(buf));
    printf("[Node] 监听: %s:%d\n", buf, ntohs(node->listen_addr.sin_port));
  } else {
    printf("[Node] 监听: 未监听\n");
  }

  peer_t *conn = node->conn;
  if(!conn) {
    printf("[Node] 连接: 无\n");
    fflush(stdout);
    return;
  }

  const char *dir = conn->inbound ? "入向" : "出向";
  inet_ntop(AF_INET, &conn->peer_addr.sin_addr, buf, sizeof(buf));
  int port = htons((uint16_t)conn->peer_addr.sin_port);

  switch (conn->state) {
    case PEER_CONNECTIING:
      printf("[Node] 连接: 连接中 %s:%d（%s）\n", buf, port, dir);
      break;
    case PEER_CONNECTED:
      printf("[Node] 连接: %s %s:%d（%s，待发 %zu 字节）\n",
             conn->closing ? "正在断开" : "已连接",
             buf, port, dir,
             conn->send_len - conn->send_sent);
      break;
    default:
      printf("[Node] 连接: 空闲\n");
      break;
  }
  fflush(stdout);
}
