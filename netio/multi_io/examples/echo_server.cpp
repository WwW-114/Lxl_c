// ============================================================================
//  echo_server.cpp —— 基于 TcpServer 的回显服务器
//
//  对比一下：这里没有任何 select/poll/epoll 的痕迹，没有 accept 循环，
//  没有 recv 循环，没有发送缓冲区，没有 fd 集合的增删 —— 全部被封装在
//  EventPoller + TcpServer 两层里了。业务代码只剩三个回调。
//
//  用法: echo_server [端口] [auto|select|poll|epoll] [lt|et]
//        例如: echo_server 9000 epoll et
// ============================================================================

#include "net/TcpServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    // 日志实时输出。默认 stdout 重定向到文件时是全缓冲，进程被 kill 时
    // 缓冲区里的日志会全部丢失，调试时会误以为"服务器什么也没打印"。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    uint16_t port = 9000;
    std::string backendArg = "auto";
    std::string trigArg = "lt";
    if (argc > 1) {
        port = (uint16_t)std::atoi(argv[1]);
    }
    if (argc > 2) {
        backendArg = argv[2];
    }
    if (argc > 3) {
        trigArg = argv[3];
    }

    net::TcpServer::Options opts;
    if (backendArg == "select") {
        opts.backend = net::EventPoller::Backend::Select;
    } else if (backendArg == "poll") {
        opts.backend = net::EventPoller::Backend::Poll;
    } else if (backendArg == "epoll") {
        opts.backend = net::EventPoller::Backend::Epoll;
    }
    opts.edgeTriggered = (trigArg == "et");

    net::TcpServer server(opts);
    if (!server.listen("", port)) {
        return 1;
    }

    std::printf("回显服务器已启动\n");
    std::printf("  地址     : 0.0.0.0:%u\n", (unsigned)server.boundPort());
    std::printf("  复用后端 : %s\n", server.poller().name());
    std::printf("  触发模式 : %s\n", opts.edgeTriggered ? "边缘触发 ET" : "水平触发 LT");
    std::printf("  按 Ctrl+C 退出\n\n");

    std::atomic<uint64_t> totalConns{0};
    std::atomic<uint64_t> totalBytes{0};

    server.setOnConnect([&](net::socket_t fd) {
        totalConns.fetch_add(1);
        std::printf("[+] 新连接 fd=%lld（当前 %zu 个）\n", (long long)fd, server.connections().size());
    });

    server.setOnMessage([&](net::socket_t fd, const char* data, size_t len) {
        totalBytes.fetch_add(len);
        // 原样回发。TcpServer 内部负责处理“没发完”的情况。
        server.send(fd, data, len);
    });

    server.setOnClose([&](net::socket_t fd) {
        std::printf("[-] 连接关闭 fd=%lld（当前 %zu 个）\n", (long long)fd, server.connections().size());
    });

    // 用带超时的 loopOnce 而不是 loop()，这样能在事件循环里顺带跑定时任务。
    // 真实的服务器就是在这个超时点上做心跳检测、踢掉超时空闲连接等工作的。
    // 注意不能"每次循环都打印统计"—— 有数据流动时 loopOnce 会立刻返回，
    // 那样会变成刷屏；必须自己用时间戳控制节奏。
    auto lastReport = std::chrono::steady_clock::now();
    while (server.running()) {
        server.loopOnce(1000);

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(5)) {
            lastReport = now;
            std::printf("[*] 累计连接 %llu 次，累计收发 %llu 字节，当前在线 %zu\n",
                        (unsigned long long)totalConns.load(), (unsigned long long)totalBytes.load(),
                        server.connections().size());
            std::fflush(stdout);
        }
    }

    net::cleanupNetwork();
    return 0;
}
