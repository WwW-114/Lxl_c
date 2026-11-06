// ============================================================================
//  http_client.cpp —— HTTP 服务器的裸 socket 测试客户端
//
//  为什么不用 curl？
//    因为下面这几条路径 curl **演示不了**，只有自己拼报文才做得出来：
//      * 拆包   —— 把一个请求拆成几段、中间停顿着发
//      * 粘包   —— 一次把两个请求塞进同一个 TCP 段
//      * 管线化 —— 不等响应就连续发多个请求
//      * 大响应 —— 验证"发送缓冲区 + 发完再关"这条最容易写错的路径
//
//  它同时也是服务器端解析逻辑的一面镜子：客户端这边一样会遇到
//  "响应分多次到达"的问题，只不过阻塞 IO 让它写起来轻松得多。
//
//  用法: http_client [主机] [端口] [大响应字节数]
//        例如: http_client 127.0.0.1 8080 8388608
//
//  前置条件：先启动 http_server，并保证它配的静态目录里有 index.html。
// ============================================================================

#include "net/SocketUtils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_lastError;  // 最近一次 recv 失败的原因，用于诊断

void report(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("  [通过] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [失败] %s    %s\n", name, detail.c_str());
    }
}

// ---------------------------------------------------------------------------
//  阻塞 socket 上的收发工具
// ---------------------------------------------------------------------------
bool sendAll(net::socket_t fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = ::send(fd, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) {
            const int err = net::socketError();
            if (net::isInterrupted(err)) {
                continue;
            }
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// 从 fd 上读满 n 个字节。返回 false 表示对端提前关闭了。
bool recvN(net::socket_t fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        const int r = ::recv(fd, buf + got, (int)(n - got), 0);
        if (r == 0) {
            return false;
        }
        if (r < 0) {
            const int err = net::socketError();
            if (net::isInterrupted(err)) {
                continue;
            }
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

// 读到一个完整的空行（CRLFCRLF）为止。
// 这里故意一个字节一个字节地读：图个简单可靠，也顺便让读者看清
// "报文的边界要靠自己找出来"这件事在两端是一模一样的。
bool readHeaderBlock(net::socket_t fd, std::string& block) {
    block.clear();
    char c = 0;
    while (true) {
        const int r = ::recv(fd, &c, 1, 0);
        if (r == 0) {
            g_lastError = "对端关闭连接（FIN），响应可能被截断";
            return false;
        }
        if (r < 0) {
            const int err = net::socketError();
            if (net::isInterrupted(err)) {
                continue;
            }
            g_lastError = "recv 失败: " + net::errorString(err);
            return false;
        }
        block.push_back(c);
        if (block.size() >= 4 && block.compare(block.size() - 4, 4, "\r\n\r\n") == 0) {
            return true;
        }
        if (block.size() > 64 * 1024) {
            return false;  // 头部大得离谱，八成是出问题了
        }
    }
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }
    return s;
}

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

// ---------------------------------------------------------------------------
//  读一个完整响应
//
//  注意 1xx：服务器可能在正式响应之前先塞一个 "100 Continue" 过来。
//  它是**中间响应**，不是最终结果，必须跳过继续读下一个 ——
//  客户端要是把它当成最终响应，就会把一个"还没有正文"的响应体当成结果。
// ---------------------------------------------------------------------------
bool readResponse(net::socket_t fd, Response& out) {
    for (;;) {
        std::string block;
        if (!readHeaderBlock(fd, block)) {
            return false;
        }

        // 去掉结尾的 CRLFCRLF，再按 CRLF 切成行
        block.resize(block.size() - 4);
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos <= block.size()) {
            size_t eol = block.find("\r\n", pos);
            if (eol == std::string::npos) {
                eol = block.size();
            }
            lines.push_back(block.substr(pos, eol - pos));
            if (eol == block.size()) {
                break;
            }
            pos = eol + 2;
        }
        if (lines.empty()) {
            return false;
        }

        out = Response{};

        // 状态行：HTTP/1.1 200 OK
        {
            const std::string& sl = lines[0];
            const size_t sp1 = sl.find(' ');
            if (sp1 == std::string::npos) {
                return false;
            }
            out.status = std::atoi(sl.c_str() + sp1 + 1);
        }

        for (size_t i = 1; i < lines.size(); ++i) {
            const size_t colon = lines[i].find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = toLowerAscii(lines[i].substr(0, colon));
            size_t v = colon + 1;
            while (v < lines[i].size() && lines[i][v] == ' ') {
                ++v;
            }
            out.headers[key] = lines[i].substr(v);
        }

        // 1xx 是中间响应，跳过，继续读真正的那一个
        if (out.status >= 100 && out.status < 200) {
            continue;
        }

        // ---- 读正文 ----
        auto it = out.headers.find("content-length");
        if (it != out.headers.end()) {
            const long long len = std::atoll(it->second.c_str());
            if (len < 0) {
                return false;
            }
            out.body.resize((size_t)len);
            if (len > 0 && !recvN(fd, &out.body[0], (size_t)len)) {
                return false;
            }
        } else {
            // 没有 Content-Length：只能一直读到连接关闭为止。
            // （我们这个服务器永远会给 Content-Length，这里只是兜底。）
            char buf[4096];
            for (;;) {
                const int r = ::recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) {
                    break;
                }
                out.body.append(buf, (size_t)r);
            }
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
//  建立一条阻塞连接
// ---------------------------------------------------------------------------
net::socket_t connectTo(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        return net::kInvalidSocket;
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
    return fd;
}

// fd 在 ms 毫秒内是否变得可读（用来断言"服务器没有提前响应"）
bool readableWithin(net::socket_t fd, int ms) {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#ifdef _WIN32
    const int n = ::select(0, &rset, nullptr, nullptr, &tv);  // Windows 忽略第一个参数
#else
    const int n = ::select((int)fd + 1, &rset, nullptr, nullptr, &tv);
#endif
    return n > 0;
}

std::string buildRequest(const std::string& method, const std::string& path,
                         const std::string& extraHeaders = "", const std::string& body = "") {
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: test\r\n";
    req += extraHeaders;
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return req;
}

// ===========================================================================
//  场景 1：拆包 —— 一个请求分三次发，中间还歇一会儿
//
//  服务器必须老老实实等着，不能因为"收到点东西了"就抢答。
//  这条路径最容易被写错的地方是：把 recv 到的一段当成一个完整请求去解析。
// ===========================================================================
void scenarioSplit(const std::string& host, uint16_t port) {
    std::printf("\n[场景 1] 拆包：一个请求分 3 次发送\n");

    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report("建立连接", false, "连不上服务器");
        return;
    }

    const std::string req = buildRequest("GET", "/hello");
    const size_t a = req.size() / 3;
    const size_t b = req.size() * 2 / 3;

    sendAll(fd, req.substr(0, a));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // 关键断言：只发了三分之一，服务器**不应该**有任何响应
    const bool early = readableWithin(fd, 200);
    report("半截请求时服务器不应响应", !early,
           early ? "服务器提前回数据了 —— 它把不完整的请求当成了完整的" : "");

    sendAll(fd, req.substr(a, b - a));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    sendAll(fd, req.substr(b));

    Response resp;
    const bool ok = readResponse(fd, resp);
    report("分片发送后收到完整响应", ok && resp.status == 200,
           ok ? ("状态码 " + std::to_string(resp.status)) : "读响应失败");
    report("正文非空", ok && !resp.body.empty(), "");

    net::closeSocket(fd);
}

// ===========================================================================
//  场景 2：粘包 / 管线化 —— 一次 send 塞两个完整请求
//
//  服务器必须在一次 onMessage 里把两个请求都解析出来，
//  并且**按顺序**回两个响应。
// ===========================================================================
void scenarioPipeline(const std::string& host, uint16_t port) {
    std::printf("\n[场景 2] 粘包 / 管线化：一次发送两个请求\n");

    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report("建立连接", false, "连不上服务器");
        return;
    }

    const std::string both = buildRequest("GET", "/hello") + buildRequest("GET", "/api/time");
    sendAll(fd, both);

    Response r1;
    Response r2;
    const bool ok1 = readResponse(fd, r1);
    const bool ok2 = readResponse(fd, r2);

    report("第一个响应", ok1 && r1.status == 200,
           ok1 ? ("状态码 " + std::to_string(r1.status)) : "读不到");
    report("第二个响应按序到达", ok2 && r2.status == 200,
           ok2 ? ("状态码 " + std::to_string(r2.status)) : "读不到");

    // /hello 返回 HTML，/api/time 返回 JSON。
    // 顺序如果反了，说明服务器把两个请求的响应发串了。
    const bool orderOk = ok1 && ok2 && r1.body.find("Hello") != std::string::npos &&
                         r2.body.find("\"utc\"") != std::string::npos;
    report("两个响应的顺序和内容都对得上", orderOk, "响应与请求的顺序不匹配");

    net::closeSocket(fd);
}

// ===========================================================================
//  场景 3：keep-alive —— 同一条连接上连着发 3 个请求
//
//  服务器必须在处理完一个请求后，把解析状态复位但**保留缓冲区里的剩余字节**，
//  否则第二个请求就会石沉大海。
// ===========================================================================
void scenarioKeepAlive(const std::string& host, uint16_t port) {
    std::printf("\n[场景 3] keep-alive：同一连接上串行发 3 个请求\n");

    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report("建立连接", false, "连不上服务器");
        return;
    }

    int okCount = 0;
    bool sawKeepAlive = false;
    for (int i = 0; i < 3; ++i) {
        sendAll(fd, buildRequest("GET", "/hello"));
        Response resp;
        if (readResponse(fd, resp) && resp.status == 200) {
            ++okCount;
        }
        auto it = resp.headers.find("connection");
        if (it != resp.headers.end() && toLowerAscii(it->second).find("keep-alive") != std::string::npos) {
            sawKeepAlive = true;
        }
    }

    report("3 个请求全部在一条连接上完成", okCount == 3,
           "成功 " + std::to_string(okCount) + "/3");
    report("响应里明确写了 Connection: keep-alive", sawKeepAlive, "");

    net::closeSocket(fd);
}

// ===========================================================================
//  场景 4：Connection: close —— 服务器必须回完就关
//
//  这里同时验证了那个最容易写错的时序：服务器是"发完缓冲区里的数据之后"
//  才关的，而不是 send 完立刻关。所以我们应该能读到**完整**的响应，
//  然后才看到 recv 返回 0。
// ===========================================================================
void scenarioClose(const std::string& host, uint16_t port) {
    std::printf("\n[场景 4] Connection: close：响应完整送达后连接关闭\n");

    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report("建立连接", false, "连不上服务器");
        return;
    }

    sendAll(fd, buildRequest("GET", "/hello", "Connection: close\r\n"));

    Response resp;
    const bool ok = readResponse(fd, resp);
    report("拿到完整响应", ok && resp.status == 200 && !resp.body.empty(),
           ok ? "正文为空" : "读响应失败");

    auto it = resp.headers.find("connection");
    report("响应里声明了 Connection: close",
           it != resp.headers.end() && toLowerAscii(it->second) == "close", "");

    // 服务器应该已经关了连接：继续 recv 会立刻返回 0
    char tmp = 0;
    const int r = ::recv(fd, &tmp, 1, 0);
    report("对端已关闭连接（recv 返回 0）", r == 0, "recv 返回 " + std::to_string(r));

    net::closeSocket(fd);
}

