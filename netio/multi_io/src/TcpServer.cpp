#include "net/TcpServer.h"

#include <cstdio>
#include <cstring>

namespace net {

namespace {
#ifdef MSG_NOSIGNAL
// Linux：向已被对端关闭的连接写数据时，用这个标志可以让 send 返回 EPIPE
// 而不是给进程发 SIGPIPE 信号（默认行为会直接杀死进程）
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
}  // namespace

TcpServer::TcpServer(Options opts) : opts_(opts) {
    initNetwork();
    ignoreSigPipe();
    poller_ = EventPoller::create(opts_.backend, opts_.maxEvents, opts_.edgeTriggered);
}

TcpServer::~TcpServer() {
    // 关闭所有连接。这里不需要调用 poller_->del()，因为 poller_ 紧接着就会被销毁。
    for (const auto& kv : conns_) {
        closeSocket(kv.first);
    }
    conns_.clear();

    if (listenFd_ != kInvalidSocket) {
        closeSocket(listenFd_);
        listenFd_ = kInvalidSocket;
    }
}

// ---------------------------------------------------------------------------
//  启动：socket -> bind -> listen -> 注册到多路复用器
// ---------------------------------------------------------------------------
bool TcpServer::listen(const std::string& ip, uint16_t port) {
    if (!poller_) {
        std::fprintf(stderr,
                     "[TcpServer] 无法创建多路复用器：当前平台不支持所选后端（poll/epoll 仅 Linux 可用）\n");
        return false;
    }

    std::string err;
    listenFd_ = createListener(ip, port, opts_.backlog, &err);
    if (listenFd_ == kInvalidSocket) {
        std::fprintf(stderr, "[TcpServer] 监听失败: %s\n", err.c_str());
        return false;
    }

    // 监听套接字也设为非阻塞：accept 才需要循环到 EAGAIN
    setNonBlocking(listenFd_, true);

    // 监听 fd 只关心可读事件（可读 == 有新连接到来）
    if (!poller_->add(listenFd_, ev::Read)) {
        std::fprintf(stderr, "[TcpServer] 注册监听 fd 失败: %s\n", poller_->lastError().c_str());
        return false;
    }

    running_ = true;
    return true;
}

// ---------------------------------------------------------------------------
//  一轮事件循环
// ---------------------------------------------------------------------------
void TcpServer::loopOnce(int timeoutMs) {
    if (!poller_ || !running_) {
        return;
    }

    std::vector<Event> ready;
    const int n = poller_->wait(ready, timeoutMs);
    if (n < 0) {
        std::fprintf(stderr, "[TcpServer] wait 失败: %s\n", poller_->lastError().c_str());
        return;
    }
    if (n == 0) {
        return;  // 超时，什么也没发生 —— 真实项目会在这里跑定时器
    }

    for (const Event& e : ready) {
        // 监听 fd：新连接
        if (e.fd == listenFd_) {
            handleAccept();
            continue;
        }

        // 可能在处理前面的同一个事件时就被关掉了
        if (conns_.find(e.fd) == conns_.end()) {
            continue;
        }

        // 出错：直接关。注意 Hup / Rdhup 不在这里处理 —— 对端半关闭时
        // 可能还有最后一批数据没读完，要交给 handleRead 读到 0 再关。
        if ((e.events & ev::Error) != 0) {
            closeConnection(e.fd);
            continue;
        }

        if ((e.events & ev::Write) != 0) {
            if (!handleWrite(e.fd)) {
                continue;  // 连接已关闭
            }
        }

        // handleWrite 内部的 onWriteComplete 回调有可能把连接关掉
        // （"发完就关"就是这个机制），而它返回的却是 true。
        // 所以这里必须重新确认一次连接还在 ——
        // 否则下面的 handleRead 会去 recv 一个已经关闭、并且可能**已经被
        // 内核回收给新连接复用**的 fd，那读到的就是别的连接的数据了。
        if (conns_.find(e.fd) == conns_.end()) {
            continue;
        }

        if ((e.events & (ev::Read | ev::Hup | ev::Rdhup)) != 0) {
            handleRead(e.fd);
        }
    }
}

void TcpServer::loop() {
    while (running_) {
        loopOnce(-1);
    }
}

// ---------------------------------------------------------------------------
//  接受新连接
// ---------------------------------------------------------------------------
void TcpServer::handleAccept() {
    // 必须循环 accept 直到 EAGAIN：
    //   LT 模式下不循环也能工作（下次还会通知），但会多一次系统调用往返；
    //   ET 模式下不循环就是 bug —— 一次通知可能对应多个积压的连接，
    //   没取走的那些在新连接到来之前再也不会被通知。
    for (;;) {
        // addr 和 addrlen 必须同时为 NULL，或同时指向有效的缓冲区。
        // 传 addr == NULL 却让 addrlen != NULL，Winsock 会返回 WSAEFAULT(10014)
        // “无效指针地址”；Linux 虽然宽容些，但规范同样要求成对出现。
        // 另外注意：这里如果拿到的是一个持续失败的错误（如 EMFILE）而直接 break，
        // LT 模式下监听 fd 依然可读，事件循环会立刻再次进来，变成 100% CPU 的空转 ——
        // 真实项目要么用“预留 fd”技巧，要么在此处短暂停止监听。
        socket_t fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd == kInvalidSocket) {
            const int err = socketError();
            if (isWouldBlock(err)) {
                break;  // 积压的连接已经取完了
            }
            if (isInterrupted(err)) {
                continue;
            }
#ifdef _WIN32
            if (err == WSAECONNRESET) {
                continue;  // 客户端在三次握手后立刻断开，忽略继续 accept
            }
#else
            if (err == ECONNABORTED) {
                continue;
            }
            if (err == EMFILE || err == ENFILE) {
                // 进程 fd 耗尽。工程做法是预留一个空闲 fd：
                // 先关掉它腾出位置 -> accept -> 立刻 close 新连接 -> 把预留 fd 重新打开，
                // 目的是让客户端立刻收到 RST，而不是一直挂在半开状态等超时。
                std::fprintf(stderr, "[TcpServer] fd 耗尽，无法接受新连接: %s\n", errorString(err).c_str());
                break;
            }
#endif
            std::fprintf(stderr, "[TcpServer] accept: %s\n", errorString(err).c_str());
            break;
        }

        // 非阻塞：ET 模式下这是硬性前提，否则最后一次 recv 会把线程永久挂住
        setNonBlocking(fd, true);
        setTcpNoDelay(fd, true);

        Connection c;
        c.fd = fd;
        conns_.emplace(fd, std::move(c));

        // 新连接只关注可读。可写事件是“按需关注”的：
        // 只有应用层还有数据待发时才打开，数据发完立刻关掉。
        if (!poller_->add(fd, ev::Read)) {
            std::fprintf(stderr, "[TcpServer] 注册连接 fd 失败: %s\n", poller_->lastError().c_str());
            conns_.erase(fd);
            closeSocket(fd);
            continue;
        }

        if (onConnect_) {
            onConnect_(fd);  // 回调里可能调用 closeConnection，所以下面不能再碰 fd 的状态
        }
    }
}

