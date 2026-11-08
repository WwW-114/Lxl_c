/* ============================================================================
 * test_qps_tcpclient_v2.c
 *
 * 面向 netio/io_uring/echo_server 的并发压测客户端：
 *   - 多线程 + 每线程多连接 + 每连接可在途 pipeline 个请求
 *   - 用「长度前缀 + 序号」做应用层分帧，正确处理 TCP 的拆包 / 粘包
 *   - 正确处理 send 短写、recv 半包、连接被重置、对端关闭
 *   - 连接建立完成后再统一开始计时，QPS 只统计校验成功的请求
 *
 * 为什么需要分帧：
 *   TCP 是字节流，不是消息流。一次 send 可能被拆成多次 recv，多次 send
 *   也可能被合并进一次 recv。服务端 echo_server 的模型是“收到多少回显
 *   多少”，所以回显流的字节内容与发送流完全一致，但 recv 的边界完全
 *   不可预测。客户端必须自己缓存字节流并循环解析出完整帧。
 *
 * 帧格式（网络字节序，大端）：
 *
 *   0        4        8        12                        12+len
 *   +--------+--------+--------+--------------------------+
 *   | magic  |  seq   |  len   |        payload           |
 *   | 4B     | 4B     | 4B     |        len 字节           |
 *   +--------+--------+--------+--------------------------+
 *
 *   magic = 0x51505431 ("QPT1")
 *   seq   = 该连接内递增的请求序号，从 0 开始
 *   len   = payload 长度（固定为 -b 指定的值）
 *   payload 前 4 字节再放一次 seq（大端），其余为固定模板，便于逐字节校验
 *
 * 编译（Linux / WSL2）：
 *   gcc -O2 -Wall -Wextra -D_GNU_SOURCE -pthread \
 *       test_qps_tcpclient_v2.c -o test_qps_tcpclient_v2
 *
 * 运行示例：
 *   ./test_qps_tcpclient_v2 -s 127.0.0.1 -p 9999 \
 *       -t 4 -c 64 -n 200000 -b 1920 -P 4
 *
 * 复现拆包 / 粘包：
 *   -b 8192          请求帧 8204 字节，远大于服务端 4096 的 recv 缓冲区，
 *                    服务端必然分多次回显 -> 复现“拆包”
 *   -P 16            一次连续发出 16 帧，服务端可能一次 recv 到多帧并
 *                    一起回显 -> 复现“粘包”
 *   本程序对以上两种情况都应全部校验成功。
 *
 * 注意事项：
 *   1. 服务端每次 recv 都会 fprintf/fwrite 到 stdout，压测时务必把服务端
 *      输出重定向到 /dev/null，否则测到的是终端 I/O 的速度。
 *   2. echo_server 的 MAX_CONNS = 1024，且 fd 直接作为数组下标，
 *      建议客户端总连接数 <= 900（还要扣除 stdin/stdout/stderr/listen fd）。
 *   3. echo_server 的 LISTEN_BACKLOG = 128，瞬间大量 connect 可能失败；
 *      本客户端把连接失败计入“失败/未完成”并继续其他连接，不会让整个
 *      进程退出；连续失败 5 次会跳过本线程剩余连接，避免服务器没启动
 *      时每个连接都等满 3 秒超时。
 *   4. 服务端未设置 TCP_NODELAY，Nagle + 延迟 ACK 可能让少量连接的
 *      小包 RTT 出现毫秒级抖动；这属于服务端行为，不是客户端统计错误。
 *   5. -P > 1 时 RTT 从“帧进入发送队列”开始算，包含排队时间，
 *      因此会略大于纯网络往返时间。
 * ==========================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * 常量
 * -------------------------------------------------------------------------*/
#define FRAME_MAGIC       0x51505431u   /* "QPT1" */
#define FRAME_HDR         12u           /* magic(4) + seq(4) + len(4) */
#define MIN_PAYLOAD       4             /* payload 前 4 字节放 seq */
#define MAX_PAYLOAD       (16 * 1024 * 1024)
#define MAX_PIPELINE      64
#define RECV_SLACK        4096u         /* recv 缓冲区在“一帧”之外多留的余量 */
#define CONNECT_TIMEOUT_MS 3000
#define CONNECT_FAIL_STREAK 5   /* 连续失败这么多次就跳过本线程剩余连接 */
#define POLL_TIMEOUT_MS   1000
#define STALL_TIMEOUT_US  (60LL * 1000 * 1000)  /* 60 秒无任何进展则终止 */
#define MAX_WORKERS       256

/* ---------------------------------------------------------------------------
 * 配置（解析完成后只读，所有线程共享）
 * -------------------------------------------------------------------------*/
