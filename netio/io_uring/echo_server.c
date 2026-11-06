/*
 * echo_server.c —— 基于 liburing 的 TCP 回显服务器（单次 shot 版本）
 *
 * 功能：
 *   监听 9999 端口，把客户端发来的数据原样发回去。
 *   accept / recv / send 全部通过 io_uring 异步提交，不使用阻塞式系统调用，
 *   也不依赖 epoll / select。
 *
 * 运行环境：
 *   - Linux 内核 >= 5.6
 *     （io_uring 本身从 5.1 开始；accept 从 5.5 开始；recv/send 从 5.6 开始）
 *   - 已构建的 liburing（本目录下的 liburing-master，版本 2.15）
 *
 * 编译（在 Linux / WSL2 中执行）：
 *   cd /path/to/cpp_internet_study/netio/io_uring
 *   (cd liburing-master && ./configure && make -C src -j"$(nproc)")
 *   gcc -O2 -Wall -Wextra -D_GNU_SOURCE \
 *       -I liburing-master/src/include \
 *       echo_server.c liburing-master/src/liburing.a \
 *       -o echo_server
 *
 * 运行：
 *   ./echo_server
 *   # 另一个终端：
 *   nc 127.0.0.1 9999
 *
 * 核心模型：
 *   - 每连接同一时刻最多只有一个在途操作（recv 或 send），
 *     因此每条连接一块固定 buffer 就足够，不需要复杂的 buffer 生命周期管理。
 *   - user_data 中编码「操作类型 + fd」，CQE 返回时据此分发到对应 handler。
 *   - accept 是 one-shot 的，每处理完一个 CQE 都必须重新提交。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>

#include <liburing.h>     /* io_uring_* 系列 API、struct io_uring / io_uring_sqe 等 */
#include <sys/socket.h>   /* socket / bind / listen / close、MSG_NOSIGNAL */
#include <arpa/inet.h>    /* sockaddr_in、htons / htonl、INADDR_ANY */

/* conns[] 数组容量，同时也是允许使用的 fd 上限（fd 直接作为数组下标） */
#define MAX_CONNS 1024
/* SQ ring 深度；CQ ring 默认是它的 2 倍 */
#define QD 64
/* listen() 的 backlog（全连接队列上限） */
#define LISTEN_BACKLOG 128
/* 每条连接的收 / 发缓冲区大小 */
#define BUFF_SIZE 4096
/* 监听端口 */
#define DEFAULT_PORT 9999


/*
 * user_data 高 32 位记录的操作类型，CQE 返回时据此决定调用哪个 handler。
 */
enum op_type {
  OP_ACCEPT = 1,  /* accept 完成：有新连接到达 */
  OP_READ,        /* recv 完成：读到数据 / EOF / 错误 */
  OP_WRITE        /* send 完成：发送完毕 / 短写 / 错误 */
};

/*
 * 每条连接的状态。
 *
 * conns[] 的下标就是文件描述符 fd，因此可以通过 conns[fd] 以 O(1) 找到连接，
 * 不需要哈希表（代价是 fd 必须小于 MAX_CONNS，见 close_conn / handle_accept）。
 */
struct conn {
  int fd;                 /* 真正的 socket fd；-1 表示该槽位空闲 */
  char buff[BUFF_SIZE];   /* 收 / 发共用缓冲区：send 完成后才会再次 recv，不会被覆盖 */
  size_t send_len;        /* 本次要回显的总字节数 */
  size_t send_off;        /* 已经成功发送的字节数，用于处理短写 */
};

static int listen_fd = -1;             /* 监听 socket */
static struct io_uring ring;           /* io_uring 实例：SQ / CQ 都挂在这上面 */
static struct conn conns[MAX_CONNS];   /* 以 fd 为下标的连接表 */


/*
 * 把「操作类型 + fd」打包进 64 位 user_data：
 *
 *   63            32 31             0
 *   +---------------+---------------+
 *   |    op_type    |      fd       |
 *   +---------------+---------------+
 *
 * 提交 SQE 时通过 io_uring_sqe_set_data64() 附带这个值，
 * 内核完成请求后会把它原样放进 CQE 的 user_data 字段。
 * 这是应用层把「完成事件」和「自己提交的请求」对应起来的唯一方式。
 */