// ---------------------------------------------------------------------------
//  读数据。返回 false 表示连接已经不存在了。
// ---------------------------------------------------------------------------
bool TcpServer::handleRead(socket_t fd) {
    std::vector<char> buf(opts_.readChunk);

    // 统一按 ET 语义实现：一直读到 EAGAIN 为止。
    // 这样同一份代码在 LT 和 ET 下都正确，也是工程中的通用写法。
    // （ET 下不读到 EAGAIN，剩余数据就永远收不到通知了。）
    for (;;) {
        const int n = ::recv(fd, buf.data(), (int)buf.size(), 0);

        if (n > 0) {
            if (onMessage_) {
                onMessage_(fd, buf.data(), (size_t)n);
            }
            // 回调里可能调用了 closeConnection，此处的 fd 已失效，必须重新确认
            if (conns_.find(fd) == conns_.end()) {
                return false;
            }
            continue;
        }

        if (n == 0) {
            // 对端发送了 FIN，正常关闭
            closeConnection(fd);
            return false;
        }

        const int err = socketError();
        if (isInterrupted(err)) {
            continue;
        }
        if (isWouldBlock(err)) {
            return true;  // 数据读干净了，这是正常退出路径
        }
        closeConnection(fd);  // 真正的错误
        return false;
    }
}