typedef struct {
  char     ip[64];
  uint16_t port;
  int      threads;      /* -t 工作线程数 */
  int      conns;        /* -c 总连接数（均分到各线程） */
  long long requests;    /* -n 总请求数（均分到各连接） */
  int      payload;      /* -b 每帧 payload 字节数 */
  int      pipeline;     /* -P 每连接最多在途请求数 */
} config_t;

static config_t         g_cfg;
static struct sockaddr_in g_server_addr;   /* main 中解析一次，线程只读 */
static unsigned char   *g_payload_template;/* -b 字节的固定校验模板 */

/* ---------------------------------------------------------------------------
 * 每连接状态
 * -------------------------------------------------------------------------*/
typedef struct {
  int fd;                  /* -1 表示连接已关闭 */

  int done;                /* 1 = 本连接不再参与 poll */
  int payload;             /* 本连接每帧 payload 长度 */
  int pipeline;            /* 本连接最大在途帧数 */

  size_t frame_size;       /* FRAME_HDR + payload */

  /* 发送队列：只在 soff == slen（已全部发完）时才追加新帧，
   * 因此容量取 pipeline * frame_size 即可。 */
  unsigned char *sbuf;
  size_t scap;
  size_t slen;             /* 已排入队列的字节数 */
  size_t soff;             /* 已成功发出的字节数 */

  /* 接收累积缓冲区：一次 recv 可能只有半帧，也可能包含多帧。 */
  unsigned char *rbuf;
  size_t rcap;
  size_t rlen;             /* 当前缓冲区里的有效字节数 */

  uint32_t next_seq;       /* 下一个要发送的帧序号 */
  uint32_t expect_seq;     /* 下一个期望收到的帧序号 */

  long long target;        /* 本连接需要完成的请求数 */
  long long issued;        /* 已排入发送队列的帧数 */
  long long inflight;      /* 已排队但尚未解析到响应的帧数 */
  long long completed;     /* 已校验成功的响应帧数 */

  /* RTT 时间戳环：在途帧不超过 pipeline，序号递增且按序返回，
   * 用 seq % pipeline 作下标不会冲突。 */
  uint64_t *ts;
} conn_t;

/* ---------------------------------------------------------------------------
 * 每线程状态与统计
 * -------------------------------------------------------------------------*/
typedef struct {
  int         id;
  int         nconns;
  conn_t     *conns;

  struct pollfd *pfds;
  conn_t       **map;       /* pfds[i] 对应的连接 */
  int           nfds;

  long long   target_total; /* 分配到的请求总数 */
  long long   ok;           /* 校验成功的响应数 */
  long long   conn_errors;  /* 连接失败 / 异常总数 */

  /* 用于证明“拆包 / 粘包”路径确实被触发：
   *   partial_waits：解析时遇到半帧、需要等下一次 recv 的次数
   *   max_batch    ：单次 conn_parse 最多连续解析出的完整帧数 */
  long long   partial_waits;
  int         max_batch;

  uint64_t    rtt_sum;
  uint64_t    rtt_min;
  uint64_t    rtt_max;

  uint64_t    last_progress_us;
  int64_t     start_us;
  int64_t     end_us;

  pthread_t   tid;
} worker_t;

static pthread_barrier_t g_barrier;

/* ---------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------*/
static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (p == NULL) {
    fprintf(stderr, "malloc(%zu) 失败\n", n);
    exit(1);
  }
  return p;
}

static void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n, size);
  if (p == NULL) {
    fprintf(stderr, "calloc(%zu, %zu) 失败\n", n, size);
    exit(1);
  }
  return p;
}

static uint64_t now_us(void) {
  struct timespec ts = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    /* CLOCK_MONOTONIC 实际不会失败；真失败说明运行环境异常，
     * 静默返回 0 会让停滞检测和 connect 截止时间全部失真。 */
    fprintf(stderr, "clock_gettime(CLOCK_MONOTONIC) 失败: %s\n",
            strerror(errno));
    exit(1);
  }
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void put_u32_be(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)(v);
}

static uint32_t get_u32_be(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |
         ((uint32_t)p[3]);
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------------------------------------------------------------------------
 * 建立连接：非阻塞 connect + poll 超时，避免服务器不可达时长时间挂住
 * -------------------------------------------------------------------------*/
static int tcp_connect(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  if (set_nonblocking(fd) < 0) {
    close(fd);
    return -1;
  }

  /* 压测小包时必须关掉 Nagle，否则 request/response 模式会被
   * Nagle + 延迟 ACK 放大成毫秒级延迟。 */
  {
    int on = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  }

  int r = connect(fd, (struct sockaddr *)&g_server_addr,
                  (socklen_t)sizeof(g_server_addr));
  if (r < 0) {
    /* 非阻塞 connect 可能返回 EINPROGRESS；个别情况下也会返回 EINTR，
     * 两者都应该用 poll 等待连接完成结果。 */
    if (errno != EINPROGRESS && errno != EINTR) {
      close(fd);
      return -1;
    }

    struct pollfd pfd = {fd, POLLOUT, 0};
    int64_t deadline = (int64_t)now_us() + (int64_t)CONNECT_TIMEOUT_MS * 1000;

    for (;;) {
      int64_t remain_us = deadline - (int64_t)now_us();
      if (remain_us <= 0) {
        close(fd);
        return -1;
      }
      int remain_ms = (int)((remain_us + 999) / 1000);
      r = poll(&pfd, 1, remain_ms);
      if (r < 0 && errno == EINTR) {
        continue;
      }
      if (r <= 0) {
        close(fd);
        return -1;
      }
      break;
    }

    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
      close(fd);
      return -1;
    }
  }

  return fd;
}

