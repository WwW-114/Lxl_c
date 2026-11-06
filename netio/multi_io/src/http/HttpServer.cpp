#include "net/http/HttpServer.h"

#include "net/http/HttpUtil.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace net {
namespace http {

namespace {

// 对 "Expect: 100-continue" 的应答。
// 注意 1xx 响应是**没有 Content-Length 的**：它靠"空行"就结束了 ——
// 这是 HTTP 里唯一一类不需要正文长度也能定界的响应
// （RFC 7230 §3.3.3 规定：1xx / 204 / 304 都不带正文）。
constexpr const char* kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";

// 把 URL 路径变成一段安全的**相对路径**。不合法就返回 false。
//
// 这是静态文件服务里最需要小心的一个函数 —— 它挡的是"目录穿越"
// （path traversal）：攻击者想让服务器去读静态根目录之外的文件。
bool sanitizePath(const std::string& urlPath, std::string& rel) {
    // 【检查一】路径里不能有 NUL 字节。
    // 这个坑很隐蔽：std::string 是能装 NUL 的，但只要把它交给任何一个
    // 只认 C 字符串的接口（fopen、std::ifstream 的 const char* 重载、系统调用），
    // 内容就会在 NUL 处被截断。
    // 于是会出现"我们检查的字符串"和"真正被打开的文件"根本不是一个东西的局面：
    //     检查的是 "a.txt\0../../etc/passwd"  → 看着很老实
    //     打开的是 "a.txt"                   → 但有的实现会继续往后读
    // 这类"检查与使用不一致"（TOCTOU 的近亲）是路径绕过里最经典的一招，直接拒掉。
    if (urlPath.find('\0') != std::string::npos) {
        return false;
    }

    // 按路径分隔符切成一段一段，逐段审查。
    std::vector<std::string> parts;
    std::string cur;
    for (char c : urlPath) {
        // 【检查二】反斜杠也要当成分隔符。
        // 在 Windows 上 '\' 和 '/' 一样是合法的路径分隔符，只按 '/' 切分的话，
        // "..\\..\\windows\\win.ini" 会被当成一个普普通通的文件名放行，
        // 然后交给文件系统 API 时它才现出原形。
        const bool isSep = (c == '/' || c == '\\');
        if (isSep) {
            if (cur.empty() || cur == ".") {
                cur.clear();
                continue;
            }
            if (cur == "..") {
                return false;  // 【检查三】目录穿越，直接拒绝
            }
            parts.push_back(cur);
            cur.clear();
            continue;
        }

        // 【检查四】':' 在 Windows 上可以构成盘符（"C:"）或 NTFS 数据流
        // （"file.txt:hidden"），一律拒绝，免得路径跑到根目录外面去。
        if (c == ':') {
            return false;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        if (cur == "..") {
            return false;
        }
        if (cur != ".") {
            parts.push_back(cur);
        }
    }

    // 拼回一个用 '/' 连接的正规相对路径
    rel.clear();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            rel += '/';
        }
        rel += parts[i];
    }

    // "/" 或者 "/subdir/" 这种以分隔符结尾的请求，按惯例去要目录下的 index.html
    const bool endsWithSep = !urlPath.empty() && (urlPath.back() == '/' || urlPath.back() == '\\');
    if (rel.empty() || endsWithSep) {
        rel += (rel.empty() ? "" : "/");
        rel += "index.html";
    }
    return true;
}

// fileCanon 是否位于 rootCanon 之内（按路径分量逐个比较）
bool isInsideRoot(const std::string& rootCanon, const std::string& fileCanon) {
    const std::filesystem::path root(rootCanon);
    const std::filesystem::path file(fileCanon);

    auto rit = root.begin();
    auto fit = file.begin();
    for (; rit != root.end() && fit != file.end(); ++rit, ++fit) {
        if (*rit != *fit) {
            return false;
        }
    }
    // root 还有没走完的分量，说明 file 比 root 还"浅"，不可能是它的子路径。
    // 注：Windows 的路径比较其实是大小写不敏感的，这里的比较是逐字符的。
    // 由于两个路径都是用同一个 root 前缀算出来的，实际不会因为大小写误判；
    // 真要做到万无一失，应当在 Windows 上改用大小写不敏感的比较。
    return rit == root.end();
}

}  // namespace

