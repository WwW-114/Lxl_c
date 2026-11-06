#pragma once

#include <cstdint>
#include <sys/time.h>
#include <string>

// ---------------------------------------------------------------------------
//  1. 时间
// ---------------------------------------------------------------------------
// KCP 内部所有时间都是 uint32_t 毫秒，并且靠无符号回绕来比较先后顺序，
// 所以这里也返回 uint32_t（大约 49.7 天回绕一次，KCP 自己能处理）。
inline uint32_t iclock() {
  timeval tv{};
  gettimeofday(&tv, nullptr);
  return static_cast<uint32_t>(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

// ---------------------------------------------------------------------------
//  2. 消息类型
// ---------------------------------------------------------------------------
enum MsgType : uint8_t {
  // ---- 客户端 -> 服务器 ----
  MSG_LOGIN =  0x01,  // 载荷：昵称
  MSG_CHAT =   0x02,  // 载荷：聊天正文
  MSG_LOGOUT = 0x03,  // 载荷：无
  MSG_PING =   0x04,  // 载荷：无（心跳）

  // ---- 服务器 -> 客户端 ----
  MSG_BROADCAST = 0x81, // 载荷：已经拼好格式的整行文本，客户端直接打印  
  MSG_SYSTEM =    0x82, // 载荷：系统提示（欢迎 / 加入 / 离开……）
  MSG_PONG =      0x83  // 载荷：无（心跳的回应）
};

// ---------------------------------------------------------------------------
//  3. 常量
// ---------------------------------------------------------------------------
constexpr uint16_t DEFAULT_PORT  = 8888; 
constexpr int      MAX_NAME_LEN  = 32;   // 昵称字节数上限
constexpr int      MAX_MSG_LEN   = 4096; // 单条应用层消息的字节数上限
constexpr int      PING_INTERVAL = 5;    // 客户端心跳间隔（秒）
constexpr int      TIMEOUT_SEC   = 30;   // 多久收不到对端数据就判定掉线（秒）

// KCP 包头固定 24 字节：conv(4) + cmd(1) + frg(1) + wnd(2) + ts(4) + sn(4)
// + una(4) + len(4)。注意这个值在 ikcp.c 里是一个文件作用域的 const 变量
// （ikcp.c:40），ikcp.h 并不导出它，所以这里自己写一份。
constexpr int      KCP_OVERHEAD  = 24;

inline std::string pack(MsgType type, const std::string &body = std::string()) {
  std::string out;
  out.reserve(body.size());
  out.push_back(static_cast<char>(type));
  out.append(body);
  return out;
}

inline MsgType msgType(const std::string &s) {
  return static_cast<MsgType>(static_cast<uint8_t>(s[0]));
}

inline std::string msgBody(const std::string &s) {
  return s.substr(1);
}

inline std::string trim(const std::string &s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if(begin == std::string::npos) return std::string();
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}
