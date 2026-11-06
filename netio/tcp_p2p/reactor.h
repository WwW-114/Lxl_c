#ifndef REACTOR_H
#define REACTOR_H

#include <stdint.h>
#include <sys/epoll.h>
#include <stdbool.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define MAX_FDS 1024

typedef struct reactor reactor_t;

//事件回调函数定义
typedef void (*event_handler_t)(reactor_t *reactor, int fd, uint32_t events,void *arg);

typedef struct event_wrapper event_wrapper_t;

struct reactor {
  int epoll_id;
  bool running;
  struct epoll_event events[MAX_EVENTS];
  event_wrapper_t *slots[MAX_FDS];
};

reactor_t* reactor_create(void);
void reactor_destory(reactor_t*);

int reactor_add(reactor_t *reactor, int fd, uint32_t events, event_handler_t handler, void *arg);
int reactor_modify(reactor_t *reactor, int fd, uint32_t events, event_handler_t handler, void *arg);
int reactor_remove(reactor_t *reactor, int fd);

void reactor_run(reactor_t*);
void reactor_stop(reactor_t*);

//将socket设为非阻塞
int set_noblocking(int fd);

#endif
