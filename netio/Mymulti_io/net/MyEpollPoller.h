#pragma once

#include "net/MyEventPoller.h"

#include <epoll.h>
#include <unordered_map>
#include <vector>

namespace net {
	class EpollPoller : public EventPoller {
	public:
		explicit EpollPoller(int maxEvents = 1024, bool edgeTriggered = false);
		~EpollPoller() = override;

		bool add(socket_t fd, uint32_t event) override;
		bool mod(socket_t fd, uint32_t event) override;
		bool del(socket_t fd) override;

		int wait(std::vector<Event>& ready, int timeout) override;

		const char* name() const override { return edgeTriggered_ ? "epoll(ET)" : "epoll(LT)"; }
		size_t size() const override { return interest_.size(); }

		bool edgeTriggered() const { return edgeTriggered_; }

		bool rearm(socket_t fd, uint32_t events);

	private:
		uint32_t toEpoll(uint32_t events) const;
		static uint32_t fromEpoll(uint32_t e);

		int epfd_ = -1;
		int maxEvents_ = 1024;
		bool edgeTriggered_ = false;

		std::vector<epoll_event> events_;

		std::unordered_map<socket_t, uint32_t> interest_;
	};

} //namespace net