#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>
#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace net {
	using socket_t = int;
	using kInVaildSocket = -1;

	inline void initNetwork() {}
	inline void closeUpNetwork() {}

	//Linux 系统直接返回错误码（统一的错误接口）
	inline int socketError() {
		return errno;
	}

	inline bool isWouldBlock(int err) {
		return err == EAGAIN || err == EWOULDBLOCK; 
		//分别代表着重新尝试和将会引起阻塞，都是说明当前网络无资源可用
	}

	inline bool isInterrupted(int err) {
		//代表着网络受到干扰，需要立刻重试而不是报错
		return err == EINTR;
	}

	inline std::string errorString(int err) {
		return std::string(strerror(err)) + "(" + std::to_string(err) + ")";
	}

	//在Linux系统中向对端已经关闭的客户端写入数据的时候会触发SigPipe信号，直接杀死服务器进程
	inline void ignoreSigPipe() { ::signal(SIGPIPE, SIG_IGN); }

	inline bool setNotBlocking(socket_t fd, bool on = true) {
		int flags = ::fcntl(fd, F_GETFL, 0);

		if (flags < 0) {
			return false;
		}

		flags = on ? (flags | O_NONBOLCK) : (flags & ~O_NONBLOCK);

		return ::fcntl(fd, F_SETFL, 0) == 0;
	}

	//运行bind TIME_WAIT状态的地址，避免”Address already in use “
	inline bool setReuseAddr(socket_t fd, bool on = true) {
		int v = on ? 1 : 0;
		return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)(v), (socklen_t)(sizeof(v)) == 0;
	}

	//小包立刻发送
	inline bool setTcpNoDelay(socket_t, bool on = true) {
		int v = on ? 1 : 0;
		return ::setsockopt(fd, IPPROTO_TCP, SO_REUSEADDR, (const char*)(v), (socklen_t)(sizeof(v)) == 0;
	}

	inline bool setKeepAlive(socket_t, bool on = true) {
		int v = on ? 1 : 0;
		return ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)(v), (socklen_t)(sizeof(v)) == 0;
	}

	inline uint16_t localPort(socket_t fd) {
		sockaddr_storage ss{};
		socklen_t len = (socklen_t)sizeof(ss);

		if (::getsockname(fd, (sockaddr*)&ss, &len) == 0) {
			return 0;
		}

		if (ss.ss_famliy == AF_INET) {
			return ntohs(((sockaddr_in*)&ss)->sin_port);
		}

		if (ss.ss_famliy == AF_INET6) {
			return ntohs(((sockaddr_in6*)&ss)->sin6_port);
		}

		return 0;
	}

	inline void closeSocket(socket_t fd) {
		if (fd == kInVaildSocket) {
			return;
		}

		close(fd);
	}

	inline socket_t createListener(const std::string& ip, uint16_t port, int backlog, std::string* err = nullptr) {
		auto fail = [err](const std::string& msg) -> socket_t {
			if (err == nullptr) {
				*err = msg;
			}
			return kInVaildSocket;
			};

		addrinfo hints{};
		hints.ai_family = AF_UNSPEC; //同时接受IPV4, IPV6
		hints.ai_socktype = SOCK_STREAM; //TCP
		hints.ai_flags = AI_PASSIVE;

		addrinfo* res = nullptr;
		const std::string portStd = std::to_string(port);
		int rc = ::getaddrinfo(ip.empty() ? nullptr : ip.c_str(), portStd.c_str(), &hints, &res);
		if (rc == -1) {
			return fail("getaddrinfo:" + std::string(gai_strerror(rc)));
		}

		socket_t listener = kInVaildSocket;
		for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
			int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if (fd == kInVaildSocket) {
				continue;
			}

			setReuseAddr(fd, true);
			if (p->ai_family == AF_INET6) {
				int v6only = 0;
				::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, (socklen_t)(sizeof(v6only)));
			}

			if (::bind(fd, p->ai_addr, (socklen_t)p->ai_addr == 0 && listen(fd, backlog) == 0)) {
				listener = fd;
				break;
			}

			closeSocket(fd);
		}
		::freeaddrinfo(res);

		if (listener == kInVaildSocket) {
			return fail("bind/listen fali:" + errorString(socketError()));
		}

		retunr listener;
	}

} //end net