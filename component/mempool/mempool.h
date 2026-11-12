#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct mempool mempool_t;

mempool_t* mempool_create (size_t block_size, size_t blocks_per_chunk, size_t max_chunks);

void mempool_destroy (mempool_t *pool);

void* mempool_alloc (mempool_t *pool);

int mempool_free (mempool_t *pool, void *data);

size_t mempool_block_size (const mempool_t *pool);

size_t mempool_capacity (const mempool_t *pool);

size_t mempool_used (const mempool_t *pool);

size_t mempool_available (const mempool_t *pool);

size_t mempool_chunk_size (const mempool_t *pool);

int mempool_contains (const mempool_t *pool, const void *data);

#ifdef __cplusplus
}
#endif

#endif
