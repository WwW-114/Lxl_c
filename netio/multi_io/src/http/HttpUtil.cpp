#include "net/http/HttpUtil.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace net {
namespace http {

// ---------------------------------------------------------------------------
//  状态码 → 原因短语
//
//  只列出本项目会真正用到的那些。没命中的一律返回 "Unknown"——
//  注意原因短语对客户端是“仅供参考”的，返回什么都不影响协议语义，
//  真正有意义的是那个三位数字。
// ---------------------------------------------------------------------------
const char* statusMessage(int code) {
    switch (code) {
        // 1xx 信息性：这不是最终响应，只是过程通知
        case 100: return "Continue";

        case 200: return "OK";
        case 204: return "No Content";  // 成功，但特意不带 body
        case 206: return "Partial Content";

        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";

        case 400: return "Bad Request";           // 报文语法错误，客户端写错了
        case 403: return "Forbidden";             // 看懂了但拒绝执行（比如试图越出根目录）
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";    // 路径存在，但方法不对（要配 Allow 头）
        case 408: return "Request Timeout";
        case 411: return "Length Required";       // 有 body 却没给 Content-Length
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";

        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";       // 服务器不支持该功能（比如 chunked 请求体）
        case 505: return "HTTP Version Not Supported";

        default: break;
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
//  扩展名 → Content-Type
//
//  比对时用文件名的后半段做后缀匹配，且要忽略大小写（.HTML 与 .html 一样）。
//  Content-Type 决定了浏览器是“渲染”还是“下载”，给错了页面就会变成乱码或附件。
// ---------------------------------------------------------------------------
const char* mimeType(const std::string& path) {
    // 先取最后一个 '.' 之后的部分作为扩展名
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }
    const std::string ext = toLower(path.substr(dot));

    // text/* 与 json/javascript 都显式带上 charset，
    // 否则浏览器会按自己的默认编码猜，中文页面必然乱码。
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js")   return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".txt")  return "text/plain; charset=utf-8";
    if (ext == ".xml")  return "application/xml; charset=utf-8";
    if (ext == ".csv")  return "text/csv; charset=utf-8";

    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".webp") return "image/webp";

    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".zip")  return "application/zip";
    if (ext == ".gz")   return "application/gzip";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".mp3")  return "audio/mpeg";
    if (ext == ".woff2") return "font/woff2";

    // 认不出来时一律当二进制流。
    // 千万不要在这里返回 text/html —— 那等于把用户上传的任意文件当网页执行，
    // 是存储型 XSS 的经典成因。
    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
//  IMF-fixdate 格式化
// ---------------------------------------------------------------------------
std::string httpDate() {
    // 星期与月份缩写表。C 标准库的 %a / %b 会跟随 locale 输出本地文字，
    // 在中文系统上会得到“周日”“11月”这种结果，那不是合法的 HTTP 日期，
    // 所以必须自己查表。
    static const char* kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[]   = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const std::time_t now = std::time(nullptr);
    std::tm gmt{};

    // 把 UTC 时间拆进我们自己提供的 struct tm —— 可重入，多线程安全
#ifdef _WIN32
    gmtime_s(&gmt, &now);
#else
    gmtime_r(&now, &gmt);
#endif

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kWeekdays[gmt.tm_wday % 7], gmt.tm_mday, kMonths[gmt.tm_mon % 12],
                  gmt.tm_year + 1900, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
//  百分号解码
// ---------------------------------------------------------------------------
namespace {

// 把一个十六进制字符转成数值，非法字符返回 -1
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool urlDecode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());  // 解码只会让字符串变短或等长，预留原长度一定够

    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];

        if (c == '%') {
            // 必须还有两个字符可供读取，否则就是被截断的非法转义
            if (i + 2 >= in.size()) {
                return false;
            }
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out.push_back((char)((hi << 4) | lo));
            i += 2;
            continue;
        }

        if (c == '+') {
            // 在 application/x-www-form-urlencoded 的语义里 '+' 代表空格。
            // 注意这是在**查询串**里的约定；路径部分严格来说 '+' 就是加号本身，
            // 实践中几乎所有服务器都会一并换掉，这里跟随主流做法。
            out.push_back(' ');
            continue;
        }

        out.push_back(c);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  字符串小工具
// ---------------------------------------------------------------------------
std::string toLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }
    return s;
}

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    // 值里不可能出现裸的 \r \n（它们会被当成分行符），但防止手工构造时带上
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

bool hasToken(const std::string& value, const std::string& token) {
    // 按 ',' 切开逐段比较。Connection 头的值是一个 token 列表，
    // 比如 "keep-alive, Upgrade"，拿整串做相等比较是查不出来的。
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        if (toLower(trim(value.substr(pos, comma - pos))) == toLower(token)) {
            return true;
        }
        pos = comma + 1;
    }
    return false;
}

}  // namespace http
}  // namespace net
