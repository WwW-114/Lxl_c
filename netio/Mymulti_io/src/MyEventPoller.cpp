#include "net/MyEventPoller.h"
#include "net/MySelectPoller.h"
#include "net/MyPollPoller.h"
#include "net/MyEpollPoller.h"

namespace net {
	const char* EventPoller::backendName(Backend b) {
		switch (b) {
		case Backend::AUTO: return "auto";
		case Backend::SELECT: return "select";
		case Backend::POLL: return "poll";
		case Backend::EPOLL: return "epoll";
		}

		return "unknown";
	}

	std::unique_ptr<EventPoller> EventPoller::create(Backend backend, int maxEvents, bool edgeTriggered) {
		if (backend == Backend::AUTO) {
			backend = Backend::EPOLL;
		}

		switch (backend)
		{
			case Backend::Select
				return std::make_unique<SelectPoller>(maxEvents);
			case Backend::Poll
					return std::make_unique<PollPoller>(maxEvents);
			case Backend::Epoll
					return std::make_unique<EpollPoller>(maxEvents);

			default:
			break;
		}

		return nullptr;
	}

} //namespace net