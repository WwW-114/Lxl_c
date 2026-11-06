#pragma once
// ============================================================================
//  HttpRequest.h —— 一个「已经解析完成」的 HTTP 请求
//
//  职责边界（这一点很重要）：
//    本类只负责「保存」和「查询」，完全不做解析。
//    把字节流变成 HttpRequest 是 HttpContext 的工作。
//
//  这样切分的好处：
//    * 解析逻辑（容易出错、需要单独测）与数据结构（简单、稳定）解耦；
//    * 业务代码（路由处理器）拿到的是一个纯数据对象，
//      不需要知道它是一口气收齐的，还是分 20 个 TCP 段拼出来的。
// ============================================================================

#include <cstddef>
#include <map>
#include <string>

namespace net {
namespace http {

class HttpRequest {
public:
    // ---- 请求行的三个部分 -------------------------------------------------
    //   "GET /index.html?a=1 HTTP/1.1"
    //    ^^^ ^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^
    //   方法        请求目标        版本

    std::string method;   // "GET" / "POST" / "HEAD" ...
                          // 方法名是**区分大小写**的，"get" 不是合法的方法名。
    std::string target;   // 原始请求目标，保持原样不拆不改（便于日志与调试）
    std::string version;  // "HTTP/1.0" 或 "HTTP/1.1"

    // target 拆出来的两部分。
    // 【顺序很关键】拆分必须在百分号解码**之前**按 '?' 做：
    // 如果先解码，请求目标里本来编码过的 "%3F"（也就是 '?'）就会变成一个
    // 真的分隔符，把一个路径切成两半。
    std::string path;     // 路径部分，**已完成百分号解码**，如 "/index.html"
    std::string query;    // 查询串部分，**保持原始编码**，如 "a=1&b=%20"。
                          // 保留原样是因为解码规则取决于具体参数怎么用，
                          // 交给业务代码自己决定（本项目的示例不解析它）。

    // ---- 头部与正文 -------------------------------------------------------
    // 键统一转小写成小写保存。
    //   RFC 7230 §3.2 规定头部字段名大小写不敏感，但各客户端的写法五花八门
    //   （Content-Length / content-length / CONTENT-LENGTH 都合法）。
    //   不统一小写的话，findHeader("content-length") 就会漏掉一大半客户端。
    //   值则原样保留 —— 值的语义由每个字段自己定义，服务器不该擅自改。
    std::map<std::string, std::string> headers;

    std::string body;     // 请求正文（按 Content-Length 读满）

    // ---- 查询接口 ---------------------------------------------------------

    // 按名查头部。注意入参必须是**小写**的字段名（内部就是这么存的）。
    // 返回 nullptr 表示不存在；返回指针而不是拷贝，避免无谓的字符串复制。
    const std::string* findHeader(const std::string& lowerName) const;
    bool hasHeader(const std::string& lowerName) const {
        return findHeader(lowerName) != nullptr;
    }

    // 按名取头部值，不存在时返回默认值（写业务代码时更顺手）
    std::string headerOrDefault(const std::string& lowerName, const std::string& def = "") const;

    // 这一次连接是否可以复用（keep-alive）。
    // 语义由「HTTP 版本」和「Connection 头」共同决定，细节见实现文件。
    bool keepAlive() const;

    // 复位成一个空请求，供同一个连接上的下一个请求复用对象
    void clear();

    // 供日志使用："GET /index.html HTTP/1.1"
    std::string requestLine() const { return method + " " + target + " " + version; }
};

}  // namespace http
}  // namespace net
