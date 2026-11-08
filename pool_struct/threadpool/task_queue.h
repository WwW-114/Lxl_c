#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct task_queue task_queue_t;

typedef enum {
  TQ_OK = 0,
  TQ_FULL = 1,
  TQ_EMPTY = 2,
  TQ_CLOSED = -1,
  TQ_NOMEM = -2,
  TQ_INVAL = -3
} tq_status_t;

task_queue_t* task_queue_create (size_t capacity, void (*free_fn)(void *data));

void task_queue_destroy (task_queue_t *q);

int task_queue_push (task_queue_t *q, void *data);

int task_queue_try_push (task_queue_t *q, void *data);

int task_queue_pop (task_queue_t *q, void **out);

int task_queue_try_pop (task_queue_t *q, void **out);

void task_queue_close (task_queue_t *q, int graceful);

int task_queue_is_closed (task_queue_t *q);
size_t task_queue_size (task_queue_t *q);
size_t task_queue_capacity (task_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif
