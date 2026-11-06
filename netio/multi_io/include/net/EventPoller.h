#pragma once
// ============================================================================
//  EventPoller.h —— IO 多路复用器的统一抽象接口
//
//  设计要点：
//    三种系统调用（select / poll / epoll）的能力差异极大，但“注册 fd →
//    等待事件 → 处理事件”这个使用模型是一致的。把它们抽象成同一个接口，
//    上层业务代码（TcpServer）就不必关心底层到底用了哪种机制。
//
//    这正是 libevent / libuv / muduo 的共同分层方式，也是“面向接口编程”
//    在 IO 层最典型的应用。
// ============================================================================

#include "net/SocketUtils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace net {

// ---------------------------------------------------------------------------
//  事件类型：用位掩码，一个 fd 可以同时关注多种事件
// ---------------------------------------------------------------------------
namespace ev {
constexpr uint32_t None  = 0x000;
constexpr uint32_t Read  = 0x001;  // 可读：有数据到达 / 对端关闭 / 有新连接
constexpr uint32_t Write = 0x002;  // 可写：内核发送缓冲区有空位
constexpr uint32_t Error = 0x004;  // 出错
constexpr uint32_t Hup   = 0x008;  // 对端挂断
constexpr uint32_t Rdhup = 0x010;  // 对端关闭写方向（半关闭），仅 epoll 会上报
}  // namespace ev

// 一次就绪事件的描述
struct Event {
    socket_t fd = kInvalidSocket;
    uint32_t events = ev::None;
};

// ============================================================================
//  EventPoller
// ============================================================================
class EventPoller {
public:
    enum class Backend {
        Auto,    // 自动选择：Linux 用 epoll，Windows 用 select
        Select,  // 所有平台可用
        Poll,    // 仅 Linux
        Epoll,   // 仅 Linux
    };

    static const char* backendName(Backend b);

    // 创建实例。失败返回 nullptr（例如在 Windows 上请求 epoll）。
    //   maxEvents     : epoll_wait 一次最多返回多少个事件（对 select/poll 只是预留空间）
    //   edgeTriggered : 是否使用边缘触发，仅 epoll 有意义
    static std::unique_ptr<EventPoller> create(Backend backend = Backend::Auto,
                                               int maxEvents = 1024,
                                               bool edgeTriggered = false);

    virtual ~EventPoller() = default;

    EventPoller(const EventPoller&) = delete;
    EventPoller& operator=(const EventPoller&) = delete;

    // ---- 注册 / 修改 / 注销 关注的 fd --------------------------------------
    // 返回值表示调用是否成功，失败原因见 lastError()
    virtual bool add(socket_t fd, uint32_t events) = 0;
    virtual bool mod(socket_t fd, uint32_t events) = 0;
    virtual bool del(socket_t fd) = 0;

    // ---- 等待事件 -----------------------------------------------------------
    // 超时时间毫秒，< 0 表示永久阻塞。
    // 返回：> 0 就绪事件数量；0 超时；-1 出错（见 lastError()）
    virtual int wait(std::vector<Event>& ready, int timeoutMs = -1) = 0;

    virtual const char* name() const = 0;
    virtual size_t size() const = 0;  // 当前注册的 fd 数量

    const std::string& lastError() const { return lastError_; }

protected:
    EventPoller() = default;

    std::string lastError_;
};

}  // namespace net
