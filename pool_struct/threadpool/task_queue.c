#include "task_queue.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct queue_node {
  void *data;
  struct queue_node *next;
} queue_node_t;

struct task_queue {
  pthread_mutex_t mutex;
  pthread_cond_t not_full;
  pthread_cond_t not_empty;

  queue_node_t *head;
  queue_node_t *tail;
  size_t size;
  size_t capacity;

  int closed;
  void (*free_fn)(void *);
};

static void free_node_list (task_queue_t *q, queue_node_t *node) {
  while (node != NULL) {
    queue_node_t *next = node->next;
    if(q->free_fn != NULL) {
      q->free_fn(node->data);
    }
    free(node);
    node = next;
  }
}

task_queue_t* task_queue_create(size_t capacity, void (*free_fn)(void *data)) {
  if(capacity != 0) {
    task_queue_t *q = (task_queue_t*)calloc(1, sizeof(task_queue_t));
    if(q != NULL) {
      q->capacity = capacity;
      q->free_fn = free_fn;
      
      if(pthread_mutex_init(&q->mutex, NULL) == 0) {
        if(pthread_cond_init(&q->not_empty, NULL) == 0) {
          if(pthread_cond_init(&q->not_full, NULL) == 0) {
            return q;
          }
          
          pthread_cond_destroy(&q->not_empty);
              
        }
        
        pthread_mutex_destroy(&q->mutex);
      }
      
      free(q);
    }
  }

  return NULL;
}

void task_queue_destroy(task_queue_t *q) {
  if(q == NULL) {
    return;
  }

  pthread_mutex_lock(&q->mutex);
  q->closed = 1;
  queue_node_t *node = q->head;
  q->head = NULL;
  q->tail = NULL;
  q->size = 0;
  pthread_cond_broadcast(&q->not_empty);
  pthread_cond_broadcast(&q->not_full);
  pthread_mutex_unlock(&q->mutex);

  free_node_list(q, node);

  pthread_mutex_destroy(&q->mutex);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);

  free(q);
}

void task_queue_close(task_queue_t *q, int graceful) {
  if(q == NULL) {
    return;
  }

  queue_node_t *discard = NULL;

  pthread_mutex_lock(&q->mutex);
  if(q->closed == 1) {
    pthread_mutex_unlock(&q->mutex);
    return;
  }
  
  q->closed = 1;

  if(!graceful) {
    discard = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
  }

  pthread_cond_broadcast(&q->not_empty);
  pthread_cond_broadcast(&q->not_full);
  pthread_mutex_destroy(&q->mutex);

  free_node_list(q, discard);
}

int task_queue_push(task_queue_t *q, void *data) {
  if(q == NULL) {
    return TQ_INVAL;
  }

  queue_node_t *node = (queue_node_t*)malloc(sizeof(queue_node_t));
  if(node == NULL) {
    return TQ_NOMEM;
  }

  node->data = data;
  node->next = NULL;
  
  pthread_mutex_lock(&q->mutex);
  
  while(q->size >= q->capacity && !q->closed) {
    pthread_cond_wait(&q->not_full, &q->mutex);
  }
  
  if(q->closed) {
    pthread_mutex_unlock(&q->mutex);
    free(node);
    return TQ_CLOSED;
  }

  if(q->tail == NULL) {
    q->head = node;
  } else {
    q->tail->next = node;
  }

  q->tail = node;
  ++q->size;

  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);

  return TQ_OK;
}

int task_queue_try_push(task_queue_t *q, void *data) {
  
if(q == NULL) {
    return TQ_INVAL;
  }

  queue_node_t *node = (queue_node_t*)malloc(sizeof(queue_node_t));
  if(node == NULL) {
    return TQ_NOMEM;
  }

  node->data = data;
  node->next = NULL;
  
  pthread_mutex_lock(&q->mutex);
  
  if(q->closed) {
    pthread_mutex_unlock(&q->mutex);
    free(node);
    return TQ_CLOSED;
  }

  if(q->size >= q->capacity) {
    pthread_mutex_unlock(&q->mutex);
    free(node);
    return TQ_FULL;
  }
  
  if(q->tail == NULL) {
    q->head = node;
  } else {
    q->tail->next = node;
  }

  q->tail = node;
  ++q->size;

  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);

  return TQ_OK;
}

int task_queue_pop(task_queue_t *q, void **out) {
  if(q == NULL || out == NULL) {
    return TQ_INVAL;
  }

  pthread_mutex_lock(&q->mutex);

  while(q->size == 0 && !q->closed) {
    pthread_cond_wait(&q->not_empty, &q->mutex);
  }

  if(q->size == 0) {
    pthread_mutex_unlock(&q->mutex);
    return TQ_CLOSED;
  }

  queue_node_t *node = q->head;
  q->head = q->head->next;
  if(q->head == NULL) {
    q->tail = NULL;
  }

  --q->size;

  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);

  *out = node->data;
  free(node);
    
  return TQ_OK;
}


int task_queue_try_pop(task_queue_t *q, void **out) {
  if(q == NULL || out == NULL) {
    return TQ_INVAL;
  }

  pthread_mutex_lock(&q->mutex);

  if(q->closed) {
    pthread_mutex_unlock(&q->mutex);
    return TQ_CLOSED;
  }

  if(q->size == 0) {
    pthread_mutex_unlock(&q->mutex);
    return TQ_EMPTY;
  }

  queue_node_t *node = q->head;
  q->head = q->head->next;
  if(q->head == NULL) {
    q->tail = NULL;
  }

  --q->size;

  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);

  *out = node->data;
  free(node);
  
  return TQ_OK;
}

int task_queue_is_closed(task_queue_t *q) {
  if(q == NULL) {
    return 1;
  }

  pthread_mutex_lock(&q->mutex);
  int closed = q->closed;
  pthread_mutex_unlock(&q->mutex);

  return closed;
}

size_t task_queue_size(task_queue_t *q) {
  if(q == NULL) {
    return 0;
  }

  pthread_mutex_lock(&q->mutex);
  int size = q->size;
  pthread_mutex_unlock(&q->mutex);

  return size;
}

size_t task_queue_capacity(task_queue_t *q) {
  return q == NULL ? 0 : q->capacity;
}