// ---------------------------------------------------------------------------
//  构造：把底层 TcpServer 的四个回调全部接管过来
// ---------------------------------------------------------------------------
HttpServer::HttpServer(Options opts) : opts_(opts), tcp_(opts.tcp) {
    // 用 lambda 把成员函数包一层挂上去。
    // 这里捕获 this 是安全的：tcp_ 是 HttpServer 的成员，
    // 生命周期不会长过 HttpServer 自己。
    tcp_.setOnConnect([this](socket_t fd) { onConnect(fd); });
    tcp_.setOnMessage([this](socket_t fd, const char* data, size_t len) {
        onMessage(fd, data, len);
    });
    tcp_.setOnClose([this](socket_t fd) { onClose(fd); });
    tcp_.setOnWriteComplete([this](socket_t fd) { onWriteComplete(fd); });
}

HttpContext::Limits HttpServer::limits() const {
    HttpContext::Limits lim;
    lim.maxLineBytes = opts_.maxLineBytes;
    lim.maxHeaderBytes = opts_.maxHeaderBytes;
    lim.maxBodyBytes = opts_.maxBodyBytes;
    return lim;
}

bool HttpServer::listen(const std::string& ip, uint16_t port) {
    return tcp_.listen(ip, port);
}

// ---------------------------------------------------------------------------
//  事件循环
// ---------------------------------------------------------------------------
void HttpServer::loopOnce(int timeoutMs) {
    tcp_.loopOnce(timeoutMs);
    closeIdleConnections();
}

void HttpServer::loop() {
    // 这里**不用** tcp_.loop()（那个是永久阻塞）。
    // 用 1 秒的分段超时，才有机会定期跑空闲连接回收。
    // 真实服务器的心跳、超时踢人、限流窗口，全都挂在这样一个"周期性超时点"上。
    while (tcp_.running()) {
        loopOnce(1000);
    }
}

// ---------------------------------------------------------------------------
//  连接建立 / 关闭
// ---------------------------------------------------------------------------
void HttpServer::onConnect(socket_t fd) {
    // 每条连接配一个独立的解析状态机
    states_.try_emplace(fd, limits());
}

void HttpServer::onClose(socket_t fd) {
    // 【必须清理】per-connection 的上下文。
    // 漏掉这两行会出两个问题：
    //   1) 内存泄漏 —— 客户端反复重连（尤其在压测或攻击下）会稳定地涨内存；
    //   2) 更隐蔽的：map 的 key 是 fd，而 fd 会被内核回收复用。
    //      新连接拿到同一个 fd 号后，会**继承**上一个连接的解析状态 ——
    //      表现为"新连接莫名其妙收到一个 400"。这种 bug 极难查。
    states_.erase(fd);
    closing_.erase(fd);
}

void HttpServer::onWriteComplete(socket_t fd) {
    // 只有"已经决定关闭、正等缓冲区排空"的连接才需要动作。
    // 正常的长连接写完一个响应后什么都不用做。
    if (closing_.count(fd) != 0) {
        tcp_.closeConnection(fd);
    }
}

// ---------------------------------------------------------------------------
//  收到数据 —— 整个 HTTP 层的心脏
// ---------------------------------------------------------------------------
void HttpServer::onMessage(socket_t fd, const char* data, size_t len) {
    {
        auto it = states_.find(fd);
        if (it == states_.end()) {
            return;  // 不该发生；防御性处理
        }
        it->second.lastActive = std::chrono::steady_clock::now();
        it->second.ctx.append(data, len);
    }

    // 【这里是关键】一次 recv 中的数据可能包含：
    //     0 个完整请求 → 拆包：请求还没到齐，等下一次 recv
    //     1 个完整请求 → 最常见的情况
    //     多个完整请求 → 粘包 / HTTP 管线化：客户端没等响应就一口气发了几个
    // 所以必须"循环解析"，而不是"解析一次"。
    for (;;) {
        // 每一轮都重新找一次：上一轮的 sendResponse 有可能已经把连接关掉，
        // 那样 states_ 里的条目就没了，继续用旧引用就是悬垂引用。
        auto sit = states_.find(fd);
        if (sit == states_.end()) {
            return;
        }

        const HttpContext::Result r = sit->second.ctx.parse();

        if (r == HttpContext::Result::Incomplete) {
            return;  // 数据不够，安心等下一次 recv
        }

        if (r == HttpContext::Result::ExpectContinue) {
            // 客户端在等我们说"可以发正文了"。先应它一声，然后继续解析 ——
            // 正文马上就会到，不能再 return 出去。
            tcp_.send(fd, kContinueResponse);
            continue;
        }

        if (r == HttpContext::Result::Error) {
            const int code = sit->second.ctx.errorCode();
            HttpResponse resp;
            fillText(resp, code, std::string(statusMessage(code)) + "\r\n");
            // 出错的连接必须关：报文已经把字节流"污染"了，
            // 我们再也无法确定下一个请求从哪儿开始，
            // 硬要复用这条连接就等于把后续的字节流交给一个错位的解析器去猜。
            resp.setCloseConnection(true);
            sendResponse(fd, sit->second.ctx.request(), resp);
            return;
        }

        // ---- Complete：交给业务处理 ----
        HttpResponse resp;
        handleRequest(sit->second.ctx.request(), resp);
        sendResponse(fd, sit->second.ctx.request(), resp);

        // sendResponse 可能已经把连接关掉（响应是 close 语义时），
        // 也可能只是打了标记、等缓冲区排空。
        // 两种情况都不该再解析后面的请求了：
        //   * 前者 states_ 里已经没有这个 fd；
        //   * 后者连接已经是"待关闭"状态，继续处理它毫无意义。
        auto again = states_.find(fd);
        if (again == states_.end() || closing_.count(fd) != 0) {
            return;
        }

        // 为同一个连接上的下一个请求做准备。
        // reset() 只清解析状态，缓冲区里属于后续请求的字节原样保留 ——
        // 这就是 keep-alive 和管线化能工作的原因。
        again->second.ctx.reset();
    }
}