// ---------------------------------------------------------------------------
//  处理可写事件：把应用层缓冲区里剩下的数据尽量发出去。
//  返回 false 表示连接已经不存在了。
// ---------------------------------------------------------------------------
bool TcpServer::handleWrite(socket_t fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return false;
    }
    Connection& c = it->second;

    while (c.outSent < c.outBuf.size()) {
        const int n = ::send(fd, c.outBuf.data() + c.outSent, (int)(c.outBuf.size() - c.outSent), kSendFlags);
        if (n > 0) {
            c.outSent += (size_t)n;
            continue;
        }
        const int err = socketError();
        if (isInterrupted(err)) {
            continue;
        }
        if (isWouldBlock(err)) {
            break;  // 内核发送缓冲区又满了，等下一次可写事件
        }
        closeConnection(fd);
        return false;
    }

    if (c.outSent >= c.outBuf.size()) {
        // 记住"这次是从有数据变成没数据"，还是本来就空着。
        // 只有前者才算真正的"写完了"，才值得通知上层。
        const bool hadPending = !c.outBuf.empty();

        c.outBuf.clear();
        c.outSent = 0;
        updateInterest(fd);  // 取消对可写事件的关注，否则 LT 下会 100% 空转

        // 回调放在最后，而且之后绝不能再碰 c。
        // 因为回调里很可能调用 closeConnection()，那会把这个 map 项整个删掉，
        // 于是 c 就成了悬垂引用 —— 再用一下就是随机崩溃。
        if (hadPending && onWriteComplete_) {
            onWriteComplete_(fd);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  应用层发送缓冲区是否已排空
// ---------------------------------------------------------------------------
bool TcpServer::sendBufferEmpty(socket_t fd) const {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return false;
    }
    return it->second.outSent >= it->second.outBuf.size();
}

// ---------------------------------------------------------------------------
//  发送数据（供业务代码调用）
// ---------------------------------------------------------------------------
bool TcpServer::send(socket_t fd, const void* data, size_t len) {
    if (len == 0) {
        return true;
    }
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return false;
    }
    Connection& c = it->second;

    // 如果之前还有没发完的数据，新数据必须排到它们后面 ——
    // 否则 TCP 字节流的顺序就乱了。这是“发送缓冲区”存在的最直接原因。
    if (c.outSent < c.outBuf.size()) {
        appendToBuffer(fd, data, len);
        updateInterest(fd);
        return true;
    }

    // 缓冲区是空的，可以先尝试直接写出去，省掉一次内存拷贝
    size_t written = 0;
    while (written < len) {
        const int n = ::send(fd, (const char*)data + written, (int)(len - written), kSendFlags);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        const int err = socketError();
        if (isInterrupted(err)) {
            continue;
        }
        if (isWouldBlock(err)) {
            break;  // 内核缓冲区满了，剩下的只能缓存
        }
        closeConnection(fd);
        return false;
    }

    if (written < len) {
        appendToBuffer(fd, (const char*)data + written, len - written);
        updateInterest(fd);
    }
    return true;
}

void TcpServer::appendToBuffer(socket_t fd, const void* data, size_t len) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    Connection& c = it->second;
    // 注意：真实项目在这里必须做“高水位”检查。如果对端一直不发数据、
    // 应用层又一直 send，这个缓冲区会无限增长直到 OOM。
    // muduo 的做法是超过水位就回调通知上层，由上层决定丢弃还是断开连接。
    const size_t oldSize = c.outBuf.size();
    c.outBuf.resize(oldSize + len);
    std::memcpy(c.outBuf.data() + oldSize, data, len);
}

