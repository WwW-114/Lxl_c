#include "net/EventPoller.h"

#include "net/SelectPoller.h"

#ifdef __linux__
#  include "net/EpollPoller.h"
#  include "net/PollPoller.h"
#endif

namespace net {

const char* EventPoller::backendName(Backend b) {
    switch (b) {
        case Backend::Select: return "select";
        case Backend::Poll:   return "poll";
        case Backend::Epoll:  return "epoll";
        case Backend::Auto:   return "auto";
    }
    return "unknown";
}

std::unique_ptr<EventPoller> EventPoller::create(Backend backend, int maxEvents, bool edgeTriggered) {
    if (backend == Backend::Auto) {
        // 自动选择：Linux 上 epoll 明显优于另外两者；Windows 上只有 select 可用
#ifdef __linux__
        backend = Backend::Epoll;
#else
        backend = Backend::Select;
#endif
    }

#ifndef __linux__
    (void)edgeTriggered;  // ET 是 epoll 独有的能力，其他后端忽略这个参数

    // poll / epoll 都是 Linux 特有的系统调用。
    // Windows 从 Vista 起虽然提供了 WSAPoll，但它长期存在“连接失败不上报”的缺陷
    // （官方文档承认并与 select 的行为不一致），微软建议改用 select 或 IOCP，
    // 所以这里直接在非 Linux 平台拒绝这两种后端。
    if (backend == Backend::Poll || backend == Backend::Epoll) {
        return nullptr;
    }
#endif

    switch (backend) {
        case Backend::Select:
            return std::make_unique<SelectPoller>(maxEvents);
#ifdef __linux__
        case Backend::Poll:
            return std::make_unique<PollPoller>(maxEvents);
        case Backend::Epoll: {
            auto p = std::make_unique<EpollPoller>(maxEvents, edgeTriggered);
            // epoll_create1 失败时对象仍然构造出来了，这里检查一下
            return p;
        }
#endif
        default:
            break;
    }
    return nullptr;
}

}  // namespace net
