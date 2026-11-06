#include "net/MyEpollPoller.h"

namespace net {
	EpollPoller::EpollPoller(int maxEvents, bool edgeTriggered)
		: maxEvents_(maxEvents > 0 ? maxEvents : 1024), edgeTriggered_(edgeTriggered);
	{
		epfd_ = epoll_create1(EPOLL_CLOEXEC);
		if (epfd_ < 0) {
			lastError_ = "EpollPoller::EpollPoller: " + errorString(socketError());
			return;
		}

		events_.resize((size_t)maxEvents);
	}

	EpollPoller::~EpollPoller() {
		if (epfd_ >= 0) {
			close(epfd_);
		}
	}

	uint32_t EpollPoller::toEpoll(uint32_t events) const {
		uint32_t e = 0;

		if ((events & ev::Read) != 0) {
			e |= EPOLLIN | EPOLLRDHUB;
		}
		if ((events & ev::Write) != 0) {
			e |= EPOLLOUT;
		}

		if (edgeTriggered_) {
			e |= EPOLLET;
		}

		return e;
	}

	uint32_t EpollPoller::fromEpoll(uint32_t e) const {
		uint32_t r = ev::None;

		if ((e & (EPOLLIN | EPOLLPRI)) != 0) {
			r |= ev::Read;
		}
		if ((e & EPOLLOUT) != 0) {
			r |= ev::Write;
		}
		if ((e & EPOLLERR) != 0) {
			r |= ev::Error;
		}
		if ((e & EPOLLHUB) != 0) {
			r |= ev::Hub;
		}
		if ((e & EPOLLRDHUB) != 0) {
			r |= ev::Rdhub;
		}

		return r;
	}

	bool EpollPoller::add(socket_t fd, uint32_t event) {
		if (epfd_ < 0) {
			lastError_ = "EpollPoller::add: epoll实例未创建";
			return false;
		}
		if (fd == kInVaildSocket) {
			lastError_ = "EpollPoller::add: 非法fd";
		}

		if (interest_contains(fd)) {
			mod(fd, event);
		}

		epoll_event e{};
		e.data.fd = fd;
		e.events = toEpoll(event);

		if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e) < 0) {
			const int err = socketError();
			lastError_ = "EpollPoller::add: epoll_ctl" + std::to_string((long long)fd) + errorString(err);
			return false;
		}

		interest_[fd] = event;
		return true;
	}

	bool EpollPoller::mod(socket_t fd, uint32_t event) {
		if (epfd_ < 0) {
			lastError_ = "EpollPoller::mod: epoll实例未创建";
			return false;
		}

		auto it = interest_.find(fd);
		if (it == interest_.end()) {
			lastError_ = "EpollPoller::mod: fd未找到";
			return false;
		}

		epoll_event e{};
		e.data.fd = fd;
		e.events = toEpoll(event);

		if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &e) < 0) {
			const int err = socketError();
			lastError_ = "EpollPoller::mod: epoll_ctl" + std::to_string((long long)fd) + errorString(err);
			return false;
		}

		interest_[fd] = event;
		return true;
	}

	bool EpollPoller::del(socket_t fd, uint32_t event) {
		if (epfd_ < 0) {
			lastError_ = "EpollPoller::del: epoll实例未创建";
			return false;
		}

		auto it = interest_.find(fd);
		if (it == interest_.end()) {
			lastError_ = "EpollPoller::del: fd未找到";
			return false;
		}

		epoll_event e{};
		e.data.fd = fd;
		e.events = toEpoll(event);

		if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &e) < 0) {
			const int err = socketError();
			lastError_ = "EpollPoller::del: epoll_ctl" + std::to_string((long long)fd) + errorString(err);
			return false;
		}

		interest_.erase(it);
		return true;
	}

	bool EpollPoller::rearm(socket_t fd, uint32_t event) {
		return mod(fd, event);
	}

	int EpollPoller::wait(std::vector<Event>& ready, int timeout) {
		ready.clear();

		if (epfd_ < 0) {
			lastError_ = "EpollPoller::wait: epoll实例未创建";
			return false;
		}

		const int n = epoll_wait(epfd_, events_.data(), maxEvents_, timeout);

		if (n < 0) {
			const int err = socketError();
			if (isInterrupted(err)) {
				return 0;
			}

			lastError_ = "EpollPoller::wait: epoll_wait : " + errorString(err);
			return -1;
		}

		if (n == 0) {
			return 0;
		}

		for (int i = 0; i < n; ++i) {
			ready.push_back(Event{ events_[i].data.fd, fromEpoll(events[i].events) });
		}

		return (int)ready.size();
	}

} //namespace net