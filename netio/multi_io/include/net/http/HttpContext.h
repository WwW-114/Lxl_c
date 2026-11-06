#pragma once
// ============================================================================
//  HttpContext.h —— 把 TCP 字节流还原成一个个 HTTP 请求（本层核心）
//
//  ┌─ 它要解决的问题 ───────────────────────────────────────────────────────┐
//  │ TCP 是**字节流**，它只保证「字节按顺序到达」，不保证「一次 recv 恰好   │
//  │ 收到一条消息」。同一个请求可能分 10 个 TCP 段到达（拆包），            │
//  │ 三个请求也可能挤在同一个段里到达（粘包 / 管线化）。                    │
//  │ HTTP 是**应用层协议**，报文的边界只能由应用层自己找出来：              │
//  │      请求行 ─CRLF→ 头部 ─CRLF→ 空行 ─→ 正文（由 Content-Length 定长） │
//  │ 这个类就是一个「增量解析状态机」，来多少字节吃多少，吃不下就等下次。   │
//  └────────────────────────────────────────────────────────────────────────┘
//
//  每个连接持有一个 HttpContext。它的使用节奏是：
//
//      append()   ← recv 到什么就喂什么，可以喂任意多次、任意大小
//      parse()    ← 问它「攒够一个完整请求了吗」
//      ...
//      reset()    ← 请求处理完了，为同一个连接上的下一个请求做准备
//
//  注意 reset() **不清空缓冲区**：缓冲区里剩下的字节属于下一个请求
//  （客户端一次性发了三个请求，第一个处理完时后两个早就在缓冲区里了）。
//  这正是 keep-alive 与 HTTP 管线化能成立的根本原因。
// ============================================================================

#include "net/http/HttpRequest.h"

#include <cstddef>
#include <string>

namespace net {
namespace http {

class HttpContext {
public:
    // -----------------------------------------------------------------------
    //  parse() 的返回值
    // -----------------------------------------------------------------------
    enum class Result {
        // 数据还不够，等下一次 recv 后再调用 parse()
        Incomplete,

        // 见到了 "Expect: 100-continue"，客户端正在**等我们先回话**才肯发正文。
        // 上层应当立刻发一行 "HTTP/1.1 100 Continue\r\n\r\n"，然后**继续调用
        // parse()** 把正文读进来。
        //
        // 为什么要有这个状态：curl 对超过 1 KiB 的请求体会自动加上这个头，
        // 然后停下来等服务器回应。服务器要是装看不见，客户端会一直等到自己的
        // 超时（curl 默认 1 秒）才补发正文 —— 表现为「POST 莫名其妙慢了一秒」。
        ExpectContinue,

        // 一个完整的请求已经就绪，可以交给业务处理了（request()）
        Complete,

        // 报文不合法（或超出限额）。应当回 errorCode() 指定的状态码并**关闭连接**。
        // 为什么必须关闭：出错的报文已经把字节流"污染"了，我们再也无法确定
        // 下一个请求从哪里开始。想在这种连接上继续复用，就是给自己挖坑。
        Error,
    };

    // -----------------------------------------------------------------------
    //  各种长度上限 —— 全部是为了防止「慢速攻击」：
    //  攻击者用一个字节一个字节地发、永远不发完的方式，让服务器为一条连接
    //  持续分配内存，几千条这样的连接就能把内存耗光。
    //  任何面向公网的服务端都必须给这几个量设上限。
    // -----------------------------------------------------------------------
    struct Limits {
        size_t maxLineBytes = 8 * 1024;            // 单行（请求行 / 单个头部行）
        size_t maxHeaderBytes = 16 * 1024;         // 整个头部区域的总量
        size_t maxBodyBytes = 1 * 1024 * 1024;     // 请求正文
    };

    // 【为什么拆成两个构造函数】
    // 原本写成 explicit HttpContext(Limits limits = {}) 更自然，但 GCC 11~15
    // 都有一个缺陷：嵌套结构体 Limits 的默认成员初始化器要等外层类解析完
    // 才真正生效，而成员函数的默认实参在那之前就要做类型检查，于是 {} 会被
    // 判定为「无法转换成 Limits」（could not convert '<brace-enclosed
    // initializer list>()'）。把默认构造拆出来、委托给带参版本即可绕开这个
    // 时机问题，两种写法的语义完全一致。
    HttpContext() : HttpContext(Limits()) {}
    explicit HttpContext(Limits limits) : limits_(limits) {}

    HttpContext(const HttpContext&) = delete;
    HttpContext& operator=(const HttpContext&) = delete;

    // 喂入原始字节。可以在任意时刻调用任意多次。
    void append(const char* data, size_t len);
    void append(const std::string& data) { append(data.data(), data.size()); }

    // 尝试推进解析。一次调用最多推进出**一个**完整请求；
    // 缓冲区里剩下的字节原样保留，下次调用（或下一次 recv 之后）继续。
    Result parse();

    const HttpRequest& request() const { return request_; }

    // Error 时：建议回给客户端的状态码（400 / 413 / 414 / 431 / 501 / 505）
    int errorCode() const { return errorCode_; }

    // 一个请求处理完毕后调用：清掉解析状态，但保留缓冲区里剩下的字节
    void reset();

    // 缓冲区里还有多少未消费的字节（用于日志与调试）
    size_t buffered() const { return inBuf_.size() - readPos_; }

private:
    // 解析状态机的位置
    enum class State {
        RequestLine,  // 正在等 / 解析请求行
        Headers,      // 正在逐行解析头部
        Body,         // 头部已知，正在等正文读满 Content-Length
        Complete,     // 一个完整请求已就绪，等上层取走
    };

    // 在缓冲区里找下一个 CRLF。找到返回 true 并把位置写进 crlfPos。
    bool findCRLF(size_t& crlfPos);

    bool parseRequestLine(size_t lineEnd);
    bool parseHeaderLine(size_t lineEnd);
    // 头部区域结束（读到空行）后的收尾工作：定出正文长度、做各项校验
    bool finishHeaders();
    bool parseContentLength(size_t& value);

    // 把已经消费掉的前缀从缓冲区里真正丢掉（攒够一批再做，避免每次都搬内存）
    void compact();

    // 统一记录错误：置位错误码并返回 Result::Error
    Result fail(int code);

    Limits limits_;

    // ---- 输入缓冲区 -------------------------------------------------------
    // 这里用的是「字符串 + 读下标」的方案，而不是每次消费都 erase(0, n)。
    // erase 从头部删除是 O(剩余长度) 的内存搬移，而 TCP 缓冲区里可能有
    // 几十 KB 的数据等着，每消费一行就搬一次太浪费。
    // 攒到一定量再 compress 一次，摊还下来每次消费是 O(1)。
    // （muduo 的 Buffer 用 readerIndex_/writerIndex_ 两个下标做同样的事。）
    std::string inBuf_;
    size_t readPos_ = 0;  // 已经消费到 inBuf_ 的哪个位置
    size_t scanPos_ = 0;  // CRLF 扫描的续扫起点，避免反复重扫同一段文本

    State state_ = State::RequestLine;

    HttpRequest request_;
    int errorCode_ = 400;
    bool failed_ = false;  // 已经出过错：之后任何 parse() 都直接返回 Error

    size_t bodyLen_ = 0;      // 由 Content-Length 定出的正文长度
    size_t headerBytes_ = 0;  // 本请求已解析的头部字节数（用于限额检查）

    bool expectContinue_ = false;  // 请求里有 "Expect: 100-continue"
    bool continueSent_ = false;    // 100 Continue 是否已经通知过上层（只回一次）
};

}  // namespace http
}  // namespace net
