#pragma once
// ============================================================================
//  PollPoller —— 基于 poll(2) 的多路复用器（仅 Linux/Unix）
//
//  poll 相对 select 的改进：
//    * 用 pollfd 数组代替位图，fd 数量不再受 FD_SETSIZE 限制
//      （真正的上限是 RLIMIT_NOFILE 和内存）
//    * events（关注什么）和 revents（发生了什么）是两个独立字段，
//      内核只改 revents，所以关注集合可以保留复用，不必每次重建
//    * 不需要计算 maxfd + 1
//    * 超时单位是毫秒且不会被内核改写（select 是微秒且会被改写）
//    * 能表达更细的事件，例如 POLLPRI（带外数据）、POLLNVAL（fd 非法）
//
//  但 poll 没有解决的（本质问题）：
//    * 整个数组依然要拷进内核 —— O(n) 拷贝
//    * 内核依然要遍历所有 fd —— O(n) 轮询
//    * 返回后依然要遍历整个数组找就绪项 —— O(n) 扫描
//
//  Windows 上没有 poll。虽然从 Vista 起提供了 WSAPoll，但它长期存在
//  连接失败不上报的缺陷，微软自己的文档也建议避免使用，故此处不实现。
// ============================================================================

#ifdef __linux__

#include "net/EventPoller.h"

#include <poll.h>

#include <unordered_map>
#include <vector>

namespace net {

class PollPoller : public EventPoller {
public:
    explicit PollPoller(int maxEvents = 1024);
    ~PollPoller() override;

    bool add(socket_t fd, uint32_t events) override;
    bool mod(socket_t fd, uint32_t events) override;
    bool del(socket_t fd) override;
    int wait(std::vector<Event>& ready, int timeoutMs) override;

    const char* name() const override { return "poll"; }
    size_t size() const override { return interest_.size(); }

private:
    std::unordered_map<socket_t, uint32_t> interest_;

    // 传给内核的 pollfd 数组。虽然关注集合本身不会丢，但每次仍要重新
    // 填进这个连续数组并整体拷进内核。
    std::vector<pollfd> pfds_;
};

}  // namespace net

#endif  // __linux__
