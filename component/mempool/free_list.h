#ifndef FREE_LIST_H
#define FREE_LIST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct free_node_s {
  struct free_node_s *next;
} free_node_t;

typedef struct free_list_s {
  free_node_t *head;
  size_t count;
} free_list_t;

void free_list_init (free_list_t *list);

void free_list_push (free_list_t *list, void *block);

void* free_list_pop (free_list_t *list);

int free_list_is_empty (const free_list_t *list);

size_t free_list_size (const free_list_t *list);

#ifdef __cplusplus
}
#endif

#endif
