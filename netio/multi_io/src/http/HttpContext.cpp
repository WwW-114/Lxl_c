#include "net/http/HttpContext.h"

#include "net/http/HttpUtil.h"

#include <cctype>

namespace net {
namespace http {

namespace {

// 消费掉多少字节之后就把缓冲区前缀真正丢掉。
// 太小 → 频繁搬内存；太大 → 空闲连接白占内存。4 KiB 是个折中值。
constexpr size_t kCompactThreshold = 4096;

// 请求方法名只允许字母。RFC 7230 里方法是一个更宽泛的 "token"，
// 但现实中在用的方法（GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH/PROPFIND…）全是字母，
// 收紧一点可以顺手挡掉一批畸形请求。
bool isAlphaToken(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!std::isalpha((unsigned char)c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  喂数据
//
//  这里**必须拷贝**：data 指向的是 TcpServer::handleRead 里那个临时的
//  recv 缓冲区，出了回调就失效了。想省掉这次拷贝只有一条路 ——
//  把解析器做成"零拷贝"的，直接在下层缓冲区上打标记（很多高性能实现会这么做，
//  但代价是缓冲区生命周期管理变得非常复杂）。教学项目不值得。
// ---------------------------------------------------------------------------
void HttpContext::append(const char* data, size_t len) {
    if (len == 0 || data == nullptr) {
        return;
    }
    inBuf_.append(data, len);
}

HttpContext::Result HttpContext::fail(int code) {
    errorCode_ = code;
    failed_ = true;
    return Result::Error;
}

// ---------------------------------------------------------------------------
//  缓冲区的"压缩"：把已经消费掉的前缀真正丢掉
// ---------------------------------------------------------------------------
void HttpContext::compact() {
    if (readPos_ == 0) {
        return;
    }
    // 两种情况值得压缩：
    //   1) 攒够了一批（摊还成本低）
    //   2) 数据已经全部消费完（空闲的 keep-alive 连接不该一直占着内存）
    if (readPos_ < kCompactThreshold && readPos_ < inBuf_.size()) {
        return;
    }

    inBuf_.erase(0, readPos_);
    // 两个下标要跟着一起挪。scanPos_ 理论上总是 >= readPos_，
    // 这里仍然做一次保护，免得将来改动逻辑时出现减法下溢（size_t 下溢会变成天文数字）。
    scanPos_ = (scanPos_ > readPos_) ? (scanPos_ - readPos_) : 0;
    readPos_ = 0;
}

// ---------------------------------------------------------------------------
//  在缓冲区里找下一个 CRLF
//
//  看起来很简单的一行 find，但有两个讲究：
//
//  ① 从 scanPos_ 续扫，而不是每次都从 readPos_ 重扫。
//     否则一个 8 KiB 的请求行分成 8000 次到达时，就要重复扫描
//     1+2+3+...+8000 ≈ 3200 万次字节比较 —— O(n²) 的经典翻车现场。
//
//  ② 找不到时把 scanPos_ 回退到"缓冲区的最后一个字节"。
//     因为缓冲区末尾可能恰好是一个孤零零的 '\r'，它的 '\n' 还在下一个 TCP 段里。
//     下次数据到了，必须从这个 '\r' 重新开始找，否则这一个 CRLF 就被永久跳过了。
// ---------------------------------------------------------------------------
bool HttpContext::findCRLF(size_t& crlfPos) {
    if (scanPos_ < readPos_) {
        scanPos_ = readPos_;
    }

    const size_t p = inBuf_.find("\r\n", scanPos_);
    if (p == std::string::npos) {
        scanPos_ = inBuf_.empty() ? 0 : inBuf_.size() - 1;
        return false;
    }
    crlfPos = p;
    return true;
}

// ---------------------------------------------------------------------------
//  解析请求行：  METHOD SP TARGET SP VERSION
// ---------------------------------------------------------------------------
bool HttpContext::parseRequestLine(size_t lineEnd) {
    const std::string line = inBuf_.substr(readPos_, lineEnd - readPos_);

    // 两个空格正好把这一行切成三段
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) {
        fail(400);
        return false;
    }
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        fail(400);
        return false;
    }
    // 第三个空格说明段数不对（比如 "GET / HTTP/1.1 extra"），或者
    // 请求目标里带了裸空格。请求目标里的空格必须是 %20，裸空格一律非法。
    if (line.find(' ', sp2 + 1) != std::string::npos) {
        fail(400);
        return false;
    }

    const std::string method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = trim(line.substr(sp2 + 1));

    if (!isAlphaToken(method) || method.size() > 32) {
        fail(400);
        return false;
    }
    if (target.empty()) {
        fail(400);
        return false;
    }

    // 版本校验。trim 是为了容忍 "HTTP/1.1 " 这种尾巴上多一个空格的写法 ——
    // 严格说它也不合法，但把它报成 505（版本不支持）比报 400 更贴近事实，
    // 而且不会让一个善意的客户端彻底懵掉。
    if (version != "HTTP/1.0" && version != "HTTP/1.1") {
        fail(505);
        return false;
    }

    request_.method = method;
    request_.target = target;
    request_.version = version;

    // ---- 把 target 拆成 path 和 query -----------------------------------
    //
    // 请求目标有四种形式（RFC 7230 §5.3）：
    //   origin-form     "/path?q"           ← 绝大多数情况，直接连服务器时用
    //   absolute-form   "http://h/path?q"   ← 经过代理时会出现
    //   authority-form  "host:port"         ← 只用于 CONNECT
    //   asterisk-form   "*"                 ← 只用于 OPTIONS
    //
    // 这里主要认 origin-form；absolute-form 顺手把 "scheme://authority" 剥掉，
    // 免得代理场景下所有的路径都匹配不上（真实服务器同样要处理这一种）。
    std::string pathAndQuery = target;
    {
        const size_t sep = pathAndQuery.find("://");
        if (sep != std::string::npos && sep <= 8) {  // "http://"(7) 或 "https://"(8)
            const size_t slash = pathAndQuery.find('/', sep + 3);
            pathAndQuery = (slash == std::string::npos) ? "/" : pathAndQuery.substr(slash);
        }
    }

    // 【顺序】先按 '?' 切分，**再**做百分号解码。
    // 反过来的话，请求目标里本来编码过的 "%3F"（也就是 '?'）会在解码后变成一个
    // 真的分隔符，把一条路径硬生生切成两半。
    const size_t qmark = pathAndQuery.find('?');
    std::string rawPath = pathAndQuery;
    if (qmark != std::string::npos) {
        rawPath = pathAndQuery.substr(0, qmark);
        // 查询串保持**原始编码**不动：怎么解码取决于每个参数自己的语义
        // （'&' 和 '=' 该不该还原、'+' 算不算空格），交给业务代码决定。
        request_.query = pathAndQuery.substr(qmark + 1);
    }

    if (!urlDecode(rawPath, request_.path)) {
        fail(400);  // 非法的百分号转义
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
//  解析一行头部：  Field-Name ":" OWS Field-Value OWS
// ---------------------------------------------------------------------------
bool HttpContext::parseHeaderLine(size_t lineEnd) {
    const size_t lineLen = lineEnd - readPos_;

    // ---- 废弃的"折行"（obs-fold） ----------------------------------------
    // 老规范允许把一行头部折成多行，续行以空格或制表符开头：
    //      X-Long: part1
    //       part2
    // RFC 7230 §3.2.4 已经把它废弃了，并**要求服务器直接回 400** ——
    // 因为它和"请求走私"关系密切：不同的中间件对折行的处理方式不一致，
    // 攻击者就能构造出"前端看到的是 A，后端看到的是 B"的报文。
    if (lineLen > 0) {
        const char first = inBuf_[readPos_];
        if (first == ' ' || first == '\t') {
            fail(400);
            return false;
        }
    }

    const size_t colon = inBuf_.find(':', readPos_);
    if (colon == std::string::npos || colon > lineEnd) {
        fail(400);  // 一整行里没有冒号，这不是一个合法的头部
        return false;
    }
    if (colon == readPos_) {
        fail(400);  // 冒号前面什么都没有，字段名是空的
        return false;
    }

    // 【必须拒绝】字段名和冒号之间不允许出现空白（RFC 7230 §3.2.4 明确要求回 400）。
    // 这里不能"顺手 trim 一下就放过"：因为
    //      Content-Length : 5
    // 这种写法在不同实现里会被解析成不同的东西 —— 有的当成 Content-Length，
    // 有的当成一个叫 "Content-Length " 的未知头部。这正是请求走私的温床。
    if (inBuf_[colon - 1] == ' ' || inBuf_[colon - 1] == '\t') {
        fail(400);
        return false;
    }

    const std::string name = inBuf_.substr(readPos_, colon - readPos_);
    const std::string value = trim(inBuf_.substr(colon + 1, lineEnd - colon - 1));

    // 字段名必须是合法的 token：不允许出现空格、控制字符、分隔符等。
    // 这里用"可打印 ASCII 且不是分隔符"来近似，够用且足够严格。
    for (char c : name) {
        const unsigned char uc = (unsigned char)c;
        if (uc <= 32 || uc >= 127 || c == ':' || c == ',' || c == ';' ||
            c == '"' || c == '(' || c == ')' || c == '{' || c == '}' || c == '\\') {
            fail(400);
            return false;
        }
    }

    const std::string key = toLower(name);

    // ---- 两个必须单独对待的头部 ------------------------------------------

    // ① Host：HTTP/1.1 只允许出现一次。
    //    RFC 7230 §5.4 要求"多于一个 Host 头"必须回 400 ——
    //    因为 Host 决定请求该交给哪个虚拟主机处理，出现两个就等于
    //    "前后端可能选了不同的主机"，又是一个走私入口。
    if (key == "host" && request_.headers.count("host") != 0) {
        fail(400);
        return false;
    }

    // ② Content-Length：允许多次出现，但**值必须完全一致**；
    //    值不一致（"5" 和 "10"）必须立刻回 400 —— 这是请求走私最经典的载体：
    //    前端按 5 字节切分、后端按 10 字节切分，多出来的 5 个字节就成了
    //    "被夹带的下一个请求"。
    if (key == "content-length") {
        auto it = request_.headers.find("content-length");
        if (it == request_.headers.end()) {
            request_.headers.emplace(key, value);
        } else if (it->second != value) {
            fail(400);
            return false;
        }
        // 值完全相同：忽略这一次，只保留一份
        return true;
    }

    auto it = request_.headers.find(key);
    if (it == request_.headers.end()) {
        request_.headers.emplace(key, value);
    } else {
        // 其它头部重复出现是合法的。RFC 7230 §3.2.2 规定：除少数例外
        // （Set-Cookie 之类），同名头部应当按出现顺序用 ", " 连接成一个值。
        // 例如：
        //      Cache-Control: no-cache
        //      Cache-Control: no-store
        //  等价于
        //      Cache-Control: no-cache, no-store
        it->second += ", ";
        it->second += value;
    }

    return true;
}

// ---------------------------------------------------------------------------
//  Content-Length 的严格解析
// ---------------------------------------------------------------------------
bool HttpContext::parseContentLength(size_t& value) {
    const std::string* v = request_.findHeader("content-length");
    if (v == nullptr || v->empty()) {
        fail(400);
        return false;
    }

    // 【绝对不能用 atoi / strtol / stoul】
    // 它们会欣然接受 "-1"（转成巨大的无符号数）、" 12"、"12abc"、"0x10"，
    // 而这些在 Content-Length 里通通是非法的。宽松解析正是请求走私的入口：
    // 服务器以为长度是 12，中间设备以为长度是 0，夹带的数据就混进了下一条请求。
    // 所以这里只认纯粹的十进制数字，一个字符一个字符地自己算。
    size_t n = 0;
    for (char c : *v) {
        if (c < '0' || c > '9') {
            fail(400);
            return false;
        }
        n = n * 10 + (size_t)(c - '0');

        // 一旦超过上限就立刻停手。这同时也堵死了"整数溢出"这条路 ——
        // 若继续累加，一个精心构造的超长数字会绕回一个很小的值，
        // 于是限额检查被绕过，而真实长度却大得离谱。
        if (n > limits_.maxBodyBytes) {
            break;
        }
    }

    value = n;
    return true;
}

// ---------------------------------------------------------------------------
//  头部区域结束（读到空行）后的收尾
// ---------------------------------------------------------------------------
bool HttpContext::finishHeaders() {
    const bool hasTE = request_.hasHeader("transfer-encoding");
    const bool hasCL = request_.hasHeader("content-length");

    // ---- ① Transfer-Encoding 与 Content-Length 同时出现 → 400 ------------
    // 这是 RFC 7230 §3.3.3 的硬性规定，也是**请求走私**最著名的构造方式：
    // 前端认识 Transfer-Encoding、后端认识 Content-Length（或者反过来），
    // 两边算出的报文边界不一样，多出来的部分就被"夹带"成了下一个请求。
    // 正确做法不是"选一个来用"，而是直接拒绝整条连接。
    if (hasTE && hasCL) {
        fail(400);
        return false;
    }

    if (hasTE) {
        // ---- ② 本项目不实现分块传输 ---------------------------------------
        // 明确回 501（Not Implemented），**绝不能**当成长度 0 悄悄放过 ——
        // 那样请求体会原封不动留在缓冲区里，被当成下一个请求去解析，
        // 正是走私攻击想要的效果。
        // （要扩展的话，就在这里加一个 ChunkSize/ChunkData/ChunkTrailer 的子状态机。）
        fail(501);
        return false;
    }

    // ---- ③ HTTP/1.1 必须有 Host ------------------------------------------
    // RFC 7230 §5.4 的强制要求。Host 决定请求该交给哪个虚拟主机，
    // 没有它服务器就不知道回哪个站点。HTTP/1.0 时代不要求，所以只对 1.1 强制。
    if (request_.version == "HTTP/1.1" && !request_.hasHeader("host")) {
        fail(400);
        return false;
    }

    // ---- ④ 定出正文长度 ---------------------------------------------------
    size_t len = 0;
    if (hasCL) {
        if (!parseContentLength(len)) {
            return false;  // 内部已经 fail 过了
        }
        if (len > limits_.maxBodyBytes) {
            fail(413);
            return false;
        }
    }
    // 既没有 Transfer-Encoding 也没有 Content-Length 时，正文长度按 0 处理
    // （RFC 7230 §3.3.3 第 6 条）。
    // 另一种同样合法的选择是回 411 Length Required 把 POST/PUT 拒掉 ——
    // 那对客户端更严格一些，本项目选了更宽容也更符合规范的做法。
    bodyLen_ = len;

    // ---- ⑤ Expect: 100-continue -----------------------------------------
    // 客户端在等我们表态"可以发正文了"。
    // 注意这里的时机：**只有前面所有校验都通过了才告诉它可以发**。
    // 如果判定要拒绝（413/400），就一声不吭直接给最终响应 —— 让客户端
    // 白白传上来几百 MB 再告诉它"太大了"，是很不礼貌也很浪费的做法。
    const std::string* expect = request_.findHeader("expect");
    if (expect != nullptr && hasToken(*expect, "100-continue") && bodyLen_ > 0) {
        expectContinue_ = true;
    }

    state_ = (bodyLen_ > 0) ? State::Body : State::Complete;
    return true;
}

// ---------------------------------------------------------------------------
//  解析主循环
//
//  设计目标：一次调用最多产出一个完整请求；缓冲区里剩下的字节保持不动。
//  于是同一个连接上，只要外层反复调用，就能一个接一个地吐出请求 —— 这就是
//  keep-alive（串行复用连接）与 pipeline（客户端不等响应就连续发多个请求）
//  能够工作的全部秘密。
// ---------------------------------------------------------------------------
HttpContext::Result HttpContext::parse() {
    if (failed_) {
        return Result::Error;  // 已经出错，不下第二次判断
    }

    compact();

    for (;;) {
        switch (state_) {
            // ---------------------------------------------------------------
            case State::RequestLine: {
                size_t lineEnd = 0;
                if (!findCRLF(lineEnd)) {
                    // 一行都还没凑齐。检查是不是有人在"慢慢喂"——
                    // 一个字节一个字节地发一条永不结束的请求行，
                    // 就能让服务器为这条连接无限地分配内存。
                    if (inBuf_.size() - readPos_ > limits_.maxLineBytes) {
                        return fail(414);
                    }
                    return Result::Incomplete;
                }

                if (!parseRequestLine(lineEnd)) {
                    return Result::Error;
                }
                readPos_ = lineEnd + 2;  // 跳过结尾的 CRLF
                scanPos_ = readPos_;
                state_ = State::Headers;
                break;
            }

            // ---------------------------------------------------------------
            case State::Headers: {
                size_t lineEnd = 0;
                if (!findCRLF(lineEnd)) {
                    const size_t pending = inBuf_.size() - readPos_;
                    if (pending > limits_.maxLineBytes) {
                        return fail(431);
                    }
                    if (headerBytes_ + pending > limits_.maxHeaderBytes) {
                        return fail(431);
                    }
                    return Result::Incomplete;
                }

                if (lineEnd == readPos_) {
                    // 读到了一个空的 CRLF —— 这就是头部结束的标志。
                    // 整个 HTTP 报文的骨架就是靠它切出来的：
                    //      请求行 CRLF 头部... CRLF CRLF 正文
                    //                        ^^^^^^^^
                    readPos_ = lineEnd + 2;
                    scanPos_ = readPos_;
                    if (!finishHeaders()) {
                        return Result::Error;
                    }
                    // finishHeaders 已经把 state_ 设成 Body 或 Complete，
                    // 回到循环顶部继续推进
                } else {
                    if (!parseHeaderLine(lineEnd)) {
                        return Result::Error;
                    }
                    headerBytes_ += (lineEnd - readPos_) + 2;
                    if (headerBytes_ > limits_.maxHeaderBytes) {
                        return fail(431);
                    }
                    readPos_ = lineEnd + 2;
                    scanPos_ = readPos_;
                }
                break;
            }

            // ---------------------------------------------------------------
            case State::Body: {
                const size_t available = inBuf_.size() - readPos_;

                if (available >= bodyLen_) {
                    // 正文到齐（客户端可能压根没等我们的 100 Continue 就直接发了，
                    // 那就更不必多此一举地回它了）
                    request_.body.assign(inBuf_, readPos_, bodyLen_);
                    readPos_ += bodyLen_;
                    scanPos_ = readPos_;
                    state_ = State::Complete;
                    break;
                }

                // 正文还没收全。如果客户端正在等 100 Continue，先给它一个明确答复，
                // 否则它会一直干等到自己的超时（curl 默认 1 秒）才补发正文。
                if (expectContinue_ && !continueSent_) {
                    continueSent_ = true;  // 只回一次
                    return Result::ExpectContinue;
                }

                return Result::Incomplete;
            }

            // ---------------------------------------------------------------
            case State::Complete:
                return Result::Complete;
        }
    }
}

// ---------------------------------------------------------------------------
//  复位：为一个连接上的下一个请求做准备
// ---------------------------------------------------------------------------
void HttpContext::reset() {
    request_.clear();
    state_ = State::RequestLine;
    bodyLen_ = 0;
    headerBytes_ = 0;
    expectContinue_ = false;
    continueSent_ = false;
    errorCode_ = 400;
    failed_ = false;

    // 【关键】这里绝对不动 inBuf_ / readPos_ / scanPos_。
    // 管线化时，客户端早就把第二、第三个请求一起发过来了，它们正安安稳稳地
    // 躺在缓冲区里等着。这里若是把缓冲区清掉，那些请求就被无声无息地丢了 ——
    // 客户端会发现收不到第二个响应，然后一直等到超时。
    compact();
}

}  // namespace http
}  // namespace net
