#pragma once
// ============================================================================
//  HttpServer.h —— 把 TcpServer 的「字节流事件」翻译成「HTTP 请求」
//
//  这一层做的事情，本质上就是一次**语义升格**：
//
//      TcpServer 说的     ：fd 7 上有 573 个字节到啦
//      HttpServer 理解的  ：GET /index.html 来了，走静态文件，回 200
//
//  具体职责有四块：
//    1. 为每条连接维护一个 HttpContext（解析状态机），把字节流拼成完整请求；
//    2. 一次 recv 里可能有 0 / 1 / 多个请求，负责把它们全部消化掉
//       （拆包、粘包、HTTP 管线化都在这里被摆平）；
//    3. 路由分发 —— 精确匹配「方法 + 路径」，没命中的交给静态文件；
//    4. 统一补齐 Date / Server / Content-Length / Connection 这几个
//       每个响应都必须有、而业务代码又懒得管的头部。
//
//  注意它对下层的依赖只有 4 个回调 + send() + closeConnection()，
//  **完全不知道**底层用的是 select、poll 还是 epoll。
// ============================================================================

#include "net/TcpServer.h"
#include "net/http/HttpContext.h"
#include "net/http/HttpRequest.h"
#include "net/http/HttpResponse.h"

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace net {
namespace http {

class HttpServer {
public:
    // 业务处理器：拿到解析好的请求，往响应里填内容。
    // 一个 Handler 就是一个函数的形状，用 lambda 注册最方便。
    using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

    struct Options {
        // 底层 Reactor 的全部参数（后端选择 / 触发模式 / backlog …）原样透传，
        // 想换成 epoll 或者开 ET，改这里的 tcp 就行。
        TcpServer::Options tcp;

        // 下面三个是防"慢速攻击"的限额，含义见 HttpContext::Limits
        size_t maxLineBytes = 8 * 1024;
        size_t maxHeaderBytes = 16 * 1024;
        size_t maxBodyBytes = 1024 * 1024;

        std::string serverName = "netio-http/1.0";

        // keep-alive 连接的空闲超时（毫秒）。
        // 长连接会把"连接数"从"并发请求数"抬高到"并发客户端数"，
        // 而 Windows 的 select 后端受 FD_SETSIZE（本项目设为 1024）硬限制，
        // Linux 上每个连接也占一个 fd。没有超时回收，空闲连接会越攒越多，
        // 最后新连接直接连不上。<= 0 表示不超时。
        int idleTimeoutMs = 60 * 1000;
    };

    // 同 TcpServer：默认构造委托给带参版本，避开 GCC 对「嵌套结构体 +
    // 默认成员初始化器 + 默认实参 = {}」的缺陷。语义与原写法一致。
    HttpServer() : HttpServer(Options()) {}
    explicit HttpServer(Options opts);
    ~HttpServer() = default;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // ---- 生命周期 ---------------------------------------------------------

    // 绑定并监听。ip 为空表示监听所有网卡。
    bool listen(const std::string& ip, uint16_t port);

    // 跑一轮事件循环。timeoutMs < 0 表示一直阻塞到有事件为止。
    void loopOnce(int timeoutMs = -1);

    // 反复跑事件循环，直到 stop()
    void loop();

    void stop() { tcp_.stop(); }
    bool running() const { return tcp_.running(); }

    // ---- 路由注册 ---------------------------------------------------------
    //
    // 都是**精确匹配**（不做正则、不支持 /user/:id 这类参数化路径）。
    // 这不是偷懒：路由表就是一个 map，讲清楚这一点比实现一个半吊子的
    // 路由框架有价值得多。真需要参数时，业务处理器里自己切 req.path 就行。

    void route(const std::string& method, const std::string& path, Handler h);
    void get(const std::string& path, Handler h) { route("GET", path, std::move(h)); }
    void post(const std::string& path, Handler h) { route("POST", path, std::move(h)); }

    // 兜底处理器：路由和静态文件都没命中时调用（默认回一个朴素的 404）
    void setDefaultHandler(Handler h) { defaultHandler_ = std::move(h); }

    // ---- 静态文件 ---------------------------------------------------------

    // 设置静态资源根目录。命中路由的请求优先走路由；没命中时再按请求路径
    // 去这个目录里找文件（只处理 GET / HEAD）。
    // 路径会经过完整的目录穿越校验，详见实现文件里的说明。
    void setStaticRoot(const std::string& dir);

    // ---- 查询 -------------------------------------------------------------

    uint16_t boundPort() const { return tcp_.boundPort(); }
    TcpServer& tcp() { return tcp_; }
    size_t connectionCount() const { return tcp_.connections().size(); }

private:
    // 每条连接的额外状态
    struct ConnState {
        HttpContext ctx;
        std::chrono::steady_clock::time_point lastActive;

        explicit ConnState(const HttpContext::Limits& limits)
            : ctx(limits), lastActive(std::chrono::steady_clock::now()) {}
    };

    void onConnect(socket_t fd);
    void onMessage(socket_t fd, const char* data, size_t len);
    void onClose(socket_t fd);
    void onWriteComplete(socket_t fd);

    HttpContext::Limits limits() const;

    // 路由查找 → 静态文件 → 405 → 兜底，四级依次尝试
    void handleRequest(const HttpRequest& req, HttpResponse& resp);
    // 尝试用静态文件满足请求。返回 true 表示已经填好响应（含 403/404 之外的
    // 所有情况）；返回 false 表示"这条路走不通"，交给后续环节
    bool serveStaticFile(const HttpRequest& req, HttpResponse& resp);
    // 路径能匹配上路由、但方法不对时填一个 405（带 Allow 头）。返回是否命中
    bool fillMethodNotAllowed(const HttpRequest& req, HttpResponse& resp) const;
    // 填一个朴素的文本响应（错误页用）
    static void fillText(HttpResponse& resp, int code, const std::string& text);

    // 补齐服务器统一头部并把响应发出去；必要时安排"发完就关"
    void sendResponse(socket_t fd, const HttpRequest& req, HttpResponse& resp);

    // 关掉超时空闲的连接
    void closeIdleConnections();

    Options opts_;
    TcpServer tcp_;

    std::map<std::string, Handler> routes_;  // key = "GET /path"
    Handler defaultHandler_;

    std::string staticRoot_;         // 用户配置的根目录（原样保存，用于日志）
    std::string staticRootCanon_;    // 规范化之后的根目录（用于穿越校验）

    std::unordered_map<socket_t, ConnState> states_;
    // 已经决定关闭、正在等发送缓冲区排空的连接。
    // 它们随时可能在 onWriteComplete 里被真正关掉。
    std::unordered_set<socket_t> closing_;
};

}  // namespace http
}  // namespace net