// ===========================================================================
//  场景 5：大响应
//
//  这是**专门用来验证发送缓冲区与 writeComplete 时序**的一条。
//  响应远大于内核 socket 发送缓冲区，一次 send 必然写不完，
//  服务器必须把剩下的缓存起来、在可写事件里续发、全部发完之后才关连接。
//
//  这里直接请求 Connection: close ——
//  如果"发完再关"的时序写错了（send 完就立刻 close），
//  客户端会收到一个被拦腰截断的正文，下面的长度与内容校验立刻就会失败。
// ===========================================================================
void scenarioBigResponse(const std::string& host, uint16_t port, size_t bigSize) {
    std::printf("\n[场景 5] 大响应：请求 %zu 字节（远大于内核发送缓冲区）\n", bigSize);

    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report("建立连接", false, "连不上服务器");
        return;
    }

    const std::string path = "/api/big?size=" + std::to_string(bigSize);
    sendAll(fd, buildRequest("GET", path, "Connection: close\r\n"));

    Response resp;
    const bool ok = readResponse(fd, resp);
    if (!ok) {
        report("读到完整响应", false, "读响应失败或被截断");
        net::closeSocket(fd);
        return;
    }

    report("状态码 200", resp.status == 200, "状态码 " + std::to_string(resp.status));

    // 长度校验：Content-Length 与实际收到的字节数必须一致
    report("正文长度正确", resp.body.size() == bigSize,
           "期望 " + std::to_string(bigSize) + "，实收 " + std::to_string(resp.body.size()));

    // 内容校验：逐字节比对服务器生成的确定性模式 (i % 251)。
    // 长度对了但内容错了，说明数据在中途被错位或者覆盖了。
    size_t badAt = std::string::npos;
    if (resp.body.size() == bigSize) {
        for (size_t i = 0; i < bigSize; ++i) {
            if (resp.body[i] != (char)(i % 251)) {
                badAt = i;
                break;
            }
        }
    }
    report("正文内容逐字节正确", badAt == std::string::npos,
           badAt != std::string::npos ? ("第 " + std::to_string(badAt) + " 字节不匹配") : "");

    net::closeSocket(fd);
}

