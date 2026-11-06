#include "net/PollPoller.h"

#ifdef __linux__

namespace net {

namespace {

// 我们自己的事件位 -> poll 的 events 字段
short toPollEvents(uint32_t e) {
    short p = 0;
    if ((e & ev::Read) != 0) {
        p |= POLLIN;
    }
    if ((e & ev::Write) != 0) {
        p |= POLLOUT;
    }
    return p;
}

// poll 的 revents 字段 -> 我们自己的事件位
uint32_t fromPollEvents(short r) {
    uint32_t e = ev::None;
    // POLLPRI 是带外数据（TCP 紧急数据），语义上归入“可读”
    if ((r & (POLLIN | POLLPRI)) != 0) {
        e |= ev::Read;
    }
    if ((r & POLLOUT) != 0) {
        e |= ev::Write;
    }
    if ((r & POLLERR) != 0) {
        e |= ev::Error;
    }
    if ((r & POLLHUP) != 0) {
        e |= ev::Hup;
    }
    if ((r & POLLNVAL) != 0) {
        e |= ev::Error;  // fd 非法，例如已经被 close 过
    }
    return e;
}

}  // namespace

PollPoller::PollPoller(int maxEvents) {
    // 只是预留容量，避免运行期反复扩容。真实的 fd 数量不受此限制。
    if (maxEvents > 0) {
        pfds_.reserve((size_t)maxEvents);
    }
}

PollPoller::~PollPoller() = default;

bool PollPoller::add(socket_t fd, uint32_t events) {
    if (fd == kInvalidSocket) {
        lastError_ = "add: 非法 fd";
        return false;
    }
    // 相比 select，这里不再需要检查 FD_SETSIZE，但 fd 仍受
    // RLIMIT_NOFILE（ulimit -n）限制，超过时 socket()/accept() 会直接失败。
    if (interest_.count(fd) != 0) {
        return mod(fd, events);
    }
    interest_[fd] = events;
    return true;
}

bool PollPoller::mod(socket_t fd, uint32_t events) {
    auto it = interest_.find(fd);
    if (it == interest_.end()) {
        lastError_ = "mod: fd 未注册";
        return false;
    }
    it->second = events;
    return true;
}

bool PollPoller::del(socket_t fd) {
    auto it = interest_.find(fd);
    if (it == interest_.end()) {
        lastError_ = "del: fd 未注册";
        return false;
    }
    interest_.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
//  等待事件 —— 相比 select 少了一次“重建关注集合”，但仍保留了两次 O(n)
// ---------------------------------------------------------------------------
int PollPoller::wait(std::vector<Event>& ready, int timeoutMs) {
    ready.clear();
    pfds_.clear();

    // 【改进点】events（关注什么）和 revents（发生了什么）是两个独立字段，
    // 内核只改 revents，所以我们的关注集合可以原样复用，不必每次 FD_ZERO/FD_SET。
    // 【仍是问题】但数组本身依然要整体拷进内核 —— O(n) 拷贝没有被消除。
    for (const auto& kv : interest_) {
        pollfd p{};
        p.fd = kv.first;
        p.events = toPollEvents(kv.second);
        p.revents = 0;  // 必须清零，否则上一轮的残留会被误认为事件
        pfds_.push_back(p);
    }

    // 【仍是问题】内核依然要遍历整个数组，逐个调用 vfs_poll 询问就绪情况，O(n)。
    // timeout 单位是毫秒（select 是微秒），且 poll 不会改写这个值。
    const int n = ::poll(pfds_.data(), (nfds_t)pfds_.size(), timeoutMs);

    if (n < 0) {
        const int err = socketError();
        if (isInterrupted(err)) {
            return 0;
        }
        lastError_ = "poll: " + errorString(err);
        return -1;
    }
    if (n == 0) {
        return 0;  // 超时
    }

    // 【仍是问题】同样不知道是哪几个就绪，只能把整个数组再扫一遍，O(n)。
    for (const pollfd& p : pfds_) {
        if (p.revents == 0) {
            continue;  // 这个 fd 本轮没事件
        }
        ready.push_back(Event{p.fd, fromPollEvents(p.revents)});
    }

    return (int)ready.size();
}

}  // namespace net

#endif  // __linux__
