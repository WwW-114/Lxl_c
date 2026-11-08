#include "task_queue.h"
#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct {
  task_fn_t func;
  void *arg;
  threadpool_t *owner;
} task_t;

struct threadpool {
  task_queue_t *queue;

  pthread_t *thread;
  int thread_num;

  pthread_mutex_t mutex;
  pthread_cond_t all_idle;

  unsigned long pending;
  unsigned long completed;
};

static void pool_task_finish (threadpool_t *thdr, int mark_completed) {
  pthread_mutex_lock(&thdr->mutex);

  
}
