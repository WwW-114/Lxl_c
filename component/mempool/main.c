#include <stdio.h>
#include <stdlib.h>

#include "mempool.h"

#define DEMO_BLOCK_SIZE 32
#define DEMO_BLOCKS_PER_CHUNK 8
#define DEMO_MAX_CHUNKS 2
#define DEMO_TOTAL_BLOCKS (DEMO_BLOCKS_PER_CHUNK * DEMO_MAX_CHUNKS)

static void print_stats (const char *tag, const mempool_t *pool) {
  printf ("%s: 容量=%zu, 已用=%zu, 空闲=%zu, chunk=%zu\n",
          tag,
          mempool_capacity (pool),
          mempool_used (pool),
          mempool_available(pool),
          mempool_chunk_size (pool));
}

int main (void) {
  printf ("=== C 固定块内存池演示 ===\n");
  printf ("配置: 请求块大小=%d, 每 chunk 块数=%d, 最大 chunk 数=%d\n",
          DEMO_BLOCK_SIZE, DEMO_BLOCKS_PER_CHUNK, DEMO_MAX_CHUNKS);

  mempool_t *pool = mempool_create(DEMO_BLOCK_SIZE, DEMO_BLOCKS_PER_CHUNK, DEMO_MAX_CHUNKS);
  if(pool == NULL) {
    fprintf (stderr, "创建内存池失败\n");
    return EXIT_FAILURE;
  }

  printf ("创建: 实际块大小=%zu, 容量=%zu, 已用=%zu, 空闲=%zu, chunk=%zu\n",
          mempool_block_size (pool),
          mempool_capacity (pool),
          mempool_used (pool),
          mempool_available (pool),
          mempool_chunk_size (pool));

  void *p = NULL;
  void *blocks[DEMO_TOTAL_BLOCKS];
  size_t count = 0;
  
  for(size_t i = 0; i < DEMO_BLOCKS_PER_CHUNK; ++i) {
    p = mempool_alloc(pool);
    if(p == NULL) {
      fprintf (stderr, "分配 p%zu 失败\n", i);
      mempool_destroy(pool);
      return EXIT_FAILURE;
    }

    blocks[count++] = p;
    printf ("分配 p%zu = %p\n", i, p);
  }

  print_stats ("第一个 chunk 用尽", pool);

  p = mempool_alloc(pool);
  if (p == NULL) {
    fprintf (stderr, "扩容分配 p8 失败\n");
    mempool_destroy(pool);
    return EXIT_FAILURE;
  }
  
  blocks[count++] = p;
  printf ("扩容后分配 p%zu = %p\n", count - 1, p);
  print_stats ("扩容后", pool);

  /* 释放 p3，再分配，观察 LIFO 复用。 */
  void *reused = blocks[3];
  if (mempool_free (pool, reused) != 0) {
    fprintf (stderr, "释放 p3 失败\n");
    mempool_destroy (pool);
    return EXIT_FAILURE;
  }

  blocks[3] = NULL;
  printf ("释放 p3 = %p\n", reused);
  print_stats ("释放 p3 后", pool);

  p = mempool_alloc (pool);
  if (p == NULL) {
    fprintf (stderr, "复用分配 p9 失败\n");
    mempool_destroy (pool);
    return EXIT_FAILURE;
  }

  /*
   * 这里是第 10 次分配（p9），但 blocks 中的存活块数仍然是 9：
   * 它复用了 blocks[3] 的位置，因此 count 不需要增加。
   */
  blocks[3] = p;
  printf ("再次分配 p%zu = %p, 与 p3 %s\n",
          count, p, (p == reused) ? "相同 (LIFO)" : "不同");
  print_stats ("复用后", pool);

  /* 继续分配到当前容量上限。 */
  while (count < DEMO_TOTAL_BLOCKS) {
    p = mempool_alloc (pool);
    if (p == NULL) {
      fprintf (stderr, "分配 p%zu 失败\n", count);
      mempool_destroy (pool);
      return EXIT_FAILURE;
    }

    blocks[count++] = p;
  }
  print_stats ("达到容量上限", pool);

  /* 达到 max_chunks 后再分配，应返回 NULL。 */
  p = mempool_alloc (pool);
  if (p == NULL) {
    printf ("达到 max_chunks 后再分配 p%zu: 返回 NULL (符合预期)\n", count);
  } else {
    printf ("达到 max_chunks 后再分配 p%zu: 意外成功 (%p)，已归还\n", count, p);
    mempool_free (pool, p);
  }

  /* 释放一个块，再分配，不需要扩容。 */
  reused = blocks[count - 1];
  if (mempool_free (pool, reused) != 0) {
    fprintf (stderr, "释放 p%zu 失败\n", count - 1);
    mempool_destroy (pool);
    return EXIT_FAILURE;
  }

  blocks[count - 1] = NULL;
  count--;
  printf ("释放 p%zu = %p\n", count, reused);
  print_stats ("释放一个块后", pool);

  p = mempool_alloc (pool);
  if (p == NULL) {
    fprintf (stderr, "释放后再次分配 p%zu 失败\n", count);
    mempool_destroy (pool);
    return EXIT_FAILURE;
  }

  blocks[count++] = p;
  printf ("释放后再次分配 p%zu = %p\n", count - 1, p);
  print_stats ("再次分配后", pool);

  /* 全部归还。 */
  for (size_t i = 0; i < count; ++i) {
    if (blocks[i] != NULL) {
      if (mempool_free (pool, blocks[i]) != 0) {
        fprintf (stderr, "释放 p%zu 失败\n", i);
        mempool_destroy (pool);
        return EXIT_FAILURE;
      }
      blocks[i] = NULL;
    }
  }
  print_stats ("全部释放", pool);

  mempool_destroy (pool);
  printf ("销毁完成\n");

  return EXIT_SUCCESS;
}