static uint64_t encode_ud(enum op_type type, int fd) {
  return ((uint64_t)type << 32) | (uint64_t)(uint32_t)fd;
}

/* 从 user_data 中取出操作类型（高 32 位） */
static enum op_type decode_type(uint64_t ud) {
  return (enum op_type)(ud >> 32);
}

/* 从 user_data 中取出 fd（低 32 位） */
static int decode_fd(uint64_t ud) {
  return (int)(uint32_t)ud;
}

/*
 * 获取一个空闲 SQE。
 *
 * 当 SQ ring 已满时，io_uring_get_sqe() 返回 NULL。此时先把已经准备好的
 * SQE 提交给内核（内核消费后槽位就空出来了），然后再取一次。
 * 如果提交后仍然取不到，说明发生了异常，直接退出。
 */
static struct io_uring_sqe* get_sqe(void) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);

  if(!sqe) {
    io_uring_submit(&ring);
    sqe = io_uring_get_sqe(&ring);
  }

  if(!sqe) {
    fprintf(stderr, "SQ ring full\n");
    exit(1);
  }

  return sqe;
}

/*
 * 关闭连接，并把对应槽位标记为空闲。
 *
 * fd 会直接作为 conns[] 的下标使用，越界会踩内存，所以必须先做边界检查。
 * 注意边界是 fd >= MAX_CONNS（合法下标是 0 .. MAX_CONNS-1）。
 */
static void close_conn(int fd) {
  if(fd < 0 || fd >= MAX_CONNS) {
    return;
  }

  if(conns[fd].fd >= 0) {
    close(conns[fd].fd);
    conns[fd].fd = -1;
  }
}

/*
 * 提交一次 accept 请求。
 *
 * io_uring 的 accept 是 one-shot 的：一个 SQE 只对应一个新连接，
 * 因此每处理完一个 accept CQE 都要再次调用本函数，否则监听就停了。
 *
 * addr / addrlen 传 NULL 表示不关心对端地址，等价于 accept(fd, NULL, NULL)。
 */
static void add_accept(void) {
  struct io_uring_sqe *sqe = get_sqe();

  io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
  io_uring_sqe_set_data64(sqe, encode_ud(OP_ACCEPT, listen_fd));
}

/*
 * 为指定连接提交一次 recv。
 *
 * 缓冲区直接使用 conns[fd].buff。因为同一连接在 send 完成之前不会再次
 * 提交 recv，所以这块缓冲区不会被新收到的数据覆盖。
 */
static void add_recv(int fd) {
  struct io_uring_sqe *sqe = get_sqe();

  io_uring_prep_recv(sqe, conns[fd].fd, conns[fd].buff, sizeof(conns[fd].buff), 0);
  io_uring_sqe_set_data64(sqe, encode_ud(OP_READ, fd));
}

/*
 * 提交 send，把 conns[fd].buff[send_off, send_len) 这段数据发出去。
 *
 * - send_off 用于处理短写：如果上一次只发出去一部分，这次从断点继续。
 * - MSG_NOSIGNAL 让「对端已关闭」表现为 -EPIPE 错误，而不是 SIGPIPE 信号；
 *   主函数里还额外忽略了 SIGPIPE，双保险。
 */
static void add_send(int fd) {
  struct conn *c = &conns[fd];
  struct io_uring_sqe *sqe = get_sqe();

  io_uring_prep_send(sqe, fd, c->buff + c->send_off, c->send_len - c->send_off, MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe, encode_ud(OP_WRITE, fd));
}

/*
 * 处理 accept 的 CQE。
 *
 * cqe->res 的语义：
 *   >= 0 : 新连接的 fd
 *   <  0 : 负数 errno（例如 -EMFILE、-ECONNABORTED、-EINTR）
 *
 * 注意：io_uring 不使用 errno，错误码直接以负值放在 cqe->res 里。
 */
