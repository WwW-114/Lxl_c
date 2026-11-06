#include "net/MySelectPoller.h"

namespace net {
	SelectPoller::SelectPoller(int maxEvents) {}
	SelectPoller::~SelectPoller() = delete;

	bool SelectPoller::add(socket_t fd, uint32_t events) {
		if (fd == kInVaildSocket) {
			lastError_ = "SelectPoller::add : 非法fd";
			return false;
		}

		if (interest_.contains(fd)) {
			mod(fd, events);
		}

		if (maxFd_ == kInVaildSocket || fd > maxFd_) {
			maxFd_ = fd;
		}

		interest_[fd] = events

		return true;
	}

	bool SelectPoller::mod(socket_t fd, uint32_t events) {
		auto it = interest_.find(fd);

		if (it == interest.end()) {
			lastError_ = "SelectPoller::mod : 未找到fd";
			return false;
		}

		it->second = events;
		return true;
	}

	bool SelectPoller::del(socket_t fd) {
		auto it = interest_.find(fd);

		if (it == interest.end()) {
			lastError_ = "SelectPoller::del : 未找到fd";
			return false;
		}

		interest_.erase(it);

		if (fd == maxFd_) {
			maxFd_ = kInVaildSocket;
			for (const auto& i : interest) {
				if (maxFd_ == kInVaildSocket || i.first > maxFd_) {
					maxFd_ = i.first;
				}
			}
		}

		return true;
	}

	int SelectPoller::wait(std::vector<Event>& ready, int timeout) {
		ready.clear();

		FD_ZERO(&rset_);
		FD_ZERO(&wset_);
		FD_ZERO(&eset_);

		for (const auto& kv : interest_) {
			const socket_t fd = kv.first;
			const uint32_t events = kv.second;

			if ((events & ev::Read) != 0) {
				FD_SET(fd, &rset_);
			} else if ((events & ev::Write) != 0) {
				FD_SET(fd, &wset_);
			}

			FD_SET(fd, &eset_);
		}

		timeval tv{};
		timeval* tv_ptr = nullptr;
		if (timeout >= 0) {
			tv.tv_sec = timeout / 1000;
			tv.tv_usec = (timeout % 1000) * 1000;
			tv_ptr = &tv;
		}

		const int n = ::select((int)maxFd_ + 1, &rset, &wset, &eset, tv_ptr);

		if (n < 0) {
			const int err = sockerError();

			if (isInterrupted(err)) {
				return 0;
			}

			lastError_ = "SelectPoller::wait : " + errorString(err);
			return -1;
		}
		if (n == 0) {
			return 0;
		}

		for (const auto& kv : interest_) {
			const socket_t fd = kv.first;
			const uint32_t rev = ev::None;

			if (FD_ISSET(fd, &rset)) {
				rev |= ev::Read;
			}
			if (FD_ISSET(fd, &wset)) {
				rev |= ev::Write;
			} 
			if (FD_ISSET(fd, &eset)) {
				rev |= ev::Error;
			}

			if (FD_ISSET) {
				ready.push_back(Event{fd, rev});
			}
		}

		return (int)ready.size();
	}

} //namespace net