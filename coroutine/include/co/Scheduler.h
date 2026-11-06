#pragma once

#include <ucontext.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <memory>

#include <co/Fiber.h>


namespace co {
  class Scheduler {
  public:
    Scheduler();
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void spawn(Fiber::Fn fn, size_t stack_size = 128 * 1024);
    
    void run();

    void stop();

    void yieldCurrent();

    bool waitFd(int fd, uint32_t events);
    
    static Scheduler* current();

  private:
    friend class Fiber;

    void pollOnce();
    
    int epfd_;
    bool stopping_;
    std::deque<Fiber::Ptr> ready_;
    std::unordered_map<int, Fiber::Ptr> waiters_;
    ucontext_t sched_ctx_;
    Fiber::Ptr current_;
     
    static thread_local Scheduler* t_sched_;
  };
}