static void handle_accept(struct io_uring_cqe *cqe) {
  int fd = cqe->res;

  if(fd < 0) {
    /* EAGAIN / ECONNABORTED / EINTR 属于暂时性错误，不打印以免刷屏 */
    if(fd != -EAGAIN && fd != -ECONNABORTED && fd != -EINTR) {
      fprintf(stderr, "accept: %s\n", strerror(-fd));
    }

    /* one-shot accept：无论成功还是失败，都要重新 arm */
    add_accept();
    return;
  }

  /* fd 将直接作为 conns[] 的下标，超界就不能放进连接表 */
  if(fd >= MAX_CONNS) {
    fprintf(stderr, "fd %d 超出 MAX_CONNS\n", fd);
    close(fd);
    add_accept();
    return;
  }

  conns[fd].fd = fd;
  conns[fd].send_len = 0;
  conns[fd].send_off = 0;

  /* 新连接先挂一个 recv，等待客户端发数据 */
  add_recv(fd);
  /* 继续接收下一个新连接 */
  add_accept();
}

/*
 * 处理 recv 的 CQE。
 *
 * cqe->res 的语义：
 *   > 0 : 本次读到的字节数
 *   = 0 : 对端正常关闭连接（EOF）
 *   < 0 : 负数 errno
 */
static void handle_recv(struct io_uring_cqe *cqe) {
  int fd = decode_fd(cqe->user_data);
  int n = cqe->res;

  /* 防御性检查：连接可能已经被同一批 CQE 中更早的事件关闭 */
  if(fd < 0 || fd >= MAX_CONNS || conns[fd].fd < 0) {
    return;
  }

  if(n == 0) {
    /* 对端关闭（四次挥手完成），释放连接 */
    close_conn(fd);
    return;
  }

  if(n < 0) {
    fprintf(stderr, "recv fd %d: %s\n", fd, strerror(-n));
    close_conn(fd);
    return;
  }

  /*
   * 调试输出：只输出本次真正收到的 n 字节。
   *
   * 不能用 printf("%s", ...)：TCP 是字节流，数据不保证以 '\0' 结尾，
   * 而且 buff 里可能残留上一次收到的内容，直接当字符串打印会越界 / 输出脏数据。
   */
  fprintf(stdout, "recv %d bytes: ", n);
  fwrite(conns[fd].buff, 1, (size_t)n, stdout);
  fputc('\n', stdout);
  fflush(stdout);

  /* 记录要回显的数据长度，然后提交 send */
  conns[fd].send_len = (size_t)n;
  conns[fd].send_off = 0;

  add_send(fd);
}

/*
 * 处理 send 的 CQE。
 *
 * send 可能只发出去一部分（短写），所以用 send_off 记录进度：
 *   - 还没发完 -> 继续提交 send
 *   - 已经发完 -> 重新提交 recv，等待下一批数据
 *
 * 只有 send 全部完成后才会重新 recv，保证 conns[fd].buff 不会被新数据覆盖。
 */
static void handle_send(struct io_uring_cqe *cqe) {
  int fd = decode_fd(cqe->user_data);
  int n = cqe->res;

  /* 防御性检查：连接可能已被关闭 */
  if(fd < 0 || fd >= MAX_CONNS || conns[fd].fd < 0) {
    return;
  }

  if(n <= 0) {
    if(n < 0) {
      fprintf(stderr, "send fd %d: %s\n", fd, strerror(-n));
    }

    close_conn(fd);
    return;
  }

  conns[fd].send_off += (size_t)n;

  if(conns[fd].send_off < conns[fd].send_len) {
    /* 短写：缓冲区里还有数据没发完，继续提交 send */
    add_send(fd);
    return;
  }

  /* 本次回显完成，继续读这个连接 */
  add_recv(fd);
}

/*
 * 事件循环：
 *   1. 先常驻一个 accept 请求
 *   2. submit_and_wait 提交所有新 SQE，并等待至少 1 个完成事件
 *   3. 批量遍历 CQ ring 中的 CQE，按 user_data 的类型分发
 *   4. io_uring_cq_advance 统一推进 CQ head
 *
 * 注意 wait_nr 必须是 1，不能是 0：
 *   io_uring_submit_and_wait(&ring, 0) 只提交、不等待，没有完成事件时会
 *   立刻返回，导致循环空转、CPU 占用 100%（见 liburing src/queue.c 中
 *   __io_uring_submit 对 wait_nr 的判断）。
 */
