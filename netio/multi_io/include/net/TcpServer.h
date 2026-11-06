#pragma once
// ============================================================================
//  TcpServer.h —— 基于 EventPoller 的 Reactor 服务器
//
//  职责划分（这是理解 Reactor 模式的关键）：
//
//      EventPoller : 只回答一个技术问题 —— “哪些 fd 就绪了、发生了什么事件”
//      TcpServer   : 把就绪事件翻译成业务语义 —— 新连接 / 收到数据 / 连接关闭
//
//  这就是 Reactor 模式：一个单线程事件循环 + 一组回调函数。
//  循环本身不阻塞在任何一次具体 IO 上，只阻塞在“等待事件”上，
//  于是一个线程就能同时服务成千上万个连接。
//
//  本类同时实现了完整的“应用层发送缓冲区”，这是非阻塞网络编程的必修课：
//  非阻塞 socket 上 send 只保证“尽力而为”，可能只写出去一部分，
//  剩余数据必须缓存下来，等可写事件到来时续发。
// ============================================================================

#include "net/EventPoller.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace net {

class TcpServer {
public:
    using ConnectionCallback = std::function<void(socket_t fd)>;
    using MessageCallback = std::function<void(socket_t fd, const char* data, size_t len)>;
    using CloseCallback = std::function<void(socket_t fd)>;
    // 应用层发送缓冲区被**彻底排空**时触发（从"还有数据没发完"变成"发完了"）。
    // 用途见下面 setOnWriteComplete 的说明。
    using WriteCompleteCallback = std::function<void(socket_t fd)>;

    struct Options {
        EventPoller::Backend backend = EventPoller::Backend::Auto;
        bool edgeTriggered = false;  // 仅 epoll 有意义
        int maxEvents = 1024;
        int backlog = 128;
        size_t readChunk = 4096;     // 单次 recv 的缓冲区大小
    };

    explicit TcpServer(Options opts = {});
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // ---- 生命周期 ---------------------------------------------------------

    // 绑定并监听。ip 为空表示监听所有网卡。
    bool listen(const std::string& ip, uint16_t port);

    // 跑一轮事件循环：等待事件 -> 分发处理。timeoutMs < 0 表示一直阻塞。
    void loopOnce(int timeoutMs = -1);

    // 反复调用 loopOnce(-1)，直到 stop()
    void loop();

    void stop() { running_ = false; }
    bool running() const { return running_; }

    // ---- 数据收发 ---------------------------------------------------------

    // 发送数据。非阻塞下可能只发出去一部分，剩余部分会进入应用层发送缓冲区，
    // 自动在后续可写事件中续发。返回 false 表示连接不存在或已出错关闭。
    bool send(socket_t fd, const void* data, size_t len);
    bool send(socket_t fd, const std::string& data) { return send(fd, data.data(), data.size()); }

    // 主动关闭连接：从多路复用器注销 + 关闭 fd + 触发 onClose 回调
    void closeConnection(socket_t fd);

    // ---- 回调注册 ---------------------------------------------------------

    void setOnConnect(ConnectionCallback cb) { onConnect_ = std::move(cb); }
    void setOnMessage(MessageCallback cb) { onMessage_ = std::move(cb); }
    void setOnClose(CloseCallback cb) { onClose_ = std::move(cb); }

    // 发送缓冲区排空时回调。
    //
    // 为什么需要它：非阻塞 socket 上一次 send 往往写不完（大响应必然如此），
    // 剩下的会留在应用层发送缓冲区里，由后续的可写事件慢慢续发。
    // 如果业务代码想"发完这些数据就把连接关掉"，**不能**在 send() 返回后
    // 直接调 closeConnection() —— 缓冲区里还没发出去的数据会随着 fd 一起蒸发，
    // 客户端收到一个被拦腰截断的响应，而且不报任何错。
    //
    // 正确的做法是两件事配合：
    //   1) 标记"这个连接发完就关"；
    //   2) 若此刻 sendBufferEmpty() 已经是 true（小响应一次就写完了），立即关；
    //      否则等这个回调来关。
    //
    // 这也是 muduo 里 WriteCompleteCallback 的设计。
    void setOnWriteComplete(WriteCompleteCallback cb) { onWriteComplete_ = std::move(cb); }

    // 应用层发送缓冲区是否已经排空（fd 不存在时返回 false）。
    bool sendBufferEmpty(socket_t fd) const;

    // ---- 查询 -------------------------------------------------------------

    EventPoller& poller() { return *poller_; }
    std::vector<socket_t> connections() const;

    // 实际绑定的端口。listen 时传 0 表示由系统分配，用这个查出来。
    uint16_t boundPort() const;

private:
    struct Connection {
        socket_t fd = kInvalidSocket;
        std::vector<char> outBuf;  // 应用层发送缓冲区
        size_t outSent = 0;        // 其中已经成功写出去多少字节
    };

    void handleAccept();
    // 返回 false 表示连接已不存在（可能在回调里被关闭了）
    bool handleRead(socket_t fd);
    bool handleWrite(socket_t fd);
    // 根据“是否还有数据待发”重新设置关注的事件
    void updateInterest(socket_t fd);
    void appendToBuffer(socket_t fd, const void* data, size_t len);

    Options opts_;
    std::unique_ptr<EventPoller> poller_;
    socket_t listenFd_ = kInvalidSocket;
    std::unordered_map<socket_t, Connection> conns_;
    bool running_ = false;

    ConnectionCallback onConnect_;
    MessageCallback onMessage_;
    CloseCallback onClose_;
    WriteCompleteCallback onWriteComplete_;
};

}  // namespace net
