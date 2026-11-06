#include "reactor.h"
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

struct event_wrapper {
  int fd;
  event_handler_t handler;
  void *arg;
};

reactor_t* reactor_create(void) {
  reactor_t *reactor = calloc(1, sizeof(reactor_t));
  if(reactor == NULL) return NULL;

  reactor->epoll_id = epoll_create1(0);
  if(reactor->epoll_id < 0) {
    free(reactor);
    return NULL;
  }

  reactor->running = false;
  return reactor;
}

void reactor_destory(reactor_t *reactor) {
  if(!reactor) return;

  for(int fd = 0; fd < MAX_FDS; ++fd) {
    free(reactor->slots[fd]);
    reactor->slots[fd] = NULL;
  }
  
  close(reactor->epoll_id);
  free(reactor);
}

static event_wrapper_t* wrapper_get(reactor_t *reactor, int fd) {
  if(fd < 0 || fd >= MAX_FDS) return NULL;
  return reactor->slots[fd];
}

static event_wrapper_t* wrapper_ensure(reactor_t *reactor, int fd) {
  if(fd < 0 || fd >= MAX_FDS) {
    printf("[Reactor] fd %d 超出影子表容量 %d\n", fd, MAX_FDS);
    return NULL;
  }

  if(reactor->slots[fd] == NULL) {
    reactor->slots[fd] = calloc(1, sizeof(event_wrapper_t));
    if(reactor->slots[fd]) reactor->slots[fd]->fd = fd;
  } 

  return reactor->slots[fd];
}

int reactor_add(reactor_t *reactor, int fd, uint32_t events, event_handler_t handler, void *arg) {
  event_wrapper_t *wrapper = wrapper_ensure(reactor, fd);
  if(!wrapper) return -1;

  int op = (wrapper->handler == NULL) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = wrapper;

  if(epoll_ctl(reactor->epoll_id, op, fd, &ev) < 0) {
    if(op == EPOLL_CTL_ADD) {
      free(wrapper);
      reactor->slots[fd] = NULL;
    }
    perror("epoll_ctl(add)");
    return -1;
  }

  wrapper->handler = handler;
  wrapper->arg = arg;
  
  return 0;
}

int reactor_modify(reactor_t *reactor, int fd, uint32_t events, event_handler_t handler, void *arg) {
  event_wrapper_t *wrapper = wrapper_get(reactor, fd);
  if(!wrapper) {
    fprintf(stderr, "[Reactor] modify no register fd %d\n", fd);
    return -1;
  }
 
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = wrapper;

  if(epoll_ctl(reactor->epoll_id, EPOLL_CTL_MOD, fd, &ev) < 0) {
    return -1;
  }

  wrapper->handler = handler;
  wrapper->arg = arg;   
  
  return 0;
}

int reactor_remove(reactor_t *reactor, int fd) {
  if(fd < 0 || fd >= MAX_FDS || reactor->slots[fd] == NULL) return -1;
  
  if(epoll_ctl(reactor->epoll_id, EPOLL_CTL_DEL, fd, NULL) < 0) {
    perror("epoll_ctl(remove)");
    return -1;
  }

  free(reactor->slots[fd]);
  reactor->slots[fd] = NULL;
  
  return 0;
}

void reactor_run(reactor_t *reactor) {
  reactor->running = true;

  while(reactor->running) {
    int nfds = epoll_wait(reactor->epoll_id, reactor->events, MAX_EVENTS, -1);

    if(nfds < 0) {
      if(errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }

    for(int i = 0; i < nfds; ++i) {
      event_wrapper_t *wrapper = reactor->events[i].data.ptr;
      if(!wrapper) continue;

      event_handler_t handler = wrapper->handler;
      int fd = wrapper->fd;
      void *arg = wrapper->arg;
      uint32_t events = reactor->events[i].events;

      if(handler) handler(reactor, fd, events, arg); 
    } 
  }
}

void reactor_stop(reactor_t *reactor) {
  reactor->running = false;
}

int set_noblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