/* ---------------------------------------------------------------------------
 * 分帧：构造一帧到 dst
 * -------------------------------------------------------------------------*/
static void build_frame(const conn_t *c, unsigned char *dst, uint32_t seq) {
  put_u32_be(dst, FRAME_MAGIC);
  put_u32_be(dst + 4, seq);
  put_u32_be(dst + 8, (uint32_t)c->payload);

  unsigned char *payload = dst + FRAME_HDR;
  put_u32_be(payload, seq);                    /* 前 4 字节放序号 */
  memcpy(payload + 4, g_payload_template + 4,  /* 其余部分用固定模板 */
         (size_t)c->payload - 4);
}

/* ---------------------------------------------------------------------------
 * 连接初始化
 * -------------------------------------------------------------------------*/
static void conn_init(conn_t *c, int global_index) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;

  c->payload  = g_cfg.payload;
  c->pipeline = g_cfg.pipeline;
  c->frame_size = (size_t)FRAME_HDR + (size_t)c->payload;

  /* 把总请求数尽量平均地分给所有连接，余数分给前 rem 个连接。 */
  long long base = g_cfg.requests / g_cfg.conns;
  long long rem  = g_cfg.requests % g_cfg.conns;
  c->target = base + ((long long)global_index < rem ? 1 : 0);

  c->scap = (size_t)c->pipeline * c->frame_size;
  c->sbuf = (unsigned char *)xmalloc(c->scap);

  /* 一帧 + 余量就够：解析循环会及时把完整帧消费掉，保证总有空间前进。 */
  c->rcap = c->frame_size + RECV_SLACK;
  c->rbuf = (unsigned char *)xmalloc(c->rcap);

  c->ts = (uint64_t *)xcalloc((size_t)c->pipeline, sizeof(uint64_t));
}

/* ---------------------------------------------------------------------------
 * 发送：处理短写与 EAGAIN
 * 返回 0 表示当前可发数据都已发出（或暂时发不动），-1 表示致命错误。
 * -------------------------------------------------------------------------*/
static int conn_flush(conn_t *c) {
  while (c->soff < c->slen) {
    ssize_t n = send(c->fd, c->sbuf + c->soff,
                     c->slen - c->soff, MSG_NOSIGNAL);
    if (n > 0) {
      c->soff += (size_t)n;
      continue;
    }
    if (n == 0) {
      errno = EPIPE;   /* send 返回 0 不会设置 errno，补一个可读的错误原因 */
      return -1;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;   /* 等 POLLOUT */
    }
    return -1;
  }

  /* 全部发完，重置队列，下一次 conn_queue 可以从头写。 */
  c->slen = 0;
  c->soff = 0;
  return 0;
}

/* ---------------------------------------------------------------------------
 * 解析接收缓冲区
 *
 * 返回 0 表示正常；-1 表示协议错误（magic / 长度 / seq / payload 不匹配）。
 *
 * 关键点：
 *   - while 循环处理“粘包”：一次 recv 里可能有多帧，循环逐帧消费。
 *   - rlen - off < need 时 break，处理“拆包”：半帧留在缓冲区，
 *     等下一次 recv 补齐后再继续。
 *   - 消费完用一次 memmove 把剩余半帧移到缓冲区头部，避免逐帧搬移。
 * -------------------------------------------------------------------------*/
