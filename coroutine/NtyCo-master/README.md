# NtyCo —— 基于 ucontext + epoll 的 C 协程网络框架

> **目录性质**：本目录是 [WangBoJing/NtyCo](https://github.com/wangbojing/NtyCo) 的 master 源码快照，
> 作为 [`coroutine/`](../) 中自写 C++17 协程框架的**参考实现**保留，不是本仓库原创代码。
> 原作者：WangBoJing；版权声明见各源文件头部，仓库未附带独立 LICENSE 文件。
>
> - 实现原理：<https://github.com/wangbojing/NtyCo/wiki/NtyCo%E7%9A%84%E5%AE%9E%E7%8E%B0>
> - 配套视频：<https://it.0voice.com/p/t_pc/goods_pc_detail/goods_detail/course_2QFAeORw45TjJA1y9tq8CmdVJTQ>
> - 作者讲解（旧）：<https://ke.qq.com/course/2705727?tuin=1bf84273>

NtyCo 要解决的核心问题与自写框架相同：**业务代码按同步顺序写，底层 socket 全程非阻塞**。
它用 `ucontext` 保存 / 恢复协程上下文，用 `epoll` 在 I/O 未就绪时挂起协程；不同之处在于它默认
通过 `dlsym(RTLD_NEXT, ...)` **劫持 libc 的 `socket` / `read` / `write` / `accept` / `connect` 等符号**，
让原本同步阻塞的代码也自动接入调度器。

---

## 快速开始

```bash
cd coroutine/NtyCo-master

make          # 编译 core：生成 objs/*.o 与静态库 libntyco.a
make bin      # 编译 sample：生成 bin/ 下的全部示例
```

`make bin` 会编译**所有**示例，其中 MySQL / Redis / WebSocket 示例依赖第三方开发库：

```bash
# Debian / Ubuntu
sudo apt-get install build-essential libssl-dev libmysqlclient-dev

# hiredis 一般需要从源码编译安装
git clone https://github.com/redis/hiredis
cd hiredis && make && sudo make install && sudo ldconfig
```

只想跑基础 echo / HTTP 示例时，可以只链接需要的目标（`make` 已创建 `bin/` 与 `objs/`）：

```bash
make
make nty_server nty_client
```

> 构建细节：`make bin` 会先进入 `sample/` 把所有 `.c` 编译成对象文件，这一步只使用顶层
> 导出的 `FLAG`（含 `-I core`），**不带** `THIRDFLAG` 里的 `/usr/include/mysql` 等头文件路径。
> 如果 `mysql.h` 不在编译器默认搜索路径，`make bin` 仍会报找不到头文件；可以先
> `export CPATH=/usr/include/mysql`，或者跳过 `make bin`，直接 `make nty_mysql_oper`
> （顶层单个目标的规则会同时带上 `FLAG` 和 `THIRDFLAG`）。

### 常见编译错误

```text
nty_mysql_oper.c:8:19: fatal error: mysql.h: No such file or directory
解决方案：
# sudo apt-get install libmysqlclient-dev

nty_rediscli.c:11:21: fatal error: hiredis.h: No such file or directory
解决方案：
需要编译安装 hiredis: https://github.com/redis/hiredis
```

> `nty_websocket_server.c` 需要 OpenSSL 头文件（`libssl-dev`）。`deps/openssl-1.1.0g.tar.gz`
> 只是随源码附带的 OpenSSL 源码包，Makefile 不会自动编译它。

---

## 目录结构

```text
NtyCo-master/
├── core/                        协程核心（编译为 libntyco.a）
│   ├── nty_coroutine.h/.c       协程对象、状态机、创建 / resume / yield
│   ├── nty_schedule.c           调度器：就绪队列、睡眠红黑树、等待红黑树、主循环
│   ├── nty_epoll.c              epoll 封装与 eventfd 注册
│   ├── nty_socket.c             nty_* I/O API + libc 同名函数 hook
│   ├── nty_queue.h              BSD 队列 / 链表宏（TAILQ、LIST）
│   ├── nty_tree.h               BSD 红黑树宏（RB_*）
│   └── Makefile / CMakeLists.txt
├── sample/                      示例程序（echo、HTTP、WebSocket、MySQL、Redis、压测）
├── htdocs/                      nty_http_server 的静态页面根目录
├── websocket/                   浏览器端 WebSocket 测试页
├── deps/                        openssl-1.1.0g.tar.gz（源码包，未自动构建）
├── Makefile                     顶层构建脚本
└── README.md
```

---

## 架构与核心机制

### 1. 调度器 `nty_schedule`

每个线程拥有一个调度器，通过 `pthread_key_t global_sched_key` 存取；
`nty_coroutine_create()` 发现当前线程还没有调度器时，会自动调用 `nty_schedule_create(0)`。

```text
nty_schedule
├── ready      TAILQ   就绪协程队列
├── sleeping   RB tree 按 sleep_usecs 排序的睡眠协程
├── waiting    RB tree 按 fd 排序的 I/O 等待协程
├── defer/busy 预留字段（当前核心代码未使用）
├── poller_fd  epoll 实例
└── eventlist  struct epoll_event[NTY_CO_MAX_EVENTS]（默认 1024*1024，约 12 MB）
```

主循环 `nty_schedule_run()` 在「ready / sleeping / waiting / busy 全部为空」时退出，
并释放调度器：

1. 从 sleeping 红黑树取出所有**已到期**的协程，逐个 `resume`；
2. 依次执行 ready 队列里的协程（用 `last_co_ready` 限定本轮范围，避免新就绪协程饿死 epoll）；
3. 根据 sleeping 树的最小超时计算 `epoll_wait` 超时；事件到达后按 fd 在 waiting 树中找到
   对应协程并 `resume`。

**定时器**：`nty_coroutine_sleep(msecs)` 把协程按到期时间插入 sleeping 树；
`nty_schedule_min_timeout()` 用最近的到期时间驱动 `epoll_wait` 超时。
I/O 等待（`nty_schedule_sched_wait`）会同时把协程插入 waiting 树和 sleeping 树，
超时后由主循环唤醒，协程在 `nty_poll_inner` 返回后清理 epoll 注册和等待项。

> 注意：`nty_schedule_sched_wait()` 里 `timeout == 1` 是「**不设置超时**」的哨兵值，
> 不是 1 毫秒。`nty_recv` / `nty_send` / `nty_accept` / `nty_connect` 内部都传 1。

### 2. 协程与状态机

```text
NEW --首次 resume--> READY --调度--> RUNNING --yield / sleep / wait fd--> READY / SLEEPING / WAIT_*
                                          |
                                          +--函数返回--> EXITED（自动 detach + free）
```

![coroutine FSM](http://bojing.wang/wp-content/uploads/2018/08/status_machine.png)

- `nty_coroutine_create()`：分配 `nty_coroutine`，状态置为 `NEW`，插入 ready 队列。
- 首次 `resume` 时调用 `getcontext` / `makecontext` 初始化上下文，入口为 `_exec`，
  由它调用 `co->func(co->arg)`。
- 协程函数返回后，`_exec` 打上 `EXITED | FDEOF | DETACH` 并 `yield`；
  `nty_coroutine_resume()` 看到 `EXITED + DETACH` 就调用 `nty_coroutine_free()`。
  因此协程是**自动 detach** 的，没有 join 语义（`co_join`、`co_exit_ptr` 字段目前未使用）。
- `status` 是位掩码，枚举值表示**位号**，绝大多数代码用 `BIT(status)` 操作。

![storage structure (ready, wait, sleep, status)](http://bojing.wang/wp-content/uploads/2018/08/6.1.png)

### 3. 共享栈：`_USE_UCONTEXT`（默认路径）

`nty_coroutine.h` 默认定义 `_USE_UCONTEXT`。这条路径下：

- 所有协程**共享调度器的同一块栈**（`sched->stack`，默认 128 KiB，`NTY_CO_MAX_STACKSIZE`）；
- `yield` 时 `_save_stack()` 把从栈顶到当前栈帧的字节 `memcpy` 到协程自己的堆内存；
- `resume` 时 `_load_stack()` 再拷回共享栈，然后 `swapcontext`。

优点是省内存（每个协程只保存实际用到的栈字节），代价是**每次切换都有一次 memcpy**，
且单协程栈深不能超过 128 KiB，没有 guard page。

关闭 `_USE_UCONTEXT` 会走另一条实现：每个协程 `posix_memalign` 独立栈，用 x86 / x86_64
汇编 `_switch` 切换寄存器，并用 `madvise(MADV_DONTNEED)` 回收未使用的栈页。
这条路径只支持 x86 架构，默认不启用。

### 4. I/O 包装与 libc hook

接入 I/O 的方式有三种：

| 方式 | 用法 | 示例 |
|------|------|------|
| 显式 API | 直接调用 `nty_socket` / `nty_accept` / `nty_recv` / `nty_send` / `nty_connect` | `nty_server.c`、`nty_client.c`、`ntyco_httpd.c` |
| 宏替换 | `#define socket nty_socket` 等，把原代码整体换掉 | `nty_http_server.c`、`nty_bench.c` |
| 符号 hook | 库导出与 libc 同名的 `socket` / `read` / `write` / `recv` / `send` / `accept` / `connect` / `close`，内部用 `dlsym(RTLD_NEXT, ...)` 取原始函数 | `hook_tcpserver.c`、`nty_mysql_client.c` |

hook 的行为：

- 当前线程绑定了调度器时，走协程逻辑（非阻塞 + `epoll` + `yield`）；
- 当前线程没有调度器时，直接调用 `dlsym` 拿到的原始 libc 函数，不影响普通线程；
- `init_hook()` 负责初始化函数指针，首次调用 hook 函数时也会惰性初始化。

`nty_poll_inner()` 是 I/O 等待的核心：把 fd 注册进 epoll、插入 waiting 树、`yield`；
事件到达或超时后，协程继续执行，再删除 epoll 注册和 waiting 项。
`nty_send` / `write` / `sendto` 会循环发送直到写完或出错。

> hook 是**进程级**的：把 `libntyco.a` 链接进程序并引用其中符号后，就会导出这些同名符号。
> 不需要 hook 时，可以只用 `nty_*` 显式 API；需要 hook 时记得链接 `-ldl`。

### 5. 多进程示例

核心调度器是**单线程**的，多核靠示例里的多进程实现：

- `ntyco_httpd.c`：父进程先创建 **listen fd** 再 `fork`，子进程继承同一个 fd；
  用共享内存 + `cmpxchg` 实现的 `accept_mtx` 串行化 `accept`，并用 `sched_setaffinity`
  把进程绑定到 CPU。
- `nty_server_mulcore.c` / `nty_http_server_mulcore.c`：`fork` 出多个进程，每个进程
  创建自己的调度器和监听 socket；共享内存里维护连接数 / 时间统计。
  这两份示例各进程分别 `bind` 同一端口，只设置了 `SO_REUSEADDR`；在 Linux 上让多个
  进程同时监听同一端口通常需要 `SO_REUSEPORT`，且代码没有检查 `bind` / `listen` 返回值，
  实际行为请以运行结果为准。
- `nty_http_epoll.c`：`ENABLE_NTYCO = 0` 的**纯 epoll 对照组**，同样使用 fork + 共享内存
  锁 + `EPOLLONESHOT`，用来和协程版本对比。它不在顶层 Makefile 的 `BIN` 列表中，
  不会被链接成可执行文件。

---

## 核心 API

| API | 说明 |
|-----|------|
| `int nty_coroutine_create(nty_coroutine **co, proc_coroutine func, void *arg)` | 创建协程并放入 ready 队列 |
| `void nty_schedule_run(void)` | 启动当前线程的调度循环；所有任务结束后返回 |
| `void nty_coroutine_sleep(uint64_t msecs)` | 睡眠；`0` 表示主动让出一次 |
| `void nty_coroutine_yield(nty_coroutine *co)` | 让出当前协程 |
| `int nty_socket(int domain, int type, int protocol)` | 创建非阻塞 socket，并设置 `SO_REUSEADDR` |
| `int nty_accept(int fd, struct sockaddr *addr, socklen_t *len)` | 协程化 accept |
| `int nty_connect(int fd, struct sockaddr *name, socklen_t namelen)` | 协程化 connect |
| `ssize_t nty_recv / nty_send / nty_recvfrom / nty_sendto` | 协程化收发 |
| `int nty_close(int fd)` | 关闭 fd（当前实现直接 `close`） |
| `void init_hook(void)` | 初始化 libc hook 的函数指针 |
| `int nty_poll(struct pollfd *fds, nfds_t nfds, int timeout)` | **仅声明，未实现**（内部使用 `nty_poll_inner`） |

最小示例（协程 echo 服务器，摘自 `sample/nty_server.c`）：

```c
#include "nty_coroutine.h"

void server_reader(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    while (1) {
        char buf[1024] = {0};
        int ret = nty_recv(fd, buf, sizeof(buf), 0);
        if (ret > 0) {
            nty_send(fd, buf, strlen(buf), 0);
        } else if (ret == 0) {
            nty_close(fd);
            break;
        }
    }
}

void server(void *arg) {
    unsigned short port = *(unsigned short *)arg;
    free(arg);

    int fd = nty_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr *)&local, sizeof(local));
    listen(fd, 20);

    while (1) {
        socklen_t len = sizeof(struct sockaddr_in);
        int cli_fd = nty_accept(fd, NULL, &len);

        int *arg = malloc(sizeof(int));
        *arg = cli_fd;
        nty_coroutine *co = NULL;
        nty_coroutine_create(&co, server_reader, arg);
    }
}

int main(void) {
    unsigned short *port = calloc(1, sizeof(unsigned short));
    *port = 9096;

    nty_coroutine *co = NULL;
    nty_coroutine_create(&co, server, port);
    nty_schedule_run();
    return 0;
}
```

---

## 示例程序一览

| 程序 | 端口 / 地址 | 说明 | 额外依赖 |
|------|-------------|------|----------|
| `nty_server` | 9096–9195（100 个端口） | 每个端口一个 `server` 协程，每个连接一个 `server_reader` 协程，echo | 无 |
| `nty_client` | `127.0.0.1:9096` | 每秒发送一次 `ntyco_client\r\n`，可用于压 `nty_server` | 无 |
| `nty_server_mulcore` | 9096 | `fork()` 两次得到 4 个进程；每进程一个调度器 + CPU 亲和；共享内存统计连接数 | 无（Linux `shm` / `sched`） |
| `hook_tcpserver` | 命令行参数指定端口 | 使用被 hook 的 `socket` / `accept` / `recv` / `send`，echo | 无；运行需传端口，如 `./bin/hook_tcpserver 9096` |
| `nty_http_server` | 4000 | 由 J. David's webserver 改造：解析 GET/POST、静态文件服务；保留了 CGI 代码但当前快照里 `cgi = 0` 被强制关闭 | 无 |
| `nty_http_server_mulcore` | 9096 | 多进程 HTTP 示例，每进程独立监听，共享内存统计 | 无 |
| `ntyco_httpd` | 9001 | 父进程 listen 后 fork，子进程继承 listen fd；共享内存 CAS 锁串行化 accept；返回固定的 nginx 欢迎页 | 无 |
| `nty_http_epoll` | 9000 | 纯 epoll 多进程对照组（`ENABLE_NTYCO = 0`），使用 `EPOLLONESHOT`；顶层 Makefile 不链接它 | 无 |
| `nty_websocket_server` | 9096 | WebSocket 握手（`Sec-WebSocket-Key` + GUID → SHA1 → Base64）与帧编解码 echo | OpenSSL（`-lcrypto -lssl`） |
| `nty_bench` | 目标 URL | 由 webbench 改造的协程压测客户端：`-c` 进程数、`-s` 并发 socket 数、`-n` 总请求数、`-t` 秒数 | 无 |
| `nty_mysql_client` | `192.168.233.133:3306` | 在协程中调用 `init_hook()` + `mysql_real_connect()`，演示 hook 接管 mysqlclient 的内部 socket | libmysqlclient |
| `nty_mysql_oper` | 同上 | MySQL CRUD 与 BLOB 图片读写（预处理语句） | libmysqlclient |
| `nty_rediscli` | `192.168.233.133:6379` | hiredis 客户端：PING / SET / GET / INCR / LPUSH / LRANGE | hiredis |
| `htdocs/` | — | `nty_http_server` 的静态页面根目录（`index.html`、`color.cgi`、`check.cgi`） | — |
| `websocket/websocket_client.html` | `ws://192.168.189.128:9096/` | 浏览器端 WebSocket 测试页，IP 需按实际服务器修改 | — |

> MySQL / Redis / WebSocket 示例里的 IP、账号、密码都是作者实验环境的固定值，
> 运行前需要改成自己的环境。`nty_http_server` 的静态路径拼成 `htdocs/...`，
> 因此要在 `NtyCo-master` 目录下启动它。

---

## 运行

```bash
# echo 服务器（监听 9096–9195）
./bin/nty_server

# echo 客户端（连接 127.0.0.1:9096）
./bin/nty_client

# 多进程 echo
./bin/nty_server_mulcore

# WebSocket echo（9096）
./bin/nty_websocket_server

# HTTP 服务器（4000，静态根目录 htdocs/）
./bin/nty_http_server
# 或固定的 nginx 欢迎页（9001）
./bin/ntyco_httpd
# 或多进程 HTTP（9096）
./bin/nty_http_server_mulcore
```

压测示例（以 `nty_http_server` 的 4000 端口为例）：

```bash
# 4 个进程，共 400 个并发 socket，总共 100000 个请求，最长 10 秒
./bin/nty_bench -c 4 -s 400 -n 100000 -t 10 http://127.0.0.1:4000/index.html
```

`nty_bench` 的约束：`-c` 最大 10；`-n` 小于 1000 会被抬到 1000；
`-s` 会被向下取整为 `-c` 的整数倍；每个子进程创建 `s/c` 个协程，每个协程发送 `n/s` 个请求。

### 原作者压测数据（图片）

`nty_bench` 对比 nginx：

![nty_bench ntyco](http://bojing.wang/wp-content/uploads/2018/08/nty_bench_ntyco.png)
![nty_bench nginx](http://bojing.wang/wp-content/uploads/2018/08/nty_bench_nginx.png)

HTTP server 的 `ab` 对比：

![ntyco ab](http://bojing.wang/wp-content/uploads/2018/08/ntyco_ab.png)
![nginx ab](http://bojing.wang/wp-content/uploads/2018/08/nginx_ab.png)

### `nty_server` 流程（作者 UML）

![nty_server process](https://cos.0voice.com/nty_server_uml.png)

---

## 代码阅读提示

- **先看 `nty_coroutine.h`**：`nty_schedule` 和 `nty_coroutine` 两个结构体把「调度器持有什么」
  和「协程持有什么」划分得很清楚；状态枚举、`BIT()` 宏、`io` 结构体也在里面。
- **再看 `nty_schedule.c` 的 `nty_schedule_run()`**：5 个数据结构如何协作，主循环是最直观的入口。
- **然后看 `nty_socket.c` 的 `nty_poll_inner()`**：这是「同步写法 + 非阻塞 I/O」的关键胶水。
- **最后看 hook 部分**：同名函数如何用 `dlsym(RTLD_NEXT, ...)` 拿回原始实现，以及没有调度器时如何透传。
- `nty_queue.h` / `nty_tree.h` 是 BSD 风格的宏实现（`TAILQ_*`、`RB_*`），不需要逐行读，
  知道它们提供什么操作即可。
- 这份代码是**教学 / 示例性质**，不是生产级实现。阅读时会遇到一些粗糙之处，例如：
  - `nty_schedule_sched_wait()` 里有两处把状态枚举值直接 `|=` 进 `status`（没有用 `BIT()`），
    与后续的 `BIT(...)` 检查不一致；
  - `nty_schedule_search_wait()` 对查找结果没有判空（当前代码里 eventfd 从未被写入，所以不会触发）；
  - `nty_socket()` / hook `socket()` 在 `fcntl` 失败时执行 `close(ret)`，而 `ret` 是 `-1`；
  - 多处 `bind` / `listen` / `accept` 的返回值未检查；
  - 调度器结构里的 `defer`、`busy`、`compute_sched`、`nfds` 等字段是预留的，核心代码尚未实现。
- `nty_coroutine_renice()`、`nty_coroutine_detach()` 已实现但示例中没有使用。

---

## 已知限制

- **仅支持 Linux**：依赖 `ucontext`、`epoll`、`eventfd`、`dlsym(RTLD_NEXT)`、System V 共享内存、
  `sched_setaffinity` 等，Windows / MSVC 无法编译。
- **单线程调度**：一个线程一个调度器，没有跨线程唤醒、work stealing 或线程池；
  多核只能像示例那样 fork 多进程。
- **共享栈上限 128 KiB**，`memcpy` 切换开销随栈使用量增长；无 guard page，递归过深会踩栈。
- **无 join / 取消语义**：协程结束时自动 detach 并释放；`CANCELLED`、`co_join` 等字段未实现。
- **hook 影响整个进程**：链接 `libntyco.a` 就会导出 libc 同名符号；文件 I/O 的 `read` / `write`
  也会被拦截（有调度器时会走 epoll 等待），不适合直接用于普通文件 I/O 密集的程序。
- 调度器的 `eventlist` 固定为 1024×1024 个事件，每个调度器约占 12 MB。
- 示例中的错误处理、端口复用、配置（IP / 账号）都比较粗糙，仅用于学习验证。

---

## 原项目资料

- 原仓库：<https://github.com/wangbojing/NtyCo>
- 实现原理 wiki：<https://github.com/wangbojing/NtyCo/wiki/NtyCo%E7%9A%84%E5%AE%9E%E7%8E%B0>
- 配套视频：<https://it.0voice.com/p/t_pc/goods_pc_detail/goods_detail/course_2QFAeORw45TjJA1y9tq8CmdVJTQ>
- 旧版视频：<https://ke.qq.com/course/2705727?tuin=1bf84273>
