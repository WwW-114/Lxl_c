#include "net/EpollPoller.h"

#ifdef __linux__

namespace net {

EpollPoller::EpollPoller(int maxEvents, bool edgeTriggered)
    : maxEvents_(maxEvents > 0 ? maxEvents : 1024), edgeTriggered_(edgeTriggered) {
    // EPOLL_CLOEXEC：fork + exec 之后这个 fd 会自动关闭，
    // 避免把 epoll 实例泄漏给子进程。等价于 epoll_create + fcntl(FD_CLOEXEC)。
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        lastError_ = "epoll_create1: " + errorString(socketError());
        return;
    }

    // 这块缓冲区是用户态内存，epoll_wait 时内核把就绪事件 copy 到这里。
    // 它的大小决定了“一次 epoll_wait 最多能返回多少事件”，
    // 并不会限制能注册多少 fd（注册数量只受内存和 RLIMIT_NOFILE 限制）。
    events_.resize((size_t)maxEvents_);
}

EpollPoller::~EpollPoller() {
    if (epfd_ >= 0) {
        ::close(epfd_);
        // 注意：关闭 epoll 实例会自动注销其上所有 fd，但被监听的那些 fd 本身
        // 不会关闭，仍需要调用方自己 close，否则就是 fd 泄漏。
    }
}

// ---------------------------------------------------------------------------
//  我们的事件位 -> epoll 的事件位
// ---------------------------------------------------------------------------
uint32_t EpollPoller::toEpoll(uint32_t events) const {
    uint32_t e = 0;
    if ((events & ev::Read) != 0) {
        // 关注可读时顺便关注 EPOLLRDHUP，这样对端半关闭（只关写方向）时
        // 我们能立刻感知，不必等到真正读到 0。
        e |= EPOLLIN | EPOLLRDHUP;
    }
    if ((events & ev::Write) != 0) {
        e |= EPOLLOUT;
    }
    if (edgeTriggered_) {
        e |= EPOLLET;
    }
    // EPOLLERR / EPOLLHUP 不需要显式注册，内核总会无条件上报，
    // 而且无法通过 epoll_ctl 关掉。
    return e;
}

uint32_t EpollPoller::fromEpoll(uint32_t e) {
    uint32_t r = ev::None;
    if ((e & (EPOLLIN | EPOLLPRI)) != 0) {
        r |= ev::Read;
    }
    if ((e & EPOLLOUT) != 0) {
        r |= ev::Write;
    }
    if ((e & EPOLLERR) != 0) {
        r |= ev::Error;
    }
    if ((e & EPOLLHUP) != 0) {
        r |= ev::Hup;
    }
    if ((e & EPOLLRDHUP) != 0) {
        r |= ev::Rdhup;
    }
    return r;
}

// ---------------------------------------------------------------------------
//  注册 —— 整个生命周期里只做一次，之后 fd 就常驻内核红黑树了
// ---------------------------------------------------------------------------
bool EpollPoller::add(socket_t fd, uint32_t events) {
    if (epfd_ < 0) {
        lastError_ = "add: epoll 实例未成功创建";
        return false;
    }
    if (interest_.count(fd) != 0) {
        return mod(fd, events);
    }

    epoll_event e{};
    e.events = toEpoll(events);
    // data 是唯一能在事件返回时带回来的“用户数据”，内核原样保存、原样返回。
    // 直接放 fd 是最简单的用法；muduo/libevent 会在这里放一个指针，
    // 直接指向连接对象，省掉一次查表。
    e.data.fd = fd;

    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e) < 0) {
        const int err = socketError();
        lastError_ = "epoll_ctl(ADD) fd=" + std::to_string((long long)fd) + ": " + errorString(err);
        return false;
    }

    interest_[fd] = events;
    return true;
}

bool EpollPoller::mod(socket_t fd, uint32_t events) {
    if (epfd_ < 0) {
        lastError_ = "mod: epoll 实例未成功创建";
        return false;
    }
    if (interest_.count(fd) == 0) {
        lastError_ = "mod: fd 未注册";
        return false;
    }

    epoll_event e{};
    e.events = toEpoll(events);
    e.data.fd = fd;

    // EPOLL_CTL_MOD 就是“改关注的事件”（比如从只读改成读写），
    // 不会引起内核红黑树的插入删除，代价很低。
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &e) < 0) {
        const int err = socketError();
        lastError_ = "epoll_ctl(MOD) fd=" + std::to_string((long long)fd) + ": " + errorString(err);
        return false;
    }

    interest_[fd] = events;
    return true;
}

bool EpollPoller::del(socket_t fd) {
    if (epfd_ < 0) {
        return false;
    }
    if (interest_.count(fd) == 0) {
        return false;
    }

    // 从内核红黑树移除，并解绑挂在 socket 等待队列上的回调。
    // 顺序很重要：必须先 epoll_ctl(DEL) 再 close(fd)。
    // 如果先 close，fd 号会被内核回收并可能立刻分配给新连接，
    // 此时红黑树里残留的旧 epitem 就会误伤那个新连接。
    // 另外：close 一个仍注册在 epoll 上的 fd 会让内核自动移除它，
    // 但如果这个 fd 是被 dup 出来的，就需要显式 DEL 了。
    if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        const int err = socketError();
        lastError_ = "epoll_ctl(DEL) fd=" + std::to_string((long long)fd) + ": " + errorString(err);
        return false;
    }

    interest_.erase(fd);
    return true;
}

// 重新激活一个 fd。配合 EPOLLONESHOT 使用：
// 因为 ONESHOT 模式下事件只通知一次，处理完必须 MOD 一次才能收到下一次。
bool EpollPoller::rearm(socket_t fd, uint32_t events) {
    return mod(fd, events);
}

// ---------------------------------------------------------------------------
//  等待事件 —— 这是 epoll 与 select/poll 性能差距的来源
// ---------------------------------------------------------------------------
int EpollPoller::wait(std::vector<Event>& ready, int timeoutMs) {
    ready.clear();

    if (epfd_ < 0) {
        lastError_ = "wait: epoll 实例未成功创建";
        return -1;
    }

    // 这一行背后发生的事：
    //   1. 检查内核里 eventpoll->rdllist 就绪链表是否为空
    //   2. 为空  -> 把当前线程挂到 eventpoll->wq 上睡眠，直到超时或被唤醒
    //   3. 非空  -> 逐个确认事件仍然有效，然后 copy_to_user 到 events_ 缓冲
    //
    // 关键：这里没有遍历所有注册的 fd，也没有传入任何 fd 清单。
    // 代价是 O(就绪数量)，与注册了多少 fd 无关。
    const int n = ::epoll_wait(epfd_, events_.data(), maxEvents_, timeoutMs);

    if (n < 0) {
        const int err = socketError();
        if (isInterrupted(err)) {
            return 0;  // 被信号打断，当作超时
        }
        lastError_ = "epoll_wait: " + errorString(err);
        return -1;
    }
    if (n == 0) {
        return 0;  // 超时
    }

    // 只遍历内核实际填进来的前 n 个事件，不是全部注册的 fd
    for (int i = 0; i < n; ++i) {
        ready.push_back(Event{events_[i].data.fd, fromEpoll(events_[i].events)});
    }
    return n;
}

}  // namespace net

#endif  // __linux__