static int conn_parse(conn_t *c, worker_t *w) {
  size_t off = 0;
  int batch = 0;   /* 本次调用解析出的完整帧数 */

  while (c->rlen - off >= FRAME_HDR) {
    const unsigned char *hdr = c->rbuf + off;
    uint32_t magic = get_u32_be(hdr);
    uint32_t seq   = get_u32_be(hdr + 4);
    uint32_t len   = get_u32_be(hdr + 8);

    if (magic != FRAME_MAGIC || len != (uint32_t)c->payload) {
      return -1;
    }

    size_t need = (size_t)FRAME_HDR + (size_t)len;
    if (c->rlen - off < need) {
      break;   /* 半包：等后续字节 */
    }

    const unsigned char *payload = hdr + FRAME_HDR;

    /* 按序校验：序号必须连续，否则说明丢帧或流错位。 */
    if (seq != c->expect_seq) {
      return -1;
    }

    unsigned char seq_be[4];
    put_u32_be(seq_be, c->expect_seq);
    if (memcmp(payload, seq_be, 4) != 0) {
      return -1;
    }
    if (c->payload > 4 &&
        memcmp(payload + 4, g_payload_template + 4,
               (size_t)c->payload - 4) != 0) {
      return -1;
    }

    if (c->inflight <= 0) {
      return -1;   /* 防御：没有在途请求却收到了响应，说明流已错位 */
    }

    uint32_t idx = seq % (uint32_t)c->pipeline;
    uint64_t sent_us = c->ts[idx];
    uint64_t now = now_us();
    uint64_t rtt = (now > sent_us) ? (now - sent_us) : 0;

    w->ok++;
    w->rtt_sum += rtt;
    if (rtt < w->rtt_min) {
      w->rtt_min = rtt;
    }
    if (rtt > w->rtt_max) {
      w->rtt_max = rtt;
    }
    w->last_progress_us = now;

    c->completed++;
    c->expect_seq++;
    c->inflight--;
    off += need;
    batch++;
  }

  if (batch > w->max_batch) {
    w->max_batch = batch;
  }

  /* 本次解析后仍有残留字节：可能是帧头被拆开，也可能是 payload 没到齐。
   * 统计的是“需要等下一次 recv 的次数”，不是被拆的帧数。 */
  if (c->rlen - off > 0) {
    w->partial_waits++;
  }

  if (off > 0) {
    memmove(c->rbuf, c->rbuf + off, c->rlen - off);
    c->rlen -= off;
  }

  return 0;
}

/* ---------------------------------------------------------------------------
 * 接收并解析
 * 返回 0 正常，-1 recv 错误（errno 有效），-2 对端关闭，-3 协议错误。
 * -------------------------------------------------------------------------*/
