#pragma once

#include <ucontext.h>

#include <cstddef>
#include <functional>
#include <memory>

namespace co {
  class Scheduler;

  class Fiber {
  public:
    using Fn = std::function<void()>;
    using Ptr = std::shared_ptr<Fiber>;

    enum State {
      READY,
      RUNNING,
      BLOCKED,
      DEAD
    };

    Fiber(Scheduler *sched, Fn fn, size_t stack_size = 128 * 1024);
    ~Fiber();

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    void yield();

    State state() const { return state_; }

  private:
    friend class Scheduler;

    static void trampoline();
    
    Scheduler *sched_;
    Fn fn_;
    ucontext_t ctx_;
    void *stack_;
    size_t stack_size_;
    State state_;
  };

  void yield();
}
