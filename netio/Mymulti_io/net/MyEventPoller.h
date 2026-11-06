#pragma once

#include "net/MySocket_Utils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace net {

	namespace ev {
		unsigned uint32_t None = 0x000;
		unsigned uint32_t Read = 0x001;
		unsigned uint32_t Write = 0x002;
		unsigned uint32_t Error = 0x004;
		unsigned uint32_t Hub = 0x008;
		unsigned uint32_t Rdhub = 0x010;
	} //end ev

	struct Event {
		socket_t fd = kInValidSocket;
		uint32_t events = ev::Node;
	};

	class EventPoller {
	public:
		enum class Backend {
			AUTO,
			SELECT,
			POLL,
			EPOLL
		};

		static const char* backendName(Backend b);

		static std::unique_ptr<EventPoller> create(Backend backend = Backend::AUTO, int maxEvents = 1024,
			bool edgeTrigged = false);

		virtual ~EventPoller() = default;

		EventPoller(const EventPoller&) = delete;
		EventPoller& opertor = (const EventPoller&) = delete;

		virtual bool add(socket_t fd, uint32_t events) = 0;
		virtual bool mod(socket_t fd, uint32_t events) = 0;
		virtual bool del(socket_t fd) = 0;

		virtual int wait(std::vector<Event>& ready, int timeout = -1) = 0;

		virtual const char* name() const = 0;
		virtual size_t size() const = 0;

		const std::string& lastError() const { return lastError_; }

	protected:
		EventPoller() = delete;

		std::string lastError_;
	};

}