static int conn_recv(conn_t *c, worker_t *w) {
  /* 限制单次事件里的循环次数，避免某个高速连接饿死同线程的其他连接。 */
  for (int i = 0; i < 32; ++i) {
    if (c->rlen == c->rcap) {
      /* 正常不会发生：一帧必然能塞进 rcap，解析后一定有空位。 */
      return -3;
    }

    ssize_t n = recv(c->fd, c->rbuf + c->rlen,
                     c->rcap - c->rlen, 0);
    if (n > 0) {
      c->rlen += (size_t)n;
      if (conn_parse(c, w) < 0) {
        return -3;
      }
      if (c->completed >= c->target) {
        return 0;   /* 本连接目标已完成，不用再读 */
      }
      continue;
    }

    if (n == 0) {
      /* 对端关闭。如果目标刚好已完成，应算成功而不是失败。 */
      return (c->completed >= c->target) ? 0 : -2;
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return -1;
  }

  return 0;
}

/* ---------------------------------------------------------------------------
 * 把一个连接上可排的帧排入发送队列
 *
 * 只有在发送队列为空时才追加，避免在半帧中间插入新帧；
 * 在途帧数受 pipeline 限制，从而给发送缓冲区一个上界。
 * -------------------------------------------------------------------------*/
static void conn_queue(conn_t *c) {
  if (c->soff != c->slen) {
    return;   /* 上一批还没发完 */
  }

  c->slen = 0;
  c->soff = 0;

  while (c->issued < c->target && c->inflight < c->pipeline) {
    if (c->slen + c->frame_size > c->scap) {
      break;   /* 防御性检查，正常不会触发 */
    }

    uint32_t seq = c->next_seq++;
    build_frame(c, c->sbuf + c->slen, seq);
    c->ts[seq % (uint32_t)c->pipeline] = now_us();

    c->slen += c->frame_size;
    c->issued++;
    c->inflight++;
  }
}

static void conn_fail(conn_t *c, worker_t *w, const char *reason) {
  if (c->done) {
    return;
  }

  if (w->conn_errors < 5) {
    fprintf(stderr, "[worker %d] 连接 fd=%d 关闭: %s\n",
            w->id, c->fd, reason);
  }

  w->conn_errors++;

  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  c->done = 1;
}

/* ---------------------------------------------------------------------------
 * 单线程事件循环：poll 同时盯住本线程的所有连接
 * -------------------------------------------------------------------------*/
static void worker_run(worker_t *w) {
  w->rtt_min = UINT64_MAX;
  w->last_progress_us = now_us();

  for (;;) {
    /* 每轮开始都检查：即使 poll 持续返回 n>0 但没有任何完整帧，
     * 60 秒兜底也不会失效。 */
    if (now_us() - w->last_progress_us > (uint64_t)STALL_TIMEOUT_US) {
      fprintf(stderr, "[worker %d] 超过 %lld 秒没有收到完整响应帧，终止\n",
              w->id, (long long)(STALL_TIMEOUT_US / 1000000));
      break;
    }

    /* 1. 构造本轮 poll 关注的事件集合 */
    w->nfds = 0;

    for (int i = 0; i < w->nconns; ++i) {
      conn_t *c = &w->conns[i];
      if (c->done) {
        continue;
      }

      if (c->completed >= c->target) {
        if (c->fd >= 0) {
          close(c->fd);
          c->fd = -1;
        }
        c->done = 1;
        continue;
      }

      conn_queue(c);

      short events = POLLIN;
      if (c->soff < c->slen) {
        events |= POLLOUT;
      }

      w->pfds[w->nfds].fd      = c->fd;
      w->pfds[w->nfds].events  = events;
      w->pfds[w->nfds].revents = 0;
      w->map[w->nfds] = c;
      w->nfds++;
    }

    if (w->nfds == 0) {
      break;   /* 所有连接都完成或失败 */
    }

    /* 2. 等待事件 */
    int n = poll(w->pfds, (nfds_t)w->nfds, POLL_TIMEOUT_MS);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "[worker %d] poll 失败: %s\n", w->id, strerror(errno));
      break;
    }

    if (n == 0) {
      continue;   /* 超时但没有事件，回到循环顶部做停滞检查 */
    }

    /* 3. 逐个处理就绪事件 */
    for (int i = 0; i < w->nfds; ++i) {
      if (w->pfds[i].revents == 0) {
        continue;
      }

      conn_t *c = w->map[i];
      if (c->done) {
        continue;
      }

      if (w->pfds[i].revents & (POLLERR | POLLNVAL)) {
        conn_fail(c, w, "POLLERR/POLLNVAL");
        continue;
      }

      if (w->pfds[i].revents & POLLOUT) {
        if (conn_flush(c) < 0) {
          char msg[128];
          snprintf(msg, sizeof(msg), "send 失败: %s", strerror(errno));
          conn_fail(c, w, msg);
          continue;
        }
      }

      if (w->pfds[i].revents & (POLLIN | POLLHUP)) {
        int r = conn_recv(c, w);
        if (r == -2) {
          conn_fail(c, w, "对端关闭连接");
        } else if (r == -3) {
          conn_fail(c, w, "协议错误（magic/len/seq/payload 不匹配）");
        } else if (r == -1) {
          char msg[128];
          snprintf(msg, sizeof(msg), "recv 失败: %s", strerror(errno));
          conn_fail(c, w, msg);
        }
      }
    }
  }

  /* 收尾：关闭本线程仍存活的连接，并补记连接级错误 */
  for (int i = 0; i < w->nconns; ++i) {
    conn_t *c = &w->conns[i];
    if (c->fd >= 0) {
      if (c->completed < c->target) {
        w->conn_errors++;   /* stall / poll 致命错误导致没有跑完 */
      }
      close(c->fd);
      c->fd = -1;
    }
    c->done = 1;
  }
}

/* ---------------------------------------------------------------------------
 * 线程入口
 *
 * 连接建立放在 barrier 之前：所有线程的连接都建好之后才一起开始发请求，
 * 这样 connect 的耗时不会被算进 QPS。
 * -------------------------------------------------------------------------*/
static void *worker_entry(void *arg) {
  worker_t *w = (worker_t *)arg;
  int shown = 0;
  int fail_streak = 0;

  for (int i = 0; i < w->nconns; ++i) {
    conn_t *c = &w->conns[i];
    c->fd = tcp_connect();

    if (c->fd < 0) {
      c->done = 1;
      w->conn_errors++;
      if (shown++ < 5) {
        fprintf(stderr, "[worker %d] 连接 %s:%u 失败\n",
                w->id, g_cfg.ip, (unsigned)g_cfg.port);
      }

      /* 服务器没启动时不要为每个连接都等 3 秒超时 */
      if (++fail_streak >= CONNECT_FAIL_STREAK) {
        int skipped = w->nconns - i - 1;
        for (int j = i + 1; j < w->nconns; ++j) {
          w->conns[j].done = 1;
        }
        w->conn_errors += (long long)skipped;
        if (skipped > 0) {
          fprintf(stderr,
                  "[worker %d] 连续 %d 次连接失败，跳过剩余 %d 个连接\n",
                  w->id, CONNECT_FAIL_STREAK, skipped);
        }
        break;
      }
    } else {
      fail_streak = 0;
    }
  }

  /* 等待所有线程完成连接建立 */
  pthread_barrier_wait(&g_barrier);

  w->start_us = (int64_t)now_us();
  worker_run(w);
  w->end_us = (int64_t)now_us();

  return NULL;
}