// ===========================================================================
//  场景 6：畸形请求 —— 各种错误姿势都必须被挡住
//
//  每一条都要单独开一条连接：服务器在回完错误响应后会主动关闭连接，
//  因为出错的报文已经把字节流"污染"了，无法再确定下一个请求的边界。
// ===========================================================================
void checkBadRequest(const std::string& name, const std::string& host, uint16_t port,
                     const std::string& rawRequest, int expectStatus) {
    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report(name.c_str(), false, "连不上服务器");
        return;
    }

    // 对于"超长"这类用例，rawRequest 可能很大，用 sendAll 循环发完
    sendAll(fd, rawRequest);

    g_lastError.clear();
    Response resp;
    const bool ok = readResponse(fd, resp);
    report(name.c_str(), ok && resp.status == expectStatus,
           ok ? ("期望 " + std::to_string(expectStatus) + "，实际 " + std::to_string(resp.status))
              : ("读响应失败 — " + g_lastError));

    net::closeSocket(fd);
}

void scenarioMalformed(const std::string& host, uint16_t port) {
    std::printf("\n[场景 6] 畸形请求：逐个验证错误状态码\n");

    // ① 版本不是 1.0 / 1.1 → 505
    checkBadRequest("非 HTTP/1.x 版本 → 505", host, port,
                    "GET / HTTP/2.0\r\nHost: t\r\n\r\n", 505);

    // ② HTTP/1.1 没有 Host → 400（RFC 7230 §5.4 的强制要求）
    checkBadRequest("HTTP/1.1 缺 Host → 400", host, port, "GET / HTTP/1.1\r\n\r\n", 400);

    // ③ 请求行超过 8 KiB → 414
    //
    // 【为什么必须给到 20000 字节这么夸张】
    // 行长度限制只在"这一行还没结束、已经攒了太多字节"时才生效。
    // 而 TcpServer 每次 recv 最多读 4096 字节，也就是说：
    //     第 1 次 recv 后攒了  4096 字节  → 还没到 8192，继续等
    //     第 2 次 recv 后攒了  8192 字节  → 刚好等于 8192，仍然继续等
    //     第 3 次 recv 后攒了 12288 字节  → 超过 8192，判定 414
    // 所以请求行必须长到"第三次 recv 时依然没有 CRLF"，也就是 > 12288 字节。
    // 如果只写 9000 字节，第三次 recv 时整行连同 CRLF 已经到齐，
    // 服务器会正常解析它，然后回一个 404 —— 那就测不到限额这条路径了。
    {
        std::string req = "GET /" + std::string(20000, 'a') + " HTTP/1.1\r\nHost: t\r\n\r\n";
        checkBadRequest("请求行超长 → 414", host, port, req, 414);
    }

    // ④ 头部总量超过 16 KiB → 431
    {
        std::string req = "GET / HTTP/1.1\r\nHost: t\r\n";
        for (int i = 0; i < 300; ++i) {
            req += "X-Pad-" + std::to_string(i) + ": " + std::string(100, 'b') + "\r\n";
        }
        req += "\r\n";
        checkBadRequest("头部总量超限 → 431", host, port, req, 431);
    }

    // ⑤ Transfer-Encoding 与 Content-Length 同时出现 → 400（请求走私）
    checkBadRequest("TE + CL 同时出现（请求走私）→ 400", host, port,
                    "POST /api/echo HTTP/1.1\r\nHost: t\r\n"
                    "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello",
                    400);

    // ⑥ Content-Length 不是纯数字 → 400
    checkBadRequest("Content-Length 非法 → 400", host, port,
                    "POST /api/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 5x\r\n\r\nhello", 400);

    // ⑦ 请求体超过 1 MiB 限额 → 413
    checkBadRequest("请求体超限 → 413", host, port,
                    "POST /api/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 2097152\r\n\r\n", 413);

    // ⑧ 头部名和冒号之间有空格 → 400（RFC 7230 §3.2.4 强制要求）
    checkBadRequest("头部名与冒号间有空格 → 400", host, port,
                    "GET / HTTP/1.1\r\nHost : t\r\n\r\n", 400);

    // ⑨ 目录穿越 → 403
    checkBadRequest("目录穿越 ../ → 403（或 404）", host, port,
                    "GET /../../etc/passwd HTTP/1.1\r\nHost: t\r\n\r\n", 403);

    // ⑩ 编码过的目录穿越（%2e%2e%2f）→ 同样要被挡住。
    //    这条专门用来验证"先解码再校验"这个顺序有没有搞反 ——
    //    顺序错了的话，检查时看到的是 "%2e%2e%2f"（无害），
    //    而实际打开文件时它已经变成 "../" 了。
    checkBadRequest("编码过的目录穿越 %2e%2e%2f → 403（或 404）", host, port,
                    "GET /%2e%2e%2f%2e%2e%2fetc/passwd HTTP/1.1\r\nHost: t\r\n\r\n", 403);
}

