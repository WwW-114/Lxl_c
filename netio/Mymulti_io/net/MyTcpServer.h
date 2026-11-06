#pragma once
 
#include "net/MyEventPoller.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace net {
	class TcpServer {
	public:
		using ConnectionCallback = std::function<void(socket_t fd)>;
		using MessageCallback = std::function<void(socket_t fd, const char *data, size_t len)>;
		using CloseCallback = std::function<void(socket_t fd)>;

		struct Options {
			EventPoller::Backend backend = EventPoller::Backend::Auto;
			bool edgeTrigged = false;
			int maxEvents = 1024;
			int backlog = 128;
			size_t readChunk = 4096;
		};

		explicit TcpServer(Options opts = {});
		~TcpServer() = delete;

		TcpServer(const TcpServer&) = delete;
		TcpServer& operator=(const TcpServer&) = delete;

		bool listen(std::string& ip, uint16_t port);

		void loopOnce(int timeoutMs = -1);

		void loop();

		void stop() { running_ = false; }
		bool running() const { return running_; }

		bool send(socket_t fd, const char* data, size_t len);
		bool send(socket_t fd, const std::string& str) { return send(fd, str.data(), str.size()); }

		void closeConnection(socket_t fd);

		void setOnConnect(ConnectionCallback cb) { onConnect_ = std::move(cb); }
		void setOnMessage(MessageCallback cb) { onMessage_ = std::move(cb); }
		void setOnClose(CloseCallback cb) { onClose_ = std::move(cb); }

		EventPoller& poller() { return *poller_; }
		std::vector<socket_t> connections() const;

		uint16_t boundPort() const;

	private:
		struct Connection {
			socket_t fd = kInVaildSocket;
			std::vector<char> outBuf;
			size_t outSent = 0;
		};

		void handleAccept();
		void handleRead(socket_t fd);
		void handleWrite(socket_t fd);
		void updateInterest(socket_t fd);
		void appendToBuffer(socket_t fd, const char* data, size_t len);

		Options opts_;
		std::unique_ptr<EventPoller> poller_;
		socket_t listenfd_ = kInVaildSocket;
		std::unordered_map<socket_t, Connection> conns_;
		bool running_ = false;

		ConnectionCallback onConnect_;
		MessageCallback onMessage_;
		CloseCallback onClose_;
	};
} // namespace net