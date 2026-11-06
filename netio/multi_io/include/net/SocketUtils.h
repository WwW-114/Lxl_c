#pragma once
// ============================================================================
//  SocketUtils.h —— 跨平台 socket 基础工具
//
//  只提供最小可用能力：类型统一、错误码统一、非阻塞设置、监听套接字创建。
//  目的：让上层 select / poll / epoll 的代码不必到处写 #ifdef。
// ============================================================================

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN      // 阻止 windows.h 拉入古老的 winsock.h（与 winsock2.h 冲突）
#  endif
#  ifndef FD_SETSIZE
// Windows 的默认值只有 64，非常小。winsock2.h 里对 FD_SETSIZE 有 #ifndef 保护，
// 所以必须在这里（早于 include <winsock2.h>）先定义。
#    define FD_SETSIZE 1024
#  endif
#  ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#    define _WINSOCK_DEPRECATED_NO_WARNINGS
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>               // FormatMessageA
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace net {

// ---------------------------------------------------------------------------
//  1. 统一的 socket 句柄类型
//
//  Linux 里 socket 就是一个 int 文件描述符；Windows 里是一个内核对象句柄
//  （UINT_PTR，64 位下是 64 位无符号整数）。用别名抹平差异。
// ---------------------------------------------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
const socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
const socket_t kInvalidSocket = -1;
#endif

// ---------------------------------------------------------------------------
//  2. 进程级初始化和清理
// ---------------------------------------------------------------------------

// Windows 下使用任何 socket API 之前必须先 WSAStartup，否则全部返回失败；
// Linux 下是空操作。std::call_once 保证多线程下也只执行一次。
inline void initNetwork() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::abort();
        }
    });
#endif
}

inline void cleanupNetwork() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ---------------------------------------------------------------------------
//  3. 统一的错误处理
//
//  Linux 用 errno，Windows 用 WSAGetLastError()，错误码编号完全不同，
//  所以要提供一层语义化的判断函数，而不是到处比较 ERRNO 常量。
// ---------------------------------------------------------------------------

// 注意命名：故意不叫 lastError()。
// EventPoller 有个成员函数 lastError()，C++ 的名字隐藏规则会让派生类成员函数
// 里的 lastError() 优先解析到那个成员函数，导致调用者拿到 std::string 而不是错误码。
// 这是个很容易踩、报错信息又很迷惑人的坑。
inline int socketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// “现在没数据 / 缓冲区已满，稍后再试”
// 非阻塞 IO 中最常见的返回值，ET 模式正是靠它来判断“读干净了”
inline bool isWouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// “被信号中断”，应该重试而不是当成错误
inline bool isInterrupted(int err) {
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

inline std::string errorString(int err) {
#ifdef _WIN32
    char* buf = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, (DWORD)err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     (LPSTR)&buf, 0, nullptr);
    std::string s = (buf != nullptr) ? buf : "unknown error";
    if (buf != nullptr) {
        ::LocalFree(buf);
    }
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s + " (" + std::to_string(err) + ")";
#else
    return std::string(std::strerror(err)) + " (" + std::to_string(err) + ")";
#endif
}

// 向已被对端关闭的连接写数据时，Linux 默认会发送 SIGPIPE 直接杀掉进程。
// 这是网络服务器最常见的“莫名退出”原因之一，必须屏蔽。
inline void ignoreSigPipe() {
#ifndef _WIN32
    ::signal(SIGPIPE, SIG_IGN);
#endif
}

// ---------------------------------------------------------------------------
//  4. socket 选项
// ---------------------------------------------------------------------------

inline bool setNonBlocking(socket_t fd, bool on = true) {
#ifdef _WIN32
    u_long mode = on ? 1UL : 0UL;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

// 允许 bind 处于 TIME_WAIT 状态的地址，服务器重启时不会因 “Address already in use” 失败
inline bool setReuseAddr(socket_t fd, bool on = true) {
    int v = on ? 1 : 0;
    return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&v, (socklen_t)sizeof(v)) == 0;
}

// 关闭 Nagle 算法：小包立即发送，降低延迟（代价是可能增加小包数量）
inline bool setTcpNoDelay(socket_t fd, bool on = true) {
    int v = on ? 1 : 0;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, (socklen_t)sizeof(v)) == 0;
}

inline bool setKeepAlive(socket_t fd, bool on = true) {
    int v = on ? 1 : 0;
    return ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&v, (socklen_t)sizeof(v)) == 0;
}

// 取本地端口（bind 时传 0 由系统分配端口后，用这个查出来）
inline uint16_t localPort(socket_t fd) {
    sockaddr_storage ss{};
    socklen_t len = (socklen_t)sizeof(ss);
    if (::getsockname(fd, (sockaddr*)&ss, &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(((sockaddr_in*)&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(((sockaddr_in6*)&ss)->sin6_port);
    }
    return 0;
}

inline void closeSocket(socket_t fd) {
    if (fd == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

// ---------------------------------------------------------------------------
//  5. 创建监听套接字
//
//  用 getaddrinfo 而不是手动填 sockaddr_in，好处是 IPv4 / IPv6 都能支持。
//  典型流程：socket() -> setsockopt(SO_REUSEADDR) -> bind() -> listen()
// ---------------------------------------------------------------------------
inline socket_t createListener(const std::string& ip, uint16_t port, int backlog,
                               std::string* err = nullptr) {
    auto fail = [err](const std::string& msg) -> socket_t {
        if (err != nullptr) {
            *err = msg;
        }
        return kInvalidSocket;
    };

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // 同时接受 IPv4 / IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP
    hints.ai_flags = AI_PASSIVE;      // 用于 bind()，ip 为空表示监听所有网卡

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    const int rc = ::getaddrinfo(ip.empty() ? nullptr : ip.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        return fail("getaddrinfo: " + std::string(gai_strerror(rc)));
    }

    socket_t listener = kInvalidSocket;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        socket_t fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }
        setReuseAddr(fd, true);
        if (p->ai_family == AF_INET6) {
            // 双栈：让 IPv6 监听套接字同时接受 IPv4 映射地址的连接
            int v6only = 0;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, (socklen_t)sizeof(v6only));
        }
        if (::bind(fd, p->ai_addr, (socklen_t)p->ai_addrlen) == 0 && ::listen(fd, backlog) == 0) {
            listener = fd;
            break;
        }
        closeSocket(fd);
    }
    ::freeaddrinfo(res);

    if (listener == kInvalidSocket) {
        return fail("bind/listen 失败: " + errorString(socketError()));
    }
    return listener;
}

}  // namespace net