// ---------------------------------------------------------------------------
//  按需切换关注的事件
// ---------------------------------------------------------------------------
void TcpServer::updateInterest(socket_t fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    const Connection& c = it->second;

    uint32_t events = ev::Read;
    if (c.outSent < c.outBuf.size()) {
        events |= ev::Write;  // 还有数据没发完，需要可写通知
    }
    // 数据发完时这里会把 Write 去掉。这一步非常关键：
    // 可写事件是“几乎永远就绪”的，只要还关注着它，事件循环就会 100% 空转。
    poller_->mod(fd, events);
}

// ---------------------------------------------------------------------------
//  关闭连接
// ---------------------------------------------------------------------------
void TcpServer::closeConnection(socket_t fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }

    // 顺序绝对不能反：必须先 epoll_ctl(DEL) 再 close(fd)。
    // 一旦 close，fd 号会被内核回收，可能立刻被新连接复用；
    // 此时复用器里残留的旧注册项就会指向一个完全不相干的连接。
    poller_->del(fd);

    // ---- 关闭之前，先把接收缓冲区里剩下的数据读掉 -------------------------
    //
    // 这一步看着多余，其实关系到「客户端到底能不能收到我们刚发出去的响应」。
    //
    //   TCP 的规矩：如果一个 socket 被关闭时**接收队列里还有没读完的数据**，
    //   内核认为这条连接出了异常，转而发送 RST 而不是正常的 FIN。
    //   而 RST 的语义是"立刻作废这条连接" ——
    //   对端收到 RST 之后，**内核会把已经收下、但应用还没来得及读走的数据全部丢掉**，
    //   后续 recv 直接返回连接重置错误。
    //
    //   于是会出现一个非常反直觉的现象：
    //   我们明明成功把一个 400/414/431 的错误响应发出去了，客户端却一个字节都没收到，
    //   只拿到一个"连接被重置"(Windows 上是 WSAECONNRESET 10054)。
    //
    //   最容易触发它的场景恰恰就是"大请求被拒绝"：
    //   客户端一口气发来 20 KB 的请求，我们在读到第 12 KB 时就判定超限、
    //   回完错误就关 —— 剩下那 8 KB 还躺在接收队列里，close 于是发了 RST，
    //   把我们刚写出去的响应一并带走了。
    //
    // 处理办法：关之前把剩余数据读干净（读到 EAGAIN 为止）。
    // 加一个字节上限，免得对端持续不断地发、把这里拖住。
    // 正常关闭（对端先发 FIN）时队列本来就是空的，这个循环会立刻退出，没有额外开销。
    {
        char drain[4096];
        size_t drained = 0;
        constexpr size_t kMaxDrainBytes = 256 * 1024;
        while (drained < kMaxDrainBytes) {
            const int n = ::recv(fd, drain, (int)sizeof(drain), 0);
            if (n > 0) {
                drained += (size_t)n;
                continue;
            }
            // n == 0：对端已经关了（正常）；
            // n < 0：要么是 EAGAIN（读干净了，正常），要么是真错误。
            // 两种情况都直接停手。
            break;
        }
        // 注意：这里读到的东西**直接丢掉**。连接都决定要关了，
        // 剩下的输入本来就没有意义了 —— 我们要的只是"关闭时接收队列是空的"这个状态。
    }

    closeSocket(fd);
    conns_.erase(it);

    if (onClose_) {
        onClose_(fd);
    }
}

uint16_t TcpServer::boundPort() const {
    if (listenFd_ == kInvalidSocket) {
        return 0;
    }
    return localPort(listenFd_);
}

std::vector<socket_t> TcpServer::connections() const {
    std::vector<socket_t> v;
    v.reserve(conns_.size());
    for (const auto& kv : conns_) {
        v.push_back(kv.first);
    }
    return v;
}

}  // namespace net
