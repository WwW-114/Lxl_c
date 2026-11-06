#include "net/http/HttpResponse.h"

#include "net/http/HttpUtil.h"

namespace net {
namespace http {

void HttpResponse::setStatus(int code) {
    setStatus(code, statusMessage(code));
}

void HttpResponse::setStatus(int code, const std::string& reason) {
    status_ = code;
    reason_ = reason;
}

const std::string* HttpResponse::findKeyIgnoreCase(const std::string& name) const {
    const std::string lower = toLower(name);
    for (const auto& kv : headers_) {
        if (toLower(kv.first) == lower) {
            return &kv.first;
        }
    }
    return nullptr;
}

void HttpResponse::setHeader(const std::string& name, const std::string& value) {
    // 先删掉可能存在的同名字段（不管大小写怎么写的），再插入。
    // 否则 "Content-Type" 和 "content-type" 会各存一份，序列化出去就是两条重复头，
    // 客户端见到重复的 Content-Length 之类会直接判定报文非法。
    const std::string* existing = findKeyIgnoreCase(name);
    if (existing != nullptr) {
        headers_.erase(*existing);
    }
    headers_[name] = value;
}

// ---------------------------------------------------------------------------
//  序列化
// ---------------------------------------------------------------------------
std::string HttpResponse::toString() const {
    std::string out;
    // 预留空间，免得拼接过程中反复 realloc。
    // 256 是个经验值：状态行 + 常见头部（Date/Server/Content-Type/
    // Content-Length/Connection）大约在这个量级。
    out.reserve(256 + body_.size());

    // ---- 状态行 ----
    // 无论客户端用的是 1.0 还是 1.1，这里一律回 "HTTP/1.1"。
    // RFC 7230 §2.6 允许（并要求）服务器回报自己支持的最高版本，
    // 1.0 的客户端会忽略版本号后面的小版本差异，能正常解析。
    // nginx / Apache 也都是这么做的。
    out += "HTTP/1.1 ";
    out += std::to_string(status_);
    out += ' ';
    out += reason_;
    out += "\r\n";

    // ---- 自动补 Content-Length ----
    // 【这是必须的】响应的正文长度必须有确定来源，否则客户端无从判断正文到哪结束：
    //   * 有 Content-Length   → 按字节数读满
    //   * 用了 chunked 编码   → 按分块规则读（本项目不实现）
    //   * 以上都没有          → 只能一直读到连接关闭为止
    // 最后一种情况下浏览器会一直转圈等，直到服务器超时断开——表现为“页面卡住”。
    // 所以哪怕正文是空的，也要老老实实给出 "Content-Length: 0"。
    if (findKeyIgnoreCase("Content-Length") == nullptr) {
        out += "Content-Length: ";
        out += std::to_string(body_.size());
        out += "\r\n";
    }

    // ---- 其余头部 ----
    for (const auto& kv : headers_) {
        out += kv.first;
        out += ": ";
        out += kv.second;
        out += "\r\n";
    }

    // ---- 空行：头部的结束标志 ----
    // 少了这一个 CRLF，客户端会把后面所有内容都当头部去解析，
    // 于是报出“响应格式错误”或干脆一直挂着等。
    out += "\r\n";

    // ---- 正文 ----
    // HEAD 请求的响应要带着和 GET 一样的 Content-Length，但**不能有正文**
    // （RFC 7231 §4.3.2）。客户端拿这个长度是为了知道资源有多大，
    // 但按协议它不应该真的收到这些字节。
    if (!suppressBody_) {
        out += body_;
    }
    return out;
}

void HttpResponse::clear() {
    status_ = 200;
    reason_ = "OK";
    headers_.clear();
    body_.clear();
    suppressBody_ = false;
    closeConnection_ = false;
}

}  // namespace http
}  // namespace net
