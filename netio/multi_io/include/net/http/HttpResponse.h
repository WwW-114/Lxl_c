#pragma once
// ============================================================================
//  HttpResponse.h —— 一个「待发送」的 HTTP 响应
//
//  与 HttpRequest 一样，本类只是数据结构 + 序列化，不碰 socket。
//  业务代码往里面填状态码、头部、正文；HttpServer 负责把它变成字节流发出去。
//
//  为什么要单独有个类，而不是直接让业务拼字符串？
//    因为「拼 HTTP 响应」这件事有一堆容易漏的规矩：Content-Length 必须准确、
//    状态行格式不能错、头部和正文之间必须有空行、HEAD 请求不能带正文……
//    把这些收进一个类里，业务代码就只需要关心「回什么内容」，
//    协议格式的正确性由这一个地方统一保证。
// ============================================================================

#include <cstddef>
#include <map>
#include <string>

namespace net {
namespace http {

class HttpResponse {
public:
    HttpResponse() = default;

    // ---- 状态行 -----------------------------------------------------------
    void setStatus(int code);  // 原因短语自动查表（statusMessage）
    void setStatus(int code, const std::string& reason);  // 自定义原因短语

    int statusCode() const { return status_; }
    const std::string& reason() const { return reason_; }

    // ---- 头部 -------------------------------------------------------------
    // 同名（不区分大小写）的头部会被覆盖，不会产生两条。
    void setHeader(const std::string& name, const std::string& value);
    void setContentType(const std::string& type) { setHeader("Content-Type", type); }

    // ---- 正文 -------------------------------------------------------------
    // 参数按值传递 + std::move：传左值时拷贝一次，传右值（比如 std::move(str)）
    // 或临时对象时直接移动，两种情况都不用写两个重载。
    void setBody(std::string body) { body_ = std::move(body); }
    void setBody(const char* data, size_t len) { body_.assign(data, len); }

    const std::string& body() const { return body_; }

    // HEAD 请求专用开关，见实现文件里的详细说明：
    // 打开后 Content-Length 仍然按真实正文长度输出，但不发送正文本身。
    void suppressBody(bool on) { suppressBody_ = on; }
    bool bodySuppressed() const { return suppressBody_; }

    // ---- 连接控制 ---------------------------------------------------------
    // 业务代码可以用它强制要求服务器在发完这个响应后关闭连接
    // （比如处理出错、或者不希望客户端继续复用这条连接）。
    void setCloseConnection(bool on) { closeConnection_ = on; }
    bool closeConnection() const { return closeConnection_; }

    // ---- 序列化 -----------------------------------------------------------
    // 变成真正在网络上传输的字节流：
    //     状态行 CRLF
    //     头部 CRLF
    //     CRLF                     ← 空行，头部结束的标志
    //     正文
    std::string toString() const;

    void clear();

private:
    // 不区分大小写地查找头部，返回它当前的键名（没有则返回 nullptr）
    const std::string* findKeyIgnoreCase(const std::string& name) const;

    int status_ = 200;
    std::string reason_ = "OK";

    std::map<std::string, std::string> headers_;  // 用 map：输出顺序稳定，便于阅读与测试
    std::string body_;

    bool suppressBody_ = false;    // HEAD 请求：有 Content-Length，但不发正文
    bool closeConnection_ = false; // 业务层要求关闭连接
};

}  // namespace http
}  // namespace net
