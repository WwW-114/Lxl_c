#pragma once
// ============================================================================
//  EpollPoller —— 基于 epoll 的多路复用器（仅 Linux）
//
//  epoll 相对 select/poll 的两个根本性变化：
//
//   1) 注册与等待解耦
//        select/poll 是“每次调用都重新告诉内核我关心哪些 fd”；
//        epoll 用 epoll_ctl 一次性把 fd 注册进内核的红黑树，之后长期驻留，
//        epoll_wait 不再需要重复传递这份清单 —— 全量拷贝消失了。
//
//   2) 就绪事件由内核主动上报，而不是轮询发现
//        socket 收到数据 → 内核协议栈回调 ep_poll_callback → 把对应的
//        epitem 挂到就绪链表 rdllist → 唤醒 epoll_wait。
//        epoll_wait 只需检查 rdllist 是否为空，复杂度 O(就绪数量)，
//        与注册的总 fd 数无关。
//
//  注意两个常被夸大的说法：
//    * “epoll 是 O(1)” —— 严格说是 O(就绪数)。当所有 fd 都活跃时，
//      它仍要把每个就绪事件 copy_to_user，此时并不比 poll 快。
//    * “epoll 用 mmap 共享内存” —— 这是 2.4 时代旧实现的说法，
//      现代实现是 epoll_wait 时把就绪事件 copy_to_user 到用户缓冲区。
// ============================================================================

#ifdef __linux__

#include "net/EventPoller.h"

#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

namespace net {

class EpollPoller : public EventPoller {
public:
    EpollPoller(int maxEvents = 1024, bool edgeTriggered = false);
    ~EpollPoller() override;

    bool add(socket_t fd, uint32_t events) override;
    bool mod(socket_t fd, uint32_t events) override;
    bool del(socket_t fd) override;
    int wait(std::vector<Event>& ready, int timeoutMs) override;

    const char* name() const override { return edgeTriggered_ ? "epoll(ET)" : "epoll(LT)"; }
    size_t size() const override { return interest_.size(); }

    bool edgeTriggered() const { return edgeTriggered_; }

    // 把一个 fd 加入 EPOLLONESHOT 模式所需的重新激活操作。
    // 也可用于多线程下“处理完再重新注册”的场景。
    bool rearm(socket_t fd, uint32_t events);

private:
    uint32_t toEpoll(uint32_t events) const;
    static uint32_t fromEpoll(uint32_t e);

    int epfd_ = -1;
    int maxEvents_ = 1024;
    bool edgeTriggered_ = false;

    // 内核把就绪事件填进这个缓冲区，我们只遍历前 n 个
    std::vector<epoll_event> events_;

    std::unordered_map<socket_t, uint32_t> interest_;
};

}  // namespace net

#endif  // __linux__
