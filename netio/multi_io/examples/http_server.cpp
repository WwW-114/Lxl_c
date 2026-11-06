// ============================================================================
//  http_server.cpp —— 基于 HttpServer 的示例服务器
//
//  对比一下 echo_server：那边三个回调就完事了，因为"回显"根本没有协议可言。
//  这边演示的是**有了应用层协议之后**业务代码该长什么样：
//  只注册路由和静态目录，剩下的（拆包、粘包、keep-alive、Content-Length、
//  错误状态码、目录穿越防护）全部由 net::http 那一层处理掉了。
//
//  用法: http_server [端口] [后端] [触发模式] [静态目录]
//        例如: http_server 8080 auto lt www
// ============================================================================

#include "net/http/HttpServer.h"
#include "net/http/HttpUtil.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    // 日志实时输出。默认 stdout 重定向到文件时是全缓冲，
    // 进程被 kill 时缓冲区里的日志会全部丢失。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    uint16_t port = 8080;
    std::string backendArg = "auto";
    std::string trigArg = "lt";
    std::string staticDir = "www";
    if (argc > 1) port = (uint16_t)std::atoi(argv[1]);
    if (argc > 2) backendArg = argv[2];
    if (argc > 3) trigArg = argv[3];
    if (argc > 4) staticDir = argv[4];

    net::http::HttpServer::Options opts;
    if (backendArg == "select") {
        opts.tcp.backend = net::EventPoller::Backend::Select;
    } else if (backendArg == "poll") {
        opts.tcp.backend = net::EventPoller::Backend::Poll;
    } else if (backendArg == "epoll") {
        opts.tcp.backend = net::EventPoller::Backend::Epoll;
    }
    opts.tcp.edgeTriggered = (trigArg == "et");
    opts.serverName = "netio-http/1.0";

    net::http::HttpServer server(opts);

    // ---- 静态资源 ---------------------------------------------------------
    // 请求路径会按这个目录去找文件（只处理 GET / HEAD）。
    // 里面的目录穿越防护、Content-Type 推断、HEAD 处理都在库内部完成。
    server.setStaticRoot(staticDir);

    // ---- 动态路由 ---------------------------------------------------------

    // 最小示例：证明"响应是代码现算出来的"
    server.get("/hello", [](const net::http::HttpRequest& req, net::http::HttpResponse& resp) {
        resp.setContentType("text/html; charset=utf-8");
        resp.setBody("<h1>Hello, HTTP!</h1>\n"
                     "<p>这条响应是 /hello 路由现算出来的。</p>\n"
                     "<p>你请求的是 <code>" + req.path + "</code>，"
                     "用的是 <code>" + req.method + "</code>。</p>\n");
    });

    // 返回 JSON：演示按查询串做简单分支
    server.get("/api/time", [](const net::http::HttpRequest& req, net::http::HttpResponse& resp) {
        (void)req;
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

        resp.setContentType("application/json; charset=utf-8");
        resp.setBody(std::string("{\"utc\":\"") + buf + "\",\"unix\":" + std::to_string((long long)t) +
                     "}\n");
    });

    // 回显 POST 上来的正文：演示**读请求体**。
    // 能看到 body 说明服务器正确按 Content-Length 收满了数据 ——
    // 如果解析器把正文长度算错了，这里拿到的内容就会是残缺或者错位的。
    server.post("/api/echo", [](const net::http::HttpRequest& req, net::http::HttpResponse& resp) {
        const std::string* ct = req.findHeader("content-type");
        resp.setContentType("application/json; charset=utf-8");
        resp.setBody("{\"received\":" + std::to_string(req.body.size()) + ",\"contentType\":\"" +
                     jsonEscape(ct != nullptr ? *ct : "") + "\",\"body\":\"" +
                     jsonEscape(req.body) + "\"}\n");
    });

    // 生成指定大小的数据，专门用来测试**大响应**。
    //
    // 为什么需要它：响应一旦超过内核 socket 发送缓冲区（Windows 上通常
    // 只有 64 KiB），一次 send 就必然写不完，剩下的会进应用层发送缓冲区，
    // 由后续的可写事件慢慢续发 —— 这正是"发完再关连接"那条最容易写错的路径。
    // 没有一个大响应，这条路径在测试里根本走不到。
    //
    // 数据内容是由位置决定的确定性模式（i % 251），
    // 客户端可以逐字节校验，任何截断或错位都会被立刻发现。
    // 用法：GET /api/big?size=8388608
    server.get("/api/big", [](const net::http::HttpRequest& req, net::http::HttpResponse& resp) {
        // 从查询串里取 size 参数。真实项目应当用 URL 解码 + 表单解析，
        // 这里为了不引入依赖就手工找一下。
        size_t size = 8 * 1024 * 1024;
        const std::string key = "size=";
        const size_t pos = req.query.find(key);
        if (pos != std::string::npos) {
            const long long v = std::atoll(req.query.c_str() + pos + key.size());
            if (v > 0) {
                size = (size_t)v;
            }
        }
        if (size > 64 * 1024 * 1024) {
            size = 64 * 1024 * 1024;  // 上限，别把自己内存撑爆
        }

        std::string body(size, '\0');
        for (size_t i = 0; i < size; ++i) {
            body[i] = (char)(i % 251);
        }
        resp.setContentType("application/octet-stream");
        resp.setBody(std::move(body));
    });

    // 服务器自身状态：演示 HttpServer 暴露出来的查询接口
    server.get("/api/info", [&server](const net::http::HttpRequest& req, net::http::HttpResponse& resp) {
        (void)req;
        resp.setContentType("application/json; charset=utf-8");
        resp.setBody(std::string("{\"backend\":\"") + server.tcp().poller().name() +
                     "\",\"connections\":" + std::to_string(server.connectionCount()) +
                     ",\"keepAlive\":true}\n");
    });

    // 兜底：路由和静态文件都没命中时走到这里。
    // 顺手打一行访问日志，方便观察请求是怎么被消化的。
    std::atomic<uint64_t> notFound{0};
    server.setDefaultHandler([&notFound](const net::http::HttpRequest& req,
                                         net::http::HttpResponse& resp) {
        notFound.fetch_add(1);
        std::printf("[404] %s\n", req.requestLine().c_str());
        resp.setStatus(404);
        resp.setContentType("text/plain; charset=utf-8");
        resp.setBody("404 Not Found: " + req.path + "\r\n");
    });

    if (!server.listen("", port)) {
        return 1;
    }

    std::printf("HTTP 服务器已启动\n");
    std::printf("  地址     : http://127.0.0.1:%u\n", (unsigned)server.boundPort());
    std::printf("  复用后端 : %s\n", server.tcp().poller().name());
    std::printf("  触发模式 : %s\n", opts.tcp.edgeTriggered ? "边缘触发 ET" : "水平触发 LT");
    std::printf("  静态目录 : %s\n", staticDir.c_str());
    std::printf("  空闲超时 : %d ms\n", opts.idleTimeoutMs);
    std::printf("\n可用路由：\n");
    std::printf("  GET  /            静态文件（%s/index.html）\n", staticDir.c_str());
    std::printf("  GET  /hello       最小动态响应\n");
    std::printf("  GET  /api/time    当前 UTC 时间（JSON）\n");
    std::printf("  POST /api/echo    回显请求正文（JSON）\n");
    std::printf("  GET  /api/info    服务器状态（JSON）\n");
    std::printf("  GET  /api/big     生成指定大小的数据（?size=N），用于测试大响应\n");
    std::printf("\n浏览器打开上面的地址即可，按 Ctrl+C 退出\n\n");

    // 用带超时的 loopOnce 而不是 loop()，这样能在事件循环里顺带跑定时任务
    // （比如下面的统计输出）。真实的服务器就是在这个超时点上做心跳检测、
    // 踢掉空闲连接、刷新指标这类工作的。
    auto lastReport = std::chrono::steady_clock::now();
    while (server.running()) {
        server.loopOnce(1000);

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(30)) {
            lastReport = now;
            std::printf("[*] 当前在线 %zu 个连接，404 累计 %llu 次\n", server.connectionCount(),
                        (unsigned long long)notFound.load());
        }
    }

    net::cleanupNetwork();
    return 0;
}
