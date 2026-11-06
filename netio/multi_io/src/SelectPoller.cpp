#include "net/SelectPoller.h"

namespace net {

SelectPoller::SelectPoller(int /*maxEvents*/) {
    // select 没有“事件数量”的概念，一次调用能返回多少就绪 fd 由传入的集合决定。
    // 参数保留只是为了和另外两个后端保持一致的接口。
}

SelectPoller::~SelectPoller() = default;

// ---------------------------------------------------------------------------
//  注册一个新的 fd。注意 Linux 上必须检查 fd < FD_SETSIZE：
//  FD_SET 是位图操作，fd 一旦 >= FD_SETSIZE 就会越界写内存 —— 这是 select
//  最阴险的坑之一，而且不会报错，只会随机破坏别的内存。
// ---------------------------------------------------------------------------
bool SelectPoller::add(socket_t fd, uint32_t events) {
    if (fd == kInvalidSocket) {
        lastError_ = "add: 非法 fd";
        return false;
    }

#ifndef _WIN32
    if (fd >= FD_SETSIZE) {
        lastError_ = "add: fd " + std::to_string(fd) + " 超出 FD_SETSIZE(" +
                     std::to_string(FD_SETSIZE) + ")，select 无法管理";
        return false;
    }
#endif

    if (interest_.count(fd) != 0) {
        return mod(fd, events);  // 已存在则视为修改
    }

    interest_[fd] = events;
    if (maxFd_ == kInvalidSocket || fd > maxFd_) {
        maxFd_ = fd;
    }
    return true;
}

bool SelectPoller::mod(socket_t fd, uint32_t events) {
    auto it = interest_.find(fd);
    if (it == interest_.end()) {
        lastError_ = "mod: fd 未注册";
        return false;
    }
    it->second = events;
    return true;
}

bool SelectPoller::del(socket_t fd) {
    auto it = interest_.find(fd);
    if (it == interest_.end()) {
        lastError_ = "del: fd 未注册";
        return false;
    }
    interest_.erase(it);

    // 删掉的如果正好是最大 fd，必须重新扫一遍找出新的最大值，
    // 否则 select 的 nfds 参数会过大，白白多轮询一堆无关 fd
    if (fd == maxFd_) {
        maxFd_ = kInvalidSocket;
        for (const auto& kv : interest_) {
            if (maxFd_ == kInvalidSocket || kv.first > maxFd_) {
                maxFd_ = kv.first;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  等待事件 —— select 的全部问题都浓缩在这个函数里
// ---------------------------------------------------------------------------
int SelectPoller::wait(std::vector<Event>& ready, int timeoutMs) {
    ready.clear();

    // 【问题 1】每次调用都要重新装配三个集合。
    // 因为上一次 select 返回时，内核已经把 fd_set 改成了“只有就绪的 fd 置位”，
    // 我们的关注集合信息在内核返回的那一刻就被销毁了。
    FD_ZERO(&rset_);
    FD_ZERO(&wset_);
    FD_ZERO(&eset_);

    for (const auto& kv : interest_) {
        const socket_t fd = kv.first;
        const uint32_t events = kv.second;
        if ((events & ev::Read) != 0) {
            FD_SET(fd, &rset_);
        }
        if ((events & ev::Write) != 0) {
            FD_SET(fd, &wset_);
        }
        FD_SET(fd, &eset_);  // 异常集合总是关注，用于捕获错误
    }

    // 【问题 2】timeval 会被内核改写成“剩余时间”，所以每次都必须重新构造。
    // 这是 select 最经典的陷阱：复用同一个 timeval 会导致超时时间越来越短。
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }

    // 【问题 3】这一行就是 O(n) 的全量拷贝：三个集合（每个 FD_SETSIZE 位）
    // 从用户态复制进内核，然后内核再逐个 fd 调用 vfs_poll 询问“就绪了吗”。
#ifdef _WIN32
    const int n = ::select(0, &rset_, &wset_, &eset_, ptv);  // Windows 忽略第一个参数
#else
    const int n = ::select((int)maxFd_ + 1, &rset_, &wset_, &eset_, ptv);
#endif

    if (n < 0) {
        const int err = socketError();
        if (isInterrupted(err)) {
            return 0;  // 被信号打断，当作超时处理
        }
        lastError_ = "select: " + errorString(err);
        return -1;
    }
    if (n == 0) {
        return 0;  // 超时
    }

    // 【问题 4】内核只告诉我们“一共有 n 个就绪”，却没说具体是哪几个。
    // 于是必须把整个 interest_ 再扫一遍，逐个 FD_ISSET 询问。
    // 到这一步为止，一共遍历了三遍：装配一遍、内核轮询一遍、这里再扫一遍。
    for (const auto& kv : interest_) {
        const socket_t fd = kv.first;
        uint32_t rev = ev::None;
        if (FD_ISSET(fd, &rset_)) {
            rev |= ev::Read;
        }
        if (FD_ISSET(fd, &wset_)) {
            rev |= ev::Write;
        }
        if (FD_ISSET(fd, &eset_)) {
            rev |= ev::Error;
        }
        if (rev != ev::None) {
            ready.push_back(Event{fd, rev});
        }
    }

    return (int)ready.size();
}

}  // namespace net
