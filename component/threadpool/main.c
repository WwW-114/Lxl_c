#define _GNU_SOURCE

#include "threadpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define WORKER_COUNT 4
#define QUEUE_CAPACITY 4
#define JOB_COUNT 12

static pthread_mutex_t g_print_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  int id;
  int work_ms;
} job_t;

void job_run (void *arg) {
  job_t *job = (job_t*)arg;
  unsigned long tid = (unsigned long)pthread_self();
  
  pthread_mutex_lock(&g_print_lock);
  printf("  [线程 %lu] 开始任务 #%02d（预计 %d ms）\n",
           tid, job->id, job->work_ms);
  pthread_mutex_unlock(&g_print_lock);

  usleep((useconds_t)(job->work_ms * 1000));

  pthread_mutex_lock(&g_print_lock);
  printf("  [线程 %lu] 完成任务 #%02d\n", tid, job->id);
  pthread_mutex_unlock(&g_print_lock);

  free(job);
}

int main (void) {
  printf("=== C 线程池演示（队列与线程池分离） ===\n");
  printf("工作线程: %d，队列容量: %d，任务总数: %d\n\n",
           WORKER_COUNT, QUEUE_CAPACITY, JOB_COUNT);

  threadpool_t *thrd_pool = threadpool_create(WORKER_COUNT, QUEUE_CAPACITY);
  if(thrd_pool == NULL) {
    fprintf(stderr, "创建线程池失败\n");
    return -1;
  }

  for (int i = 0; i < JOB_COUNT; ++i) {
    job_t *job = (job_t*)malloc(sizeof(job_t));
    if (job == NULL) {
      fprintf(stderr, "分配任务失败\n");
      break;
    }

    job->id = i + 1;
    job->work_ms = 100 + (i % 5) * 100;

    if(threadpool_add(thrd_pool, job_run, job) != 0) {
      fprintf(stderr, "提交任务 #%d 失败（线程池已关闭？）\n", job->id);
      free(job);
      break;
    }

    printf("主线程: 提交任务 #%02d，队列中 %zu 个，未完成 %lu 个\n",
               job->id, threadpool_queued(thrd_pool), threadpool_pending(thrd_pool));
  }

  printf("\n主线程: 等待剩余任务执行完毕...\n");
  threadpool_wait(thrd_pool);

  printf("主线程: 全部完成，累计完成 %lu 个任务\n",
          threadpool_completed(thrd_pool));

  threadpool_destroy(thrd_pool, 1);
  printf("主线程: 线程池已优雅关闭\n");
  return 0;
}