// ---------------------------------------------------------------------------
//  路由与静态文件
// ---------------------------------------------------------------------------
void HttpServer::route(const std::string& method, const std::string& path, Handler h) {
    routes_[method + " " + path] = std::move(h);
}

void HttpServer::setStaticRoot(const std::string& dir) {
    staticRoot_ = dir;

    // 预先算好根目录的规范化形式，用于后面的符号链接逃逸检查。
    // weakly_canonical 会把 "." ".." 和已存在的符号链接都解析掉；
    // 对不存在的部分则原样保留在末尾（"weakly" 的含义）。
    std::error_code ec;
    const auto p = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec);
    staticRootCanon_ = ec ? std::string() : p.string();
}

void HttpServer::fillText(HttpResponse& resp, int code, const std::string& text) {
    resp.setStatus(code);
    // 【必须显式指定 text/plain】错误正文里会回显用户给出的路径，
    // 如果这里写成 text/html，浏览器就会把请求路径当成 HTML 去渲染 ——
    // 请求 "/<script>alert(1)</script>" 就变成了一个反射型 XSS。
    // 回显用户输入时，要么用 text/plain，要么老老实实做 HTML 转义。
    resp.setContentType("text/plain; charset=utf-8");
    resp.setBody(text);
}

void HttpServer::handleRequest(const HttpRequest& req, HttpResponse& resp) {
    // ① 精确路由匹配。key 是 "方法 + 空格 + 路径"，路径不含查询串。
    auto it = routes_.find(req.method + " " + req.path);

    // HEAD 回退到同路径的 GET 处理器。
    // RFC 7231 §4.3.2 规定：**支持 GET 的服务器必须同时支持 HEAD**，
    // 两者语义完全一样，唯一的区别是 HEAD 的响应里不能有正文
    // （但 Content-Length 要如实给出，客户端靠它判断资源大小）。
    // 所以业务代码只需要注册 GET，HEAD 自动就能用。
    bool headFallback = false;
    if (it == routes_.end() && req.method == "HEAD") {
        it = routes_.find("GET " + req.path);
        headFallback = (it != routes_.end());
    }

    if (it != routes_.end()) {
        it->second(req, resp);
        // 放在处理器跑完之后再设置：处理器可能自己调用了 resp.clear()，
        // 提前设会被它一并清掉。
        if (headFallback) {
            resp.suppressBody(true);
        }
        return;
    }

    // ② 静态文件（内部会处理目录穿越、文件类型检查等）
    if (serveStaticFile(req, resp)) {
        return;
    }

    // ③ 路径对得上、只是方法用错了 → 405。
    //    比笼统回 404 更准确，也顺手告诉客户端该用什么方法（Allow 头）。
    if (fillMethodNotAllowed(req, resp)) {
        return;
    }

    // ④ 兜底
    if (defaultHandler_) {
        defaultHandler_(req, resp);
        return;
    }
    fillText(resp, 404, "404 Not Found: " + req.path + "\r\n");
}

