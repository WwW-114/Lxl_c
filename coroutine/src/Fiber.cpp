#include <co/Fiber.h>
#include <co/Scheduler.h>

#include <new>
#include <utility>

namespace co {
  Fiber::Fiber(Scheduler *sched, Fn fn, size_t stack_size)
    : sched_(sched),
      fn_(std::move(fn)),
      stack_(nullptr),
      stack_size_(stack_size),
      state_(READY)  
  {
    stack_ = ::operator new(stack_size_);

    ::getcontext(&ctx_);
    ctx_.uc_stack.ss_sp = stack_;
    ctx_.uc_stack.ss_size = stack_size_;
    ctx_.uc_stack.ss_flags = 0;
    ctx_.uc_link = &sched_->sched_ctx_;

    ::makecontext(&ctx_, &Fiber::trampoline, 0);
  }

  Fiber::~Fiber() {
    ::operator delete(stack_);
  }

  void Fiber::yield() {
    sched_->yieldCurrent();
  }

  void Fiber::trampoline() {
    Scheduler *sched = Scheduler::current();
    Fiber *self = sched->current_.get();

    try {
      self->fn_();
    } catch (...) {
    
    }

    self->fn_ = nullptr;
    self->state_ = DEAD;

    ::swapcontext(&self->ctx_, &sched->sched_ctx_);
  }

  void yield() {
    if(Scheduler *sched = Scheduler::current()) {
      sched->yieldCurrent();
    }
  }
}