/* ---------------------------------------------------------------------------
 * 参数解析
 * -------------------------------------------------------------------------*/
static void usage(const char *prog) {
  fprintf(stderr,
          "用法: %s -s <ip> -p <port> -t <threads> -c <conns> "
          "-n <requests> -b <payload_bytes> -P <pipeline>\n"
          "\n"
          "  -s 服务器 IP（默认 127.0.0.1）\n"
          "  -p 服务器端口（默认 9999）\n"
          "  -t 工作线程数（默认 1）\n"
          "  -c 总连接数，均分到各线程（默认等于 -t）\n"
          "  -n 总请求数，均分到各连接（默认 10000，必须 >= -c）\n"
          "  -b 每帧 payload 字节数（默认 64，必须 >= 4）\n"
          "  -P 每连接最大在途请求数（默认 1，范围 1..%d）\n"
          "\n"
          "示例:\n"
          "  %s -s 127.0.0.1 -p 9999 -t 4 -c 64 -n 200000 -b 1920 -P 4\n"
          "  %s -s 127.0.0.1 -p 9999 -t 2 -c 16  -n 20000  -b 8192 -P 16\n",
          prog, MAX_PIPELINE, prog, prog);
}

static int parse_int(const char *s, int *out) {
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
    return -1;
  }
  *out = (int)v;
  return 0;
}

static int parse_ll(const char *s, long long *out) {
  char *end = NULL;
  errno = 0;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return -1;
  }
  *out = v;
  return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/