static void event_loop(void) {
  add_accept();

  for(; ;) {
    int ret = io_uring_submit_and_wait(&ring, 1);

    if(ret < 0) {
      /*
       * -EINTR: 被信号打断，属于正常情况，重新进入等待即可。
       * -EAGAIN: 暂时没有事件，同样重试。
       * 其它错误视为致命错误，退出事件循环。
       */
      if(ret == -EINTR || ret == -EAGAIN) {
        continue;
      }

      fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-ret));
      return;
    }

    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned done = 0;

    /*
     * 批量遍历当前所有 CQE。
     *
     * 处理某个 CQE 时可能又提交了新的 SQE（例如 accept 完成后马上 add_recv），
     * 这是允许的；新的 SQE 会在下一轮 submit_and_wait 中提交给内核。
     */
    io_uring_for_each_cqe(&ring, head, cqe) {
      switch (decode_type(cqe->user_data)) {
        case OP_ACCEPT:
          handle_accept(cqe);
          break;
        case OP_READ:
          handle_recv(cqe);
          break;
        case OP_WRITE:
          handle_send(cqe);
          break;
        default:
          fprintf(stderr, "unknown user_data\n");
          break;
      }

      ++done;
    }

    /*
     * 告诉内核这一批 CQE 已经消费完了。
     * 必须调用，否则下一轮还会重复看到同一批完成事件。
     */
    io_uring_cq_advance(&ring, done);
  }
}

int main(void) {
  struct sockaddr_in addr;
  int on = 1;
  int ret;

  /*
   * 忽略 SIGPIPE：向已关闭的 socket 写数据时，Linux 默认会发送 SIGPIPE，
   * 进程可能直接被杀死。add_send 里还用了 MSG_NOSIGNAL，这里是双保险。
   */
  signal(SIGPIPE, SIG_IGN);

  /* 初始化连接表：fd = -1 表示该槽位空闲 */
  for(int i = 0; i < MAX_CONNS; ++i) {
    conns[i].fd = -1;
  }

  /*
   * 监听 socket 的创建仍然使用普通系统调用。
   * io_uring 接管的是 accept / recv / send 这些会阻塞的 IO 操作，
   * socket / bind / listen 本身没有必须异步化的理由。
   */
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if(listen_fd < 0) {
    perror("socket");
    return 1;
  }

  /*
   * SO_REUSEADDR：服务器重启时，即使旧连接处于 TIME_WAIT，
   * 也能立刻重新 bind 同一个端口。失败不致命，只打印告警。
   */
  if((setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0) {
    perror("setsockopt(SO_REUSEADDR)");
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DEFAULT_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    perror("bind");
    close(listen_fd);
    return 1;
  }

  if(listen(listen_fd, LISTEN_BACKLOG) == -1) {
    perror("listen");
    close(listen_fd);
    return 1;
  }

  /*
   * 初始化 io_uring 实例：
   *   - QD 是 SQ ring 的深度；CQ ring 默认是 2 倍
   *   - flags 传 0，使用默认配置（单线程提交足够）
   *   - 返回值 < 0 时是负数 errno，例如 -ENOSYS（内核不支持）、
   *     -EPERM（被 seccomp / 容器策略禁止）、-ENOMEM 等
   */
  ret = io_uring_queue_init(QD, &ring, 0);
  if(ret < 0) {
    fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
    close(listen_fd);
    return 1;
  }

  printf("io_uring echo server listening on port %d\n", DEFAULT_PORT);
  fflush(stdout);

  /* 进入事件循环；正常情况不会返回，Ctrl-C 结束进程 */
  event_loop();

  /* 只有事件循环因致命错误退出时才会执行到这里 */
  io_uring_queue_exit(&ring);
  close(listen_fd);
  return 0;
}
