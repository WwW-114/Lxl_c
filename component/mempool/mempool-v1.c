#include <stdio.h>
#include <stdlib.h>

#define MEM_PAGE_SIZE 4096

typedef struct mempool_s {
  int block_size;
  int free_cnt;

  char *free_ptr;
  char *mem;
} mempool_t;

int mem_init (mempool_t *m, int size) {
  if (m == NULL) {
    printf("init error\n");
    return -1;
  }

  if (size < 16) {
    size = 16;
  }
  
  m->free_cnt = MEM_PAGE_SIZE / size;
  m->block_size = size;

  m->mem = (char*)malloc(MEM_PAGE_SIZE);
  if(!m->mem) {
    printf("m->mem alloc error\n");
    return -1;
  }

  void *ptr = m->mem;
  m->free_ptr = m->mem;
  
  for(int i = 0; i < m->free_cnt; ++i) {
    *(char**)ptr = ptr + size;
    ptr += size;
  }

  *(char**)ptr = NULL;
  
  return 0;
}

void mem_destroy (mempool_t *m) {
  if(!m || !m->mem) {
    return;
  }

  free(m->mem);
}

void* mem_alloc (mempool_t *m) {
  if(!m || m->free_cnt == 0) {
    return NULL;
  }

  void *ptr = m->free_ptr;
  m->free_ptr = *(char**)ptr;
  m->free_cnt++;
  
  return ptr;
}

int mem_free (mempool_t *m, void *ptr) {
  if(!m || !ptr) {
    return -1;
  }

  *(char**)ptr = m->free_ptr;
  m->free_ptr = (char*)ptr;
  m->free_cnt++;
  
  return 0;
}

int main () {
  mempool_t m;

  int rc = mem_init(&m, 32);
  if(rc == -1) {
    return -1;
  }

  void *p1 = mem_alloc(&m);
  printf("p1 = %p\n", p1);

  void *p2 = mem_alloc(&m);
  printf("p2 = %p\n", p2);

  void *p3 = mem_alloc(&m);
  printf("p3 = %p\n", p3);

  void *p4 = mem_alloc(&m);
  printf("p4 = %p\n", p4);

  rc = mem_free(&m, p3);
  if(rc == -1) {
    return -1;
  }
  
  void *p5 = mem_alloc(&m);
  printf("p5 = %p\n", p5);

  mem_destroy(&m);
}
