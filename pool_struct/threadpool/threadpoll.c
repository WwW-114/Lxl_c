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

static void pool_task_finish (threadpool_t *thrd_pool, int mark_completed) {
  pthread_mutex_lock(&thrd_pool->mutex);

  thrd_pool->pending--;
  if(mark_completed == 1) {
    thrd_pool->completed++;
  }
  if(thrd_pool->pending == 0) {
    pthread_cond_broadcast(&thrd_pool->all_idle);
  }

  pthread_mutex_unlock(&thrd_pool->mutex);
}

static void task_release (void *data) {
  task_t *task = (task_t*)data;
  threadpool_t *thrd_pool = task->owner;

  free(task);
  pool_task_finish(thrd_pool, 0);
}

static void* work_main (void *arg) {
  threadpool_t *thrd_pool = (threadpool_t*)arg;
  void *data = NULL;
  
  for(; ;) {
    int rc = task_queue_pop(thrd_pool->queue, &data);
    if(rc != TQ_OK) {
      break;
    }

    task_t *task = (task_t*)data;
    task->func(task->arg);
    free(task);

    pool_task_finish(thrd_pool, 1);
  }
  
  return NULL;
}

threadpool_t* threadpool_create (int thread_num, int queue_num) {
  if (thread_num <= 0) {
    thread_num = 1;
  }
  if (queue_num <= 0) {
    queue_num = 64;
  }

  threadpool_t *thrd_pool = (threadpool_t*)calloc(1, sizeof(threadpool_t));
  if(thrd_pool != NULL) {
    thrd_pool->thread = ()
    
    free(thrd_pool);
  }

  return NULL;
}
