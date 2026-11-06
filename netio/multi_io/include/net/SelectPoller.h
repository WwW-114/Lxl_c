#pragma once
// ============================================================================
//  SelectPoller —— 基于 select(2) / Winsock select 的多路复用器
//
//  select 的核心特征（也是它的全部问题来源）：
//    * 关注集合用位图 fd_set 表示，容量编译期固定为 FD_SETSIZE（通常 1024）
//    * fd_set 是“值传递”：每次调用都要把整个集合从用户态拷进内核
//    * fd_set 会被内核“破坏性修改”，返回后只剩就绪的 fd，下次必须重新装配
//    * 内核要 O(n) 遍历所有 fd 询问“你好了吗”
//    * 返回后用户态仍要 O(n) 遍历才能知道到底是谁就绪
//
//  唯一的优势：几乎所有平台都有（Windows / Linux / macOS / 各种 Unix）。
// ============================================================================

#include "net/EventPoller.h"

#include <unordered_map>

namespace net {

class SelectPoller : public EventPoller {
public:
    explicit SelectPoller(int maxEvents = 1024);
    ~SelectPoller() override;

    bool add(socket_t fd, uint32_t events) override;
    bool mod(socket_t fd, uint32_t events) override;
    bool del(socket_t fd) override;
    int wait(std::vector<Event>& ready, int timeoutMs) override;

    const char* name() const override { return "select"; }
    size_t size() const override { return interest_.size(); }

private:
    // fd -> 关注的事件。这份表是我们自己维护的，因为内核会破坏 fd_set
    std::unordered_map<socket_t, uint32_t> interest_;

    // select 需要“最大 fd + 1”作为第一个参数，每次增删都得维护
    socket_t maxFd_ = kInvalidSocket;

    // 三个集合作为成员复用，避免每次 wait 在栈上分配 3 * FD_SETSIZE 位
    fd_set rset_{};
    fd_set wset_{};
    fd_set eset_{};
};

}  // namespace net
