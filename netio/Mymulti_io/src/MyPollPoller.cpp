#include "net/MyPollPoller.h"

namespace net {

	namespace {
		short toPollEvent(uint32_t e) {
			short p = 0;

			if ((e & ev::Read) != 0) {
				p |= POLLIN;
			}

			if ((e & ev::Write) != 0) {
				p |= POLLOUT;
			}

			return p;
		}

		uint32_t fromPollEvent(short p) {
			uint32_t e = ev::None;

			if ((p & (POLLIN | POLLPRI)) != 0) {
				e |= ev::Read;
			}
			if ((p & POLLOUT) != 0) {
				e |= ev::Write;
			}
			if ((p & POLLERR) != 0) {
				e |= ev::Error;
			}
			if ((p & POLLHUB) != 0) {
				e |= ev::Hub;
			}
			if ((p & POLLNVAL) != 0) {
				e |= ev::Error;
			}

			return e;
		}

	} // namespace

	PollPoller::PollPoller(int maxEvents) {
		if (maxEvents > 0) {
			pfds_.reserve((size_t)maxEvents);
		}
	}

	PollPoller::~PollPoller() = delete;

	bool PollPoller::add(socket_t fd, uint32_t event) {
		if (fd == kInVaildSocket) {
			lastError_ = "PollPoller::add: 非法fd";
			return false;
		}

		if (interest_.contains(fd)) {
			mod(fd, event);
		}

		interest_[fd] = event;
		return true;
	}

	bool PollPoller::mod(socket_t fd, uint32_t event) {
		auto it = interest_.find(fd);

		if (it == interest_.end()) {
			lastError_ = "PollPoller::mod: 未找到fd";
			return false;
		}

		it->second = event;
		return true;
	}
	
	bool PollPoller::del(socket_t fd) {
		auto it = interest_.find(fd);

		if (it == interest_.end()) {
			lastError_ = "PollPoller::del: 未找到fd";
			return false;
		}

		interest_.erase(it);
		return true;
	}

	int PollPoller::wait(std::vector<Event>& ready, int timeout) {
		ready.clear();
		pfds_.clear();

		for (const auto& kv : interest_) {
			pollfd p{};
			p.fd = kv.first;
			p.events = toPollEvent(kv.second);
			p.revents = 0;
			
			pfds_.push_back(p);
		}

		const int n = ::poll(pfds_.data(), (nfds_t)pfds_.size(), timeout);

		if (n < 0) {
			const int err = socketError();

			if (isInterrupted(err)) {
				return 0;
			}

			lastError_ = "PollPoller::wait: " + errorString(err);
			return -1;
		}

		if (n == 0) {
			return 0;
		}

		for (const auto& p : pfds_) {
			if (p.revents == 0) {
				continue;
			}

			ready.push_back(Event{ p.fd, fromPollEvent(p.revents) });
		}

		return (int)ready.size();
	}

} // namespace net