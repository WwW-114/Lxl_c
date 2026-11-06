#include <co/Scheduler.h>

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <utility>
#include <stdexcept>

namespace co {
  thread_local Scheduler* Scheduler::t_sched_ = nullptr;
  
  Scheduler::Scheduler()
   : epfd_(::epoll_create1(EPOLL_CLOEXEC)), stopping_(false) {
    if(epfd_ < 0) {
      throw std::runtime_error("epoll_create1 failed");
    }
  }

  Scheduler::~Scheduler() {
    ready_.clear();
    waiters_.clear();
    current_.reset();

    if(epfd_ >= 0) {
      ::close(epfd_);
    }
  }

  void Scheduler::spawn(Fiber::Fn fn, size_t stack_size) {
    ready_.push_back(std::make_shared<Fiber>(this, std::move(fn), stack_size));
  }

  void Scheduler::run() {
    Scheduler *prev = t_sched_;
    t_sched_ = this;

    while(!stopping_) {
      if(!ready_.empty()) {
        Fiber::Ptr fiber = ready_.front();
        ready_.pop_front();

        if(fiber->state_ != Fiber::READY) {
          continue;
        }

        current_ = fiber;
        fiber->state_ = Fiber::RUNNING;

        ::swapcontext(&sched_ctx_, &fiber->ctx_);

        current_.reset();
        continue;
      }

      if(waiters_.empty()) {
        break;
      }

      pollOnce();
    }

    t_sched_ = prev;
  }

  void Scheduler::pollOnce() {
    epoll_event events[64];
    int n = ::epoll_wait(epfd_, events, 64, -1);
    if(n < 0) {
      if(errno == EINTR) {
        return;
      }

      stopping_ = true;
      return;
    }

    for(int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      auto it = waiters_.find(fd);
      if(it == waiters_.end()) {
        continue;
      }

      ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
      it->second->state_ = Fiber::READY;
      ready_.push_back(it->second);
      waiters_.erase(it);
    }
  }

  void Scheduler::stop() {
    stopping_ = true;
  }

  void Scheduler::yieldCurrent() {
    if(!current_) {
      return;
    }

    current_->state_ = Fiber::READY;
    ready_.push_back(current_);
    ::swapcontext(&current_->ctx_, &sched_ctx_);
  }

  bool Scheduler::waitFd(int fd, uint32_t events) {
     if(!current_) {
       errno = EPERM;
       return false;
     }

     if(waiters_.find(fd) != waiters_.end()) {
       errno = EBUSY;
       return false;
     }

     epoll_event ev{};
     ev.events = events;
     ev.data.fd = fd;

     if(::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
       if(errno != EEXIST || ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
         return false;
       }
     }

     waiters_[fd] = current_;
     current_->state_ = Fiber::BLOCKED;
     ::swapcontext(&current_->ctx_, &sched_ctx_);
     
     return true;
  }

  Scheduler* Scheduler::current() {
    return t_sched_;
  }
}
