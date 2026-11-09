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
    thrd_pool->thread = (pthread_t*)calloc((size_t)thread_num, sizeof(pthread_t));
    if(thrd_pool->thread != NULL) {
      thrd_pool->queue = task_queue_create((size_t)queue_num, task_release);
      if(thrd_pool->queue != NULL) {
        if(pthread_mutex_init(&thrd_pool->mutex, NULL) != 0) {
          if(pthread_cond_init(&thrd_pool->all_idle, NULL) != 0) {
            int i = 0;

            for(; i < thread_num; ++i) {
              if(pthread_create(&thrd_pool->thread[i], NULL, work_main, thrd_pool) != 0) {
                if(i == thread_num - 1) {
                  return thrd_pool;
                }
              } else {
                task_queue_close(thrd_pool->queue, 0);
                break;
              }
            }

            for(int j = 0; j < i; ++j) {
              pthread_join(thrd_pool->thread[i], NULL);
            }
            
            pthread_cond_destroy(&thrd_pool->all_idle);
          }
          
          pthread_mutex_destroy(&thrd_pool->mutex);
        }
        
        free(thrd_pool->queue);
      }
          
      free(thrd_pool->thread);
    }
    
    free(thrd_pool);
  }

  return NULL;
}

void threadpool_destroy(threadpool_t *thrd_pool, int graceful) {
  if(thrd_pool == NULL) {
    return;
  }

  task_queue_close(thrd_pool->queue, graceful);

  for(int i = 0; i < thrd_pool->thread_num; ++i) {
    pthread_join(thrd_pool->thread[i], NULL);
  }

  task_queue_destroy(thrd_pool->queue);

  pthread_cond_destroy(&thrd_pool->all_idle);
  pthread_mutex_destroy(&thrd_pool->mutex);

  free(thrd_pool->thread);
  free(thrd_pool);
}

int threadpool_add(threadpool_t *thrd_pool, task_fn_t func, void *arg) {
  if(thrd_pool == NULL || func == NULL) {
    return -1;
  }

  task_t *task = (task_t*)malloc(sizeof(task_t));
  if(task == NULL) {
    return -1;
  }

  task->owner = thrd_pool;
  task->func = func;
  task->arg = arg;

  pthread_mutex_lock(&thrd_pool->mutex);
  thrd_pool->pending++;
  pthread_mutex_unlock(&thrd_pool->mutex);
  
  int rc = task_queue_push(thrd_pool->queue, task);
  if(rc != TQ_OK) {
    task_release(task);
    return -1;
  }

    return 0;
}

void threadpool_wait(threadpool_t *thrd_pool) {
  if(thrd_pool == NULL) {
    return;
  }

  pthread_mutex_lock(&thrd_pool->mutex);
  while(thrd_pool->pending > 0) {
    pthread_cond_wait(&thrd_pool->all_idle, &thrd_pool->mutex);
  }
  pthread_mutex_unlock(&thrd_pool->mutex);
}

unsigned long threadpool_pending (threadpool_t *thrd_pool) {
  if(thrd_pool == NULL) {
    return 0;
  }

  pthread_mutex_lock(&thrd_pool->mutex);
  unsigned long pending = thrd_pool->pending;
  pthread_mutex_unlock(&thrd_pool->mutex);

  return pending;
}

unsigned long threadpool_completed(threadpool_t *thrd_pool) {
  if(thrd_pool == NULL) {
    return 0;
  }

  pthread_mutex_lock(&thrd_pool->mutex);
  unsigned long completed = thrd_pool->completed;
  pthread_mutex_unlock(&thrd_pool->mutex);

  return completed;
}

size_t threadpool_queueud(threadpool_t *thrd_pool) {
  return thrd_pool == NULL ? 0 : task_queue_size(thrd_pool->queue);
}

