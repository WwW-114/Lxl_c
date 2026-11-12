#include "free_list.h"

#include <assert.h>

void free_list_init(free_list_t *list) {
  assert(list != NULL);

  list->head = NULL;
  list->count = 0;
}

void free_list_push(free_list_t *list, void *block) {
  free_node_t *node;

  assert(list != NULL);
  assert(block != NULL);

  node = (free_node_t*)block;

  node->next = list->head;
  list->head = node;

  list->count++;
}

void* free_list_pop(free_list_t *list) {
  assert(list != NULL);

  free_node_t *node = list->head;
  if(node == NULL) {
    return NULL;
  }

  assert(list->count > 0);

  list->head = node->next;
  list->count--;

  return (void*)node;
}

int free_list_is_empty(const free_list_t *list) {
  assert(list != NULL);

  return list->head == NULL;
}

size_t free_list_size(const free_list_t *list) {
  assert(list != NULL);

  return list->count;
}