int main(int argc, char **argv) {
  int opt;
  int ret = 0;

  /* 默认值 */
  snprintf(g_cfg.ip, sizeof(g_cfg.ip), "%s", "127.0.0.1");
  g_cfg.port     = 9999;
  g_cfg.threads  = 1;
  g_cfg.conns    = 0;       /* 0 表示默认等于 threads */
  g_cfg.requests = 10000;
  g_cfg.payload  = 64;
  g_cfg.pipeline = 1;

  while ((opt = getopt(argc, argv, "s:p:t:c:n:b:P:h")) != -1) {
    switch (opt) {
      case 's':
        snprintf(g_cfg.ip, sizeof(g_cfg.ip), "%s", optarg);
        break;
      case 'p': {
        int v = 0;
        if (parse_int(optarg, &v) < 0 || v <= 0 || v > 65535) {
          fprintf(stderr, "非法端口: %s\n", optarg);
          return 1;
        }
        g_cfg.port = (uint16_t)v;
        break;
      }
      case 't':
        if (parse_int(optarg, &g_cfg.threads) < 0 ||
            g_cfg.threads <= 0 || g_cfg.threads > MAX_WORKERS) {
          fprintf(stderr, "非法线程数: %s（1..%d）\n", optarg, MAX_WORKERS);
          return 1;
        }
        break;
      case 'c':
        if (parse_int(optarg, &g_cfg.conns) < 0 || g_cfg.conns <= 0) {
          fprintf(stderr, "非法连接数: %s\n", optarg);
          return 1;
        }
        break;
      case 'n':
        if (parse_ll(optarg, &g_cfg.requests) < 0 || g_cfg.requests <= 0) {
          fprintf(stderr, "非法请求数: %s\n", optarg);
          return 1;
        }
        break;
      case 'b':
        if (parse_int(optarg, &g_cfg.payload) < 0 ||
            g_cfg.payload < MIN_PAYLOAD || g_cfg.payload > MAX_PAYLOAD) {
          fprintf(stderr, "非法 payload 长度: %s（%d..%d）\n",
                  optarg, MIN_PAYLOAD, MAX_PAYLOAD);
          return 1;
        }
        break;
      case 'P':
        if (parse_int(optarg, &g_cfg.pipeline) < 0 ||
            g_cfg.pipeline <= 0 || g_cfg.pipeline > MAX_PIPELINE) {
          fprintf(stderr, "非法 pipeline: %s（1..%d）\n",
                  optarg, MAX_PIPELINE);
          return 1;
        }
        break;
      case 'h':
        usage(argv[0]);
        return 0;
      default:
        usage(argv[0]);
        return 1;
    }
  }

  if (g_cfg.conns == 0) {
    g_cfg.conns = g_cfg.threads;
  }

  if (g_cfg.conns < g_cfg.threads) {
    fprintf(stderr, "-c(%d) 必须 >= -t(%d)\n", g_cfg.conns, g_cfg.threads);
    return 1;
  }

  if (g_cfg.requests < g_cfg.conns) {
    fprintf(stderr, "-n(%lld) 必须 >= -c(%d)，否则会有连接分不到请求\n",
            g_cfg.requests, g_cfg.conns);
    return 1;
  }

  if (g_cfg.conns > 1000) {
    fprintf(stderr,
            "警告: echo_server 的 MAX_CONNS=1024，%d 个连接可能被服务端拒绝\n",
            g_cfg.conns);
  }

  memset(&g_server_addr, 0, sizeof(g_server_addr));
  g_server_addr.sin_family = AF_INET;
  g_server_addr.sin_port   = htons(g_cfg.port);
  if (inet_pton(AF_INET, g_cfg.ip, &g_server_addr.sin_addr) != 1) {
    fprintf(stderr, "非法服务器 IP: %s\n", g_cfg.ip);
    return 1;
  }

  /* 固定校验模板：第 0..3 字节每帧会被 seq 覆盖，第 4 字节起保持不变。 */
  g_payload_template = (unsigned char *)xmalloc((size_t)g_cfg.payload);
  for (int i = 0; i < g_cfg.payload; ++i) {
    /* 用无符号运算，避免 i*131 在 -b 接近 16 MiB 时触发有符号溢出 UB */
    g_payload_template[i] =
        (unsigned char)(((unsigned)i * 131u + 17u) & 0xffu);
  }

  signal(SIGPIPE, SIG_IGN);

  if (pthread_barrier_init(&g_barrier, NULL,
                           (unsigned)g_cfg.threads + 1) != 0) {
    fprintf(stderr, "pthread_barrier_init 失败\n");
    return 1;
  }

  /* 分配前先估算内存，避免 -b 16M -P 64 -c 1000 这类组合把机器打爆 */
  {
    size_t frame_size_est = (size_t)FRAME_HDR + (size_t)g_cfg.payload;
    double est_mib = (double)g_cfg.conns *
        ((double)g_cfg.pipeline * (double)frame_size_est +
         (double)frame_size_est + (double)RECV_SLACK +
         (double)g_cfg.pipeline * (double)sizeof(uint64_t) +
         (double)(sizeof(conn_t) + sizeof(struct pollfd) +
                  sizeof(conn_t *))) /
        (1024.0 * 1024.0);
    if (est_mib > 4096.0) {
      fprintf(stderr,
              "估算客户端缓冲 %.1f MiB 超过 4 GiB，请减小 -c / -P / -b\n",
              est_mib);
      pthread_barrier_destroy(&g_barrier);
      return 1;
    }
  }

  worker_t *workers = (worker_t *)xcalloc((size_t)g_cfg.threads,
                                          sizeof(worker_t));

  /* 把连接均分到各线程，并记录每个连接在全局的序号，用于分配请求数。 */
  int conn_base = g_cfg.conns / g_cfg.threads;
  int conn_rem  = g_cfg.conns % g_cfg.threads;
  int first_index = 0;

  for (int i = 0; i < g_cfg.threads; ++i) {
    worker_t *w = &workers[i];
    int nconns = conn_base + (i < conn_rem ? 1 : 0);

    memset(w, 0, sizeof(*w));
    w->id     = i;
    w->nconns = nconns;
    w->conns  = (conn_t *)xcalloc((size_t)nconns, sizeof(conn_t));
    w->pfds   = (struct pollfd *)xcalloc((size_t)nconns,
                                         sizeof(struct pollfd));
    w->map    = (conn_t **)xcalloc((size_t)nconns, sizeof(conn_t *));

    for (int j = 0; j < nconns; ++j) {
      conn_init(&w->conns[j], first_index + j);
      w->target_total += w->conns[j].target;
    }

    first_index += nconns;
  }

  /* 打印配置 */
  size_t frame_size = (size_t)FRAME_HDR + (size_t)g_cfg.payload;
  printf("==================== 压测配置 ====================\n");
  printf("服务器            : %s:%u\n", g_cfg.ip, (unsigned)g_cfg.port);
  printf("线程 / 连接       : %d / %d\n", g_cfg.threads, g_cfg.conns);
  printf("请求总数          : %lld（每连接约 %lld）\n",
         g_cfg.requests, g_cfg.requests / g_cfg.conns);
  printf("payload / 帧大小  : %d / %zu 字节\n", g_cfg.payload, frame_size);
  printf("每连接 pipeline   : %d\n", g_cfg.pipeline);
  printf("客户端缓冲估算    : %.1f MiB（所有连接发送队列 + 接收缓冲）\n",
         (double)g_cfg.conns *
         ((double)g_cfg.pipeline * (double)frame_size +
          (double)frame_size + (double)RECV_SLACK +
          (double)g_cfg.pipeline * (double)sizeof(uint64_t) +
          (double)(sizeof(conn_t) + sizeof(struct pollfd) +
                   sizeof(conn_t *)))
         / (1024.0 * 1024.0));
  printf("连接建立耗时不参与统计；成功判定 = 序号 + 长度 + 内容全部匹配\n");
  printf("==================================================\n\n");
  fflush(stdout);

  /* 创建线程 */
  for (int i = 0; i < g_cfg.threads; ++i) {
    int err = pthread_create(&workers[i].tid, NULL,
                             worker_entry, &workers[i]);
    if (err != 0) {
      fprintf(stderr, "pthread_create(%d) 失败: %s\n", i, strerror(err));
      exit(1);
    }
  }

  /* 等所有线程把连接建好，然后放行 */
  pthread_barrier_wait(&g_barrier);

  for (int i = 0; i < g_cfg.threads; ++i) {
    pthread_join(workers[i].tid, NULL);
  }

  /* 汇总统计：用最早开始时间和最晚结束时间计算总耗时 */
  long long ok = 0;
  long long conn_errors = 0;
  long long target_total = 0;
  long long partial_waits = 0;
  int       max_batch = 0;
  uint64_t rtt_sum = 0;
  uint64_t rtt_min = UINT64_MAX;
  uint64_t rtt_max = 0;
  int64_t  start_us = INT64_MAX;
  int64_t  end_us   = 0;

  for (int i = 0; i < g_cfg.threads; ++i) {
    worker_t *w = &workers[i];
    ok          += w->ok;
    conn_errors += w->conn_errors;
    target_total += w->target_total;
    partial_waits += w->partial_waits;
    if (w->max_batch > max_batch) {
      max_batch = w->max_batch;
    }
    rtt_sum     += w->rtt_sum;

    if (w->ok > 0 && w->rtt_min < rtt_min) {
      rtt_min = w->rtt_min;
    }
    if (w->rtt_max > rtt_max) {
      rtt_max = w->rtt_max;
    }
    if (w->start_us < start_us) {
      start_us = w->start_us;
    }
    if (w->end_us > end_us) {
      end_us = w->end_us;
    }
  }

  long long failed = target_total - ok;

  double elapsed = (end_us > start_us)
                   ? (double)(end_us - start_us) / 1000000.0 : 0.0;
  double qps = (elapsed > 0.0) ? (double)ok / elapsed : 0.0;
  double mbps = (elapsed > 0.0)
                ? ((double)ok * (double)frame_size * 2.0)
                  / (1024.0 * 1024.0) / elapsed
                : 0.0;
  double avg_rtt = (ok > 0) ? (double)rtt_sum / (double)ok : 0.0;

  printf("\n==================== 压测结果 ====================\n");
  printf("目标请求数        : %lld\n", target_total);
  printf("成功响应          : %lld\n", ok);
  printf("失败 / 未完成     : %lld\n", failed);
  printf("连接失败/异常     : %lld\n", conn_errors);
  printf("半帧等待次数      : %lld（>0 可确认发生过拆包；次数≠帧数）\n",
         partial_waits);
  printf("单次最多整帧数    : %d（>1 可确认发生过粘包）\n", max_batch);
  printf("耗时              : %.3f 秒\n", elapsed);
  printf("QPS               : %.1f 次/秒\n", qps);
  printf("吞吐              : %.2f MiB/s（收发合计）\n", mbps);

  if (ok > 0) {
    printf("RTT 平均/最小/最大: %.1f / %llu / %llu us\n",
           avg_rtt,
           (unsigned long long)rtt_min,
           (unsigned long long)rtt_max);
  } else {
    printf("RTT               : 无成功样本\n");
  }

  printf("\n---------------- 每线程统计 ----------------\n");
  for (int i = 0; i < g_cfg.threads; ++i) {
    worker_t *w = &workers[i];
    double sec = (w->end_us > w->start_us)
                 ? (double)(w->end_us - w->start_us) / 1000000.0 : 0.0;
    double tqps = (sec > 0.0) ? (double)w->ok / sec : 0.0;
    printf("thread %-3d 连接=%-4d 目标=%-10lld 成功=%-10lld "
           "失败=%-8lld 错误=%-4lld QPS=%.1f\n",
           w->id, w->nconns, w->target_total, w->ok,
           w->target_total - w->ok, w->conn_errors, tqps);
  }
  printf("==================================================\n");

  if (failed != 0) {
    ret = 1;
  }

  /* 清理（进程即将退出，这里的 free 主要用于示例完整性） */
  for (int i = 0; i < g_cfg.threads; ++i) {
    worker_t *w = &workers[i];
    for (int j = 0; j < w->nconns; ++j) {
      free(w->conns[j].sbuf);
      free(w->conns[j].rbuf);
      free(w->conns[j].ts);
    }
    free(w->conns);
    free(w->pfds);
    free(w->map);
  }
  free(workers);
  free(g_payload_template);
  pthread_barrier_destroy(&g_barrier);

  return ret;
}
