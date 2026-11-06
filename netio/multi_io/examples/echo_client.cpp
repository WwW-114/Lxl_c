// ============================================================================
//  echo_client.cpp —— 配套的压测客户端
//
//  故意用最简单的阻塞 socket 实现：客户端不需要并发处理很多连接，
//  用多路复用只是徒增复杂度。（工具要选对场景，不是越“高级”越好。）
//
//  它的作用是验证服务端：
//    * 大量小包 -> 检验 LT/ET 下的读循环是否正确（漏读会导致数据对不上）
//    * 大包     -> 检验服务端的应用层发送缓冲区是否正确（丢数据会校验失败）
//
//  用法: echo_client [主机] [端口] [消息数] [消息大小]
//        例如: echo_client 127.0.0.1 9000 10000 4096
// ============================================================================

#include "net/SocketUtils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 阻塞 socket 上循环发送，直到全部写完
bool sendAll(net::socket_t fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) {
            const int err = net::socketError();
            if (net::isInterrupted(err)) {
                continue;
            }
            std::fprintf(stderr, "send 失败: %s\n", net::errorString(err).c_str());
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// 阻塞 socket 上循环接收，直到收满 len 字节
bool recvAll(net::socket_t fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        const int n = ::recv(fd, data + got, (int)(len - got), 0);
        if (n == 0) {
            std::fprintf(stderr, "对端提前关闭连接\n");
            return false;
        }
        if (n < 0) {
            const int err = net::socketError();
            if (net::isInterrupted(err)) {
                continue;
            }
            std::fprintf(stderr, "recv 失败: %s\n", net::errorString(err).c_str());
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    net::initNetwork();

    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    int count = 1000;
    int size = 512;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = (uint16_t)std::atoi(argv[2]);
    if (argc > 3) count = std::atoi(argv[3]);
    if (argc > 4) size = std::atoi(argv[4]);
    if (count <= 0 || size <= 0) {
        std::fprintf(stderr, "参数非法\n");
        return 1;
    }

    // ---- 解析地址并连接 ---------------------------------------------------
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "无法解析主机 %s\n", host.c_str());
        return 1;
    }

    net::socket_t fd = net::kInvalidSocket;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == net::kInvalidSocket) {
            continue;
        }
        if (::connect(fd, p->ai_addr, (socklen_t)p->ai_addrlen) == 0) {
            break;
        }
        net::closeSocket(fd);
        fd = net::kInvalidSocket;
    }
    ::freeaddrinfo(res);

    if (fd == net::kInvalidSocket) {
        std::fprintf(stderr, "连接 %s:%u 失败: %s\n", host.c_str(), (unsigned)port,
                     net::errorString(net::socketError()).c_str());
        return 1;
    }
    net::setTcpNoDelay(fd, true);

    std::printf("已连接 %s:%u\n", host.c_str(), (unsigned)port);
    std::printf("发送 %d 条消息，每条 %d 字节，共 %.2f MiB\n\n", count, size,
                (double)count * size / 1024.0 / 1024.0);

    // ---- 收发 -------------------------------------------------------------
    std::vector<char> out((size_t)size, 'A');
    std::vector<char> in((size_t)size, 0);

    // 让每条消息内容都不一样，这样校验才能发现“粘包 / 少读”这类错误
    for (int i = 0; i < size; ++i) {
        out[(size_t)i] = (char)('A' + (i % 26));
    }

    int okCount = 0;
    int badCount = 0;
    const auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < count; ++i) {
        // 最后 4 字节放序号，便于定位是哪一条出错
        std::memcpy(out.data() + size - 4, &i, 4);

        if (!sendAll(fd, out.data(), out.size())) {
            break;
        }
        if (!recvAll(fd, in.data(), in.size())) {
            break;
        }

        if (std::memcmp(out.data(), in.data(), out.size()) == 0) {
            ++okCount;
        } else {
            ++badCount;
            if (badCount <= 3) {
                std::fprintf(stderr, "第 %d 条回显内容不一致\n", i);
            }
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(end - begin).count();
    const double bytes = (double)okCount * size * 2;  // 发送 + 接收

    std::printf("结果: 成功 %d 条，失败 %d 条\n", okCount, badCount);
    std::printf("耗时: %.3f 秒\n", sec);
    if (sec > 0) {
        std::printf("吞吐: %.2f MiB/s，%.0f 次往返/秒\n", bytes / 1024.0 / 1024.0 / sec, okCount / sec);
        std::printf("平均往返延迟: %.1f 微秒\n", sec * 1e6 / (okCount > 0 ? okCount : 1));
    }

    getchar();

    net::closeSocket(fd);
    net::cleanupNetwork();
    return (badCount == 0 && okCount == count) ? 0 : 1;
}
