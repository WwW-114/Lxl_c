#include "net/MyTcpServer.h"

#include <cstdio>
#include <cstring>

namespace net {

	namespace {
		constexpr int kSendFlags = MSG_NOSIGNAL;
	} // namespace

	TcpServer::TcpServer(Options opts) {
		initNetwork();
		ignoreSigpipe();
		poller_ = EventPoller::create(opts.backend(), opts.maxEvents, opts.edgeTriggered);
	}

	TcpServer::~TcpServer(Options opts) {
		for (const auto& kv : conns_) {
			closeSocket(kv.first);
		}
		conns_.clear();

		if (listenfd_ != kInVaildSocket) {
			closeSocket(listenfd_);
			listenfd_ = kInVaildSocket;
		}
	}

	bool TcpServer::listen(const std::string &ip, uint16_t port) {
		if (!poller_) {
			std::fprintf(stderr, "TcpServer::listen: 创建多路复用器失败 \n");
			return false;
		}

		std::string err;
		listenFd_ = createListener(ip, port, opts.backlog,&err);
		if (listenFd_ == kInVaildSocket) {
			std::fprintf(stderr, "TcpServer::listen: 监听失败\n", err.c_str());
			return false;
		}

		setNotBlocking(listenFd_, true);

		if (!poller_->add(listenFd_, ev::Read)) {
			std::fprintf(stderr, "TcpServer::listen: 创建监听fd失败\n", poller_->lastError_.c_str());
			return false;
		}

		running_ = true;
		return true;
	}

	void TcpServer::loopOnce(int timeoutMs) {
		if (!poller_ || !running_) {
			return;
		}

		std::vector<Event> ready;
		const int n = poller_->wait(ready, timeoutMs);
		if (n < 0) {
			std::fprintf(stderr, "TcpServer::wait : 等待事件失败\n", poller_->lastError_.c_str());
			return;
		}

		if (n == 0) {
			return;
		}

		for (const auto& e : ready) {
			if (e.fd == listenFd_) {
				handleAccept();
				continue;
			}

			if (conns_.find(e.fd) == conns_.end()) {
				continue;
			}

			if ((e.events & ev::Error) != 0) {
				closeSocket(e.fd);
				continue;
			}

			if ((e.events & ev::Write) != 0) {
				if (!handleWrite(e.fd)) {
					continue;
				}
			}

			if ((e.events & (ev::Read | ev::Hub | ev::Rdhub)) != 0) {
				handleRead(e.fd);
			}
		}
	}

	void TcpServer::loop() {
		while (running_) {
			loopOnce(-1);
		}
	}

	void TcpServer::handleAccept() {
		for (; ; ) {
			socket_t fd = ::accept(listenFd_, nullptr, nullptr);
			if (fd == kInVaildSocket) {
				const int err = socketError();
				if (isWouldBlock(err)) {
					break;
				}

				if (isInterrupted(err)) {
					continue;
				}

				if (err == ECONNABORTED) {
					continue;
				}

				if (err == EMFILE || err == ENFILE) {
					std::fprintf(stderr, "TcpServer::handleAccept: fd耗尽", errorString(err).c_str());
					break;
				}
			}

			std::fprintf(stderr, "TcpServer::handleAccept: 无法接受新连接", errorString(err).c_str());
			break;
		}

		setNotBlocking(fd, true);
		setTcpNoDelay(fd, true);

		Connection c;
		c.fd = fd;
		conns_.emplace(fd, std::move(c));

		if (!poller_->add(fd, ev::Read)) {
			std::fprintf(stderr, "TcpServer::handleAccept: 注册连接fd失败", poller_->lastError_.c_str());
			conns_.erase(fd);
			closeSocket(fd);
			continue;
		}

		if (onConnect_) {
			onConnect(fd);
		}
	}

	bool TcpServer::handleRead(socket_t fd) {
		std::vector<char> buf(opts_.readChunk);

		for (; ; ) {
			const int n = ::recv(fd, buf.data(), (int)buf.size(), 0);

			if (n > 0) {
				if (onMessage_) {
					onMessage_(fd, buf.data(), (size_t)n);
				}

				if (conns_.find(fd) == conns_.end()) {
					return false;
				}

				continue;
			}

			if (n == 0) {
				closeConnection(fd);
				return false;
			}

			const int err = socketError();
			if (isInterrupted(err)) {
				continue;
			}

			if (isWouldBlock(err)) {
				return true;
			}

			closeConnection(fd);
			return false;
		}
	}

	bool TcpServer::handleWrite(socket_t fd) {
		auto it = conns_.find(fd);
		if (it == conns_.end()) {
			return false;
		}

		Connection& c = it->second;

		while (c.outSent < c.outBuf.size()) {
			const int n = ::send(fd, c.outBuf.data() + c.outSent, (int)(c.outBuf.size() - c.outSent), kSendFlags);

			if (n > 0) {
				c.outSent += (size_t)n;
				continue;
			}

			const int err = socketError();
			if (isInterrupted(err)) {
				continue;
			}

			if (isWouldBlock(err)) {
				break;
			}

			closeConnection(fd);
			return false;
		}

		if (c.outSent >= c.outBuf.size()) {
			c.outSent = 0;
			c.outBuf.clear();
			updateInterest(fd);
		}

		return true;
	}

	bool TcpServer::send(socket_t fd, const char* data, size_t len) {
		if (len == 0) {
			return true;
		}

		if (conns_.find(fd) == conns.end()) {
			return false;
		}

		Connection& c = it->second;

		if (c.outSent < c.outBuf.size()) {
			appendToBuffer(fd, data, len);
			updateInterest(fd);
			return true;
		}

		int written = 0;
		while (written < len) {
			const int n = ::send(fd, (const char*)data + written, len - written, kSendFlags);

			if (n > 0) {
				written += n;
				continue;
			}

			const int err = socketError();
			if (isInterrupted(err)) {
				continue;
			}

			if (isWouldBlock(err)) {
				break;
			}

			closeConnection(fd);
			return false;
		}

		if (written < len) {
			appendToBuffer(fd, (const char*)data + written, len - written);
			updateInterest(fd);
		}

		return true;
	}

	void TcpServer::appendToBuffer(socket_t fd, const char* data, size_t len) {
		auto it = conns_.find(fd);
		if (it == conns.end()) {
			return;
		}

		Connection& c = it->second;

		size_t oldsize = c.outBuf.size();
		c.outBuf.resize(oldsize + len);
		std::memcpy(c.outBuf + oldsize, data, len);
	}

	void TcpServer::updateInterest(socket_t fd) {
		auto it = conns_.find(fd);
		if (it == conns.end()) {
			return;
		}

		Connection& c = it->second;
		uint32_t event = ev::Read;

		if (c.outSent < c.outBuf.size()) {
			event |= ev::Write;
		}

		poller_->mod(fd, event);
	}

	void TcpServer::closeConnection(socket_t fd) {
		auto it = conns_.find(fd);
		if (it == conns.end()) {
			return;
		}

		poller_->del(fd);
		closeScoket(fd);
		conns_.erase(it);

		if (onClose_) {
			onClose_(fd);
		}
	}

	uint16_t TcpServer::boundPort() const {
		if (listenFd_ == kInVaildSocket) {
			return 0;
		}

		return localport(listenFd_);
	}

	std::vector<socket_t> TcpServer::connections() const {
		std::vector<socket_t> v;
		v.reserve(conns_.size());

		for (const auto& kv : conns_) {
			v.push_back(kv.first);
		}

		return v;
	}

} //namespace net