// ===========================================================================
//  场景 7：POST 带正文 + Expect: 100-continue
//
//  curl 对超过 1 KiB 的正文会自动加上 Expect: 100-continue，
//  然后**停下来等服务器回 "100 Continue"** 才肯发正文。
//  服务器要是不理它，客户端就得干等到自己的超时才补发 —— 表现为"莫名慢一秒"。
// ===========================================================================
void scenarioExpectContinue(const std::string& host, uint16_t port) {
    std::printf("\n[场景 7] Expect: 100-continue：等服务器点头再发正文\n");

    net::socket_t fd = connectTo(host, port);
    if (fd == net::kInvalidSocket) {
        report("建立连接", false, "连不上服务器");
        return;
    }

    const std::string body(2048, 'Z');  // 超过 1 KiB，模拟真实客户端的行为
    const std::string head = "POST /api/echo HTTP/1.1\r\nHost: test\r\n"
                             "Content-Type: text/plain\r\n"
                             "Expect: 100-continue\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

    // 先只发头部，正文捏在手里不发
    sendAll(fd, head);

    // 服务器应当很快回一个 100 Continue
    const bool gotSomething = readableWithin(fd, 1000);
    report("服务器回了 100 Continue", gotSomething, "服务器没有理会 Expect 头");

    if (gotSomething) {
        std::string block;
        const bool ok = readHeaderBlock(fd, block);
        report("100 Continue 格式正确", ok && block.find("100") != std::string::npos,
               ok ? ("收到: " + block.substr(0, block.find("\r\n"))) : "读失败");
    }

    // 现在才把正文发出去
    sendAll(fd, body);

    Response resp;
    const bool ok = readResponse(fd, resp);
    const bool bodyOk =
        ok && resp.body.find("\"received\":" + std::to_string(body.size())) != std::string::npos;
    report("服务器按 Content-Length 收满了正文", bodyOk,
           ok ? ("响应: " + resp.body.substr(0, 120)) : "读响应失败");

    net::closeSocket(fd);
}

}  // namespace

// ===========================================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    net::initNetwork();

    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    size_t bigSize = 8 * 1024 * 1024;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = (uint16_t)std::atoi(argv[2]);
    if (argc > 3) bigSize = (size_t)std::atoll(argv[3]);
    if (bigSize < 1024 * 1024) {
        bigSize = 1024 * 1024;  // 太小就压不出"部分写"的场景了
    }

    std::printf("HTTP 测试客户端\n");
    std::printf("  目标: %s:%u\n", host.c_str(), (unsigned)port);
    std::printf("  大响应: %zu 字节\n", bigSize);

    scenarioSplit(host, port);
    scenarioPipeline(host, port);
    scenarioKeepAlive(host, port);
    scenarioClose(host, port);
    scenarioBigResponse(host, port, bigSize);
    scenarioExpectContinue(host, port);
    scenarioMalformed(host, port);

    std::printf("\n================================================\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    std::printf("================================================\n");

    net::cleanupNetwork();
    return g_fail == 0 ? 0 : 1;
}