bool HttpServer::fillMethodNotAllowed(const HttpRequest& req, HttpResponse& resp) const {
    // 在路由表里找"路径部分相同、方法不同"的所有条目，汇总出 Allow 头。
    // 路由数量通常只有几十个，扫一遍的开销可以忽略，
    // 不必为此再维护一份额外的索引。
    const std::string suffix = " " + req.path;
    std::string allow;
    bool hasGet = false;
    for (const auto& kv : routes_) {
        const std::string& key = kv.first;
        if (key.size() > suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            const std::string method = key.substr(0, key.size() - suffix.size());
            if (!allow.empty()) {
                allow += ", ";
            }
            allow += method;
            hasGet = hasGet || (method == "GET");
        }
    }

    if (allow.empty()) {
        return false;
    }

    // 注册了 GET 就等于也支持 HEAD（见 handleRequest 里的说明），
    // 所以 Allow 里必须把它一并列出来，否则客户端会误以为 HEAD 不被支持。
    if (hasGet && req.method != "HEAD") {
        allow += ", HEAD";
    }

    // 405 响应**必须**带 Allow 头（RFC 7231 §6.5.5），
    // 否则客户端只知道"不许"，不知道"该用什么"。
    resp.setHeader("Allow", allow);
    fillText(resp, 405, "405 Method Not Allowed，允许的方法: " + allow + "\r\n");
    return true;
}

