#pragma once

#include "net/MyEventPoller.h"

#include <poll.h>
#include <unordered_map>
#include <vector>

namespace net {
	class PollPoller : public EventPoller {
	public:
		explicit PollPoller(int maxEvents = 1024);
		~PollPoller() override;

		bool add(socket_t fd, uint32_t event) override;
		bool mod(socket_t fd, uint32_t event) override;
		bool del(socket_t fd) override;

		int wait(std::vector<Event>& ready, int timeout) override;

		const char* name() const override { return "poll"; }
		size_t size const override{ return interest_.size(); }

	private:
		std::unordered_map<socket_t, uint32_t> interest_;
		std::vector<pollfd> pfds_;
	};

} //namespace net