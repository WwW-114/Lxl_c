#include "mempool.h"
#include "free_list.h"

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#ifndef MEMPOOL_ALIGN
#  if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined (_MSC_VER)
#    define MEMPOOL_ALIGN _Alignof(max_align_t)
#  else
#    define MEMPOOL_ALIGN 16 
#  endif
#endif

typedef struct mp_chunk {
  struct mp_chunk *next;
  size_t count;
} mp_chunk_t;

struct mempool {
  size_t block_size;
  size_t block_per_chunk;
  size_t data_offset;
  size_t chunk_bytes;
  size_t max_chunks;
  size_t chunk_count;
  size_t capacity;
  free_list_t free_list;
  mp_chunk_t *chunks;
};

static int align_up (size_t value, size_t alignment, size_t *out) {
  size_t remainder = 0;
  size_t padding = 0;

  assert(alignment > 0);
  assert(out != NULL);

  remainder = value % alignment;
  if(remainder == 0) {
    *out = value;
    return 0;
  }

  padding = alignment - remainder;

  if (value > SIZE_MAX - padding) {
    return -1;
  }

  *out = value + padding;
  return 0;
}

static int mempool_grow (mempool_t *pool) {
  assert(pool != NULL);

  if (pool->max_chunks == 0 || pool->chunk_count < pool->max_chunks) {
    if(pool->chunk_count < SIZE_MAX) {
      if(pool->capacity <= SIZE_MAX - pool->block_per_chunk) {
        mp_chunk_t *chunk = (mp_chunk_t*)malloc(pool->chunk_bytes);

        if(chunk != NULL) {
          chunk->count = pool->block_per_chunk;
          
          chunk->next = pool->chunks;
          pool->chunks = chunk;
          pool->chunk_count++;
          pool->capacity += pool->block_per_chunk;

          unsigned char *base = (unsigned char*)chunk + pool->data_offset;
          for(size_t i = pool->block_per_chunk; i > 0; --i) {
            free_list_push(&pool->free_list, base + (i - 1) * pool->block_size);
          }

          return 0;
        }
      }
    }
  }

  return -1;
}

mempool_t* mempool_create(size_t block_size, size_t blocks_per_chunk, size_t max_chunks) {
  if(block_size > 0 && blocks_per_chunk > 0) {
    size_t requested_block_size = block_size;
    if(requested_block_size < sizeof(free_node_t)) {
      requested_block_size = sizeof(free_node_t);
    }

    size_t actual_block_size = 0;
    if(align_up(requested_block_size, MEMPOOL_ALIGN, &actual_block_size) == 0) {
      size_t data_offset = 0;
      
      if(align_up(sizeof(mp_chunk_t), MEMPOOL_ALIGN, &data_offset) == 0) {
        
        if(blocks_per_chunk <= (SIZE_MAX - data_offset) / actual_block_size) {
          size_t chunk_bytes = blocks_per_chunk * actual_block_size + data_offset;

          if(max_chunks == 0 || blocks_per_chunk <= SIZE_MAX / max_chunks) {
            mempool_t *pool = calloc(1, sizeof(mempool_t));

            if(pool != NULL) {
              pool->block_per_chunk = blocks_per_chunk;
              pool->block_size = actual_block_size;
              pool->chunk_bytes = chunk_bytes;
              pool->max_chunks = max_chunks;
              pool->data_offset = data_offset;
              free_list_init(&pool->free_list);

              if(mempool_grow(pool) == 0) {
                return pool;
              }
              
              free(pool);
            }
          }
        }
      }
    }
  }
  
  return NULL;
}

void mempool_destroy(mempool_t *pool) {
   if(pool == NULL) {
     return;
   }

   mp_chunk_t *chunk = pool->chunks;
   while(chunk != NULL) {
     mp_chunk_t *next = chunk->next;
     free(chunk);
     chunk = next;
   }

   free(pool);
}

void* mempool_alloc(mempool_t *pool) {
  if(pool == NULL) {
    return NULL;
  }

  if(free_list_is_empty(&pool->free_list)) {
    if(mempool_grow(pool) != 0) {
      return NULL;
    }
  }

  return free_list_pop(&pool->free_list);
}

int mempool_free(mempool_t *pool, void *data) {
  if(pool == NULL || data == NULL) {
    return -1;
  }

#ifdef MEMPOOL_DEBUG
  if(!mempool_contains(pool, data)) {
    return -1;
  }
#endif

  free_list_push(&pool->free_list, data);
  return 0;
}

size_t mempool_block_size(const mempool_t *pool) {
  return pool == NULL ? 0 : pool->block_size;
}

size_t mempool_capacity(const mempool_t *pool) {
  return pool == NULL ? 0 : pool->capacity;
}

size_t mempool_available(const mempool_t *pool) {
  return pool == NULL ? 0 : free_list_size(&pool->free_list);
}

size_t mempool_chunk_size(const mempool_t *pool) {
  return pool == NULL ? 0 : pool->chunk_count;
}

size_t mempool_used(const mempool_t *pool) {
  if(pool == NULL) {
    return 0;
  }

  size_t available = free_list_size(&pool->free_list);
  assert(pool->capacity >= available);
  return pool->capacity - available; 
}

int mempool_contains(const mempool_t *pool, const void *data) {
  if(pool == NULL || data == NULL) {
    return 0;
  }

  uintptr_t target = (uintptr_t)data;

  for(mp_chunk_t *chunk = pool->chunks; chunk != NULL; chunk = chunk->next) {
    size_t block_bytes = pool->block_size * chunk->count;
    uintptr_t base = (uintptr_t)chunk + pool->data_offset;

    if(base > UINTPTR_MAX - block_bytes) {
      continue;
    }

    uintptr_t end = (uintptr_t)base + block_bytes;

    if(target >= base && target < end && (target - base) % pool->block_size == 0) {
      return 1;
    }
  }

  return 0;
}