bool HttpServer::serveStaticFile(const HttpRequest& req, HttpResponse& resp) {
    if (staticRoot_.empty()) {
        return false;  // 没配静态目录，这条线不走
    }
    // 只有 GET / HEAD 能取文件。用 POST 去读文件既不合语义，
    // 也容易被当成 CSRF 的跳板。
    if (req.method != "GET" && req.method != "HEAD") {
        return false;
    }

    // ---- 1. 把 URL 路径变成一个安全的相对路径 -----------------------------
    // 【顺序至关重要】req.path 已经在 HttpContext 里做过百分号解码了。
    // 校验必须发生在**解码之后** —— 否则 "%2e%2e%2f" 这种写法能大摇大摆地
    // 走过检查，等解码完才现出 "../" 的原形。
    std::string rel;
    if (!sanitizePath(req.path, rel)) {
        fillText(resp, 403, "403 Forbidden: 非法路径\r\n");
        return true;
    }

    std::string fullPath = staticRoot_ + "/" + rel;

    std::error_code ec;

    // ---- 2. 指向目录时，按惯例去要它下面的 index.html ---------------------
    // 比如请求 "/sub"（没有结尾的斜杠）。真实服务器这时会先回一个 301，
    // 让浏览器把地址改成 "/sub/"，这样页面里的相对链接才能正确解析；
    // 教学项目直接内部换成 index.html，少一次往返。
    if (std::filesystem::is_directory(fullPath, ec)) {
        fullPath += "/index.html";
        rel += "/index.html";
    }

    // ---- 3. 必须是一个"普通文件" -----------------------------------------
    // 为什么非查不可：如果这个路径指向一个 FIFO（命名管道）或者字符设备，
    // **读取它会永久阻塞**。而我们是单线程事件循环 ——
    // 一条连接卡在 read 上，整个服务器的所有连接就一起卡死了。
    // 这是"单线程 Reactor 做文件 IO"最凶险的坑，没有之一。
    if (!std::filesystem::is_regular_file(fullPath, ec)) {
        return false;  // 不存在 / 是目录 / 是特殊文件 → 交给 404 兜底
    }

    // ---- 4. 二次校验：符号链接逃逸 ---------------------------------------
    // 上面逐段检查 ".." 拦的是"路径里写着 .."的情况。
    // 但如果静态目录里有人放了一个**符号链接**指向 C:\Windows，
    // 路径里一个 ".." 都没有，照样能读到根目录之外的东西。
    // 所以要把最终路径规范化（解析掉所有链接），再确认它确实在根目录之内。
    if (!staticRootCanon_.empty()) {
        const auto fileCanon = std::filesystem::weakly_canonical(std::filesystem::path(fullPath), ec);
        if (!ec && !isInsideRoot(staticRootCanon_, fileCanon.string())) {
            fillText(resp, 403, "403 Forbidden: 越出静态根目录\r\n");
            return true;
        }
    }

    // ---- 5. 读文件 --------------------------------------------------------
    // 【必须带 std::ios::binary】
    // Windows 上文本模式会把写出的 "\n" 悄悄换成 "\r\n"（读取时反过来）。
    // 对二进制内容（图片、字体、压缩包）这是灾难性的破坏；
    // 更隐蔽的是，读出来的字节数会和文件真实大小**对不上**，
    // 于是我们报出去的 Content-Length 是在撒谎 ——
    // 客户端要么提前截断，要么一直等剩下的字节直到超时。
    std::ifstream in(fullPath, std::ios::binary);
    if (!in) {
        return false;
    }

    // 教学项目直接从内存里过一遍。
    // 真实服务器的做法完全不同：Linux 用 sendfile(2)、Windows 用 TransmitFile，
    // 让内核把文件页缓存直接 DMA 到网卡，一次用户态拷贝都不做，
    // 也不会在内存里放一整份文件副本。这是后面可以自己动手改进的方向。
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return false;  // 读出错（比如中途被删了）
    }

    resp.setStatus(200);
    resp.setContentType(mimeType(rel));
    resp.setBody(std::move(content));

    // HEAD 请求：上面算出来的 Content-Length 必须照常发出去
    // （客户端靠它判断资源大小），但正文一个字节都不能发。
    if (req.method == "HEAD") {
        resp.suppressBody(true);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  发送响应 —— 本层最容易写错的地方
// ---------------------------------------------------------------------------
void HttpServer::sendResponse(socket_t fd, const HttpRequest& req, HttpResponse& resp) {
    // ① 补齐每个响应都该有、业务代码又懒得管的三个头
    resp.setHeader("Date", httpDate());
    resp.setHeader("Server", opts_.serverName);

    // ② 定下这条连接的去留，并明确告诉客户端。
    //    【必须显式回 Connection 头】：虽然 HTTP/1.1 默认就是长连接，
    //    但客户端要等到读完整个响应才知道服务器打不打算关，
    //    显式写出来能让它立刻做出判断（比如决定复用还是另开一条）。
    const bool keepAlive = req.keepAlive() && !resp.closeConnection();
    resp.setHeader("Connection", keepAlive ? "keep-alive" : "close");

    const std::string out = resp.toString();

    // ③ 先把"发完就关"的标记打上，再发数据。
    //    在本项目里回调不会在 send 内部被同步触发（writeComplete 只发生在
    //    事件循环的可写事件里），所以两种顺序其实都对。
    //    但"先把状态标好、再动数据"是更稳妥的习惯：万一将来有人把回调改成
    //    可以同步触发，先 send 的写法就会漏掉这次关闭。
    if (!keepAlive) {
        closing_.insert(fd);
    }

    if (!tcp_.send(fd, out)) {
        return;  // 连接已经不在了
    }

    // ④ 关闭的时机 —— 整个 HTTP 层最容易踩的坑就在这里
    //
    //    ❌ 绝对不能 send 完就立刻 closeConnection()：
    //       非阻塞 socket 上一次 send 只保证"尽力而为"。大响应（比如一整个
    //       HTML 文件）必然只能发出去一部分，剩下的躺在 TcpServer 的
    //       应用层发送缓冲区里。这时 close 会把它们连同 fd 一起丢掉，
    //       客户端收到一个**被拦腰截断**的响应 —— 而且不报任何错误，
    //       单看日志一切正常，极难排查。
    //
    //    ✅ 正确做法是两条路配合：
    //       * 若缓冲区此刻已经空了（小响应一次就写完）→ 立刻关，安全；
    //       * 否则挂着，等 TcpServer 的 onWriteComplete 回调来关。
    //    两条路合起来覆盖全部情况，一条都不漏。
    if (!keepAlive && tcp_.sendBufferEmpty(fd)) {
        tcp_.closeConnection(fd);
    }

    // 注意：closeConnection 之后 fd 已经失效、req 也可能已经被销毁，
    // 所以本函数里所有对 req / resp 的使用都必须在这之前完成。
}

// ---------------------------------------------------------------------------
//  空闲连接回收
// ---------------------------------------------------------------------------
void HttpServer::closeIdleConnections() {
    if (opts_.idleTimeoutMs <= 0) {
        return;  // 用户关掉了这个功能
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<socket_t> expired;

    for (const auto& kv : states_) {
        const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - kv.second.lastActive)
                                .count();
        if (idleMs >= opts_.idleTimeoutMs) {
            expired.push_back(kv.first);
        }
    }

    // 先收集再统一关闭：绝不能在遍历 states_ 的过程中去删它 ——
    // closeConnection 会触发 onClose，而 onClose 里正是 states_.erase(fd)，
    // 边遍历边删容器是未定义行为。
    for (socket_t fd : expired) {
        // 这里直接关掉了。真实服务器通常会先发一个带 Connection: close 的
        // 响应（或者干脆静默关闭），教学项目从简。
        tcp_.closeConnection(fd);
    }
}

}  // namespace http
}  // namespace net
