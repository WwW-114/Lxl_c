#include "net/http/HttpRequest.h"

#include "net/http/HttpUtil.h"

namespace net {
namespace http {

const std::string* HttpRequest::findHeader(const std::string& lowerName) const {
    // 调用方可能顺手传了大小写不一的字段名，这里兜一手，
    // 免得它拿到 nullptr 却以为是“服务端没收到这个头”。
    auto it = headers.find(lowerName);
    if (it != headers.end()) {
        return &it->second;
    }
    it = headers.find(toLower(lowerName));
    return (it != headers.end()) ? &it->second : nullptr;
}

std::string HttpRequest::headerOrDefault(const std::string& lowerName,
                                         const std::string& def) const {
    const std::string* v = findHeader(lowerName);
    return (v != nullptr) ? *v : def;
}

// ---------------------------------------------------------------------------
//  连接是否可以复用 —— HTTP 里最容易记反的一处细节
//
//  两套默认值正好相反：
//
//      HTTP/1.0：默认**短连接** —— 服务器回完就关，除非客户端明确说 keep-alive
//      HTTP/1.1：默认**长连接** —— 服务器保持连接，除非客户端明确说 close
//
//  为什么 1.1 要反过来？因为 1.0 时代“每个请求一趟 TCP 三次握手”的开销实在太大，
//  1.1 干脆把长连接定成默认行为，同时**强制**要求 Host 头，
//  这样同一个连接才能服务于同一台服务器上的多个虚拟主机。
//
//  另一个坑：Connection 的值是**逗号分隔的 token 列表**，
//  合法的值可能是 "keep-alive, Upgrade" 或 "close, foo"。
//  必须按 token 查，不能拿整串去 == 比较。
// ---------------------------------------------------------------------------
bool HttpRequest::keepAlive() const {
    const std::string* conn = findHeader("connection");

    if (version == "HTTP/1.1") {
        // 1.1 默认复用；只有明确说了 close 才断开
        return !(conn != nullptr && hasToken(*conn, "close"));
    }

    // HTTP/1.0（以及任何其它版本，虽然解析阶段已经把非 1.0/1.1 的挡掉了）
    // 默认关闭；只有明确说了 keep-alive 才复用
    return conn != nullptr && hasToken(*conn, "keep-alive");
}

void HttpRequest::clear() {
    method.clear();
    target.clear();
    version.clear();
    path.clear();
    query.clear();
    headers.clear();
    body.clear();
}

}  // namespace http
}  // namespace net
