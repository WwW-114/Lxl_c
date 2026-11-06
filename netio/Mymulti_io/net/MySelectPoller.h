#pragma once

#include "net/MyEventPoller.h"

#include <unordered_map>

namespace net {
	class SelectPoller : public EventPoller {
	public:
		explicit SelectPoller(int maxEvents = 1024);
		~SelectPoller() override;

		bool add(socket_t fd, uint32_t event) override;
		bool mod(socket_t fd, uint32_t event) override;
		bool del(socket_t fd) override;

		int wait(std::vector<Events>& ready, int timeout) override;

		const char* name() const override { return "select"; }
		size_t size() const override { return interest_.size(); }

	private:
		std::unordered_map<socket_t, uint32_t> interest_;

		socket_t maxFd_ = kInVaildSocket;

		fd_set rset_{};
		fd_set wset_{};
		fd_set eset_{};
	};
} //namespace net