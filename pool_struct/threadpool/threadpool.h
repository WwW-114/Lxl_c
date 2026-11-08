#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct threadpool threadpool_t;

typedef void (*task_fn_t)(void* /* ctx */);

threadpool_t* threadpool_create (int thread_num, int queue_num);

int threadpool_add (threadpool_t *thrd, task_fn_t func, void *arg);

void threadpool_wait (threadpool_t *thrd);

void threadpool_destroy (threadpool_t *thrd, int graceful);

unsigned long threadpool_pending (threadpool_t *thrd);

unsigned long threadpool_completed (threadpool_t *thrd);

unsigned long threadpool_queueud (threadpool_t *thrd);  

#ifdef __cplusplus
}
#endif

#endif
