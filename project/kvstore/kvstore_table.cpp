/*
 * kvstore 数据表实现：通过条件编译在多种数据结构之间切换。
 *
 * 由 CMake 选项 KVSTORE_TABLE_BACKEND 注入对应宏：
 *
 *   -DKVSTORE_TABLE_BACKEND=array   -> 定义 KVSTORE_TABLE_USE_ARRAY
 *       可变长数组：沿用原 kvstore_table.c 的 kv_entry_t / kv_table_t
 *       数据结构，以及 malloc / realloc / free 管理方式
 *
 *   -DKVSTORE_TABLE_BACKEND=rbtree  -> 定义 KVSTORE_TABLE_USE_RBTREE
 *       红黑树：使用 MyRedBlackTree.hpp 中的 MyRedBlackTree<K, V> 模板
 *
 * 说明：
 *   1. 红黑树是 C++ 模板，因此本编译单元统一使用 C++ 编译；
 *      数组分支的代码与原 C 实现逐行一致，仅改由 C++ 编译器编译，
 *      行为不变（malloc/free 与 new/delete 不会混用）。
 *   2. 对外仍然暴露 kvstore_table.h 中的 C 接口（头文件带 extern "C"），
 *      kvstore.c、kvstore_demo.c、peer.c、main.c 不需要任何修改。
 */

#include "kvstore_table.h"
#include "kvstore_config.h"

#if defined(KVSTORE_TABLE_USE_ARRAY)

#include <stdlib.h>
#include <string.h>

#define KVSTORE_TABLE_INIT_CAP 16

typedef struct kv_entry {
  char *key;
  char *value;
} kv_entry_t;

typedef struct kv_table {
  kv_entry_t *entries;
  size_t count;
  size_t cap;
} kv_table_t;

static kv_table_t g_table;

static char* kvstore_strdup(const char *s) {
  if(s == NULL) {
    return NULL;
  }

  size_t len = strlen(s) + 1;
  char *p = (char*)malloc(len);
  if(p == NULL) {
    return NULL;
  }

  memcpy(p, s, len);
  return p;
}

static long kv_find_index(const char *key) {
  for(size_t i = 0; i < g_table.count; ++i) {
    if(strcmp(g_table.entries[i].key, key) == 0) {
      return (long)i;
    }
  }

  return -1;
}

static int kv_ensure_capacity(size_t need) {
  if(need <= g_table.cap) {
    return KVSTORE_TABLE_OK;
  }

  size_t cap = g_table.cap ? g_table.cap : KVSTORE_TABLE_INIT_CAP;

  while(cap < need) {
    if(cap > (size_t)KVSTORE_TABLE_MAX_ENTRIES / 2) {
      cap = (size_t)KVSTORE_TABLE_MAX_ENTRIES;
      break;
    }

    cap *= 2;
  }

  if(cap < need) {
    return KVSTORE_TABLE_LIMIT;
  }

  kv_entry_t *entries = (kv_entry_t*)realloc(g_table.entries, cap * sizeof(kv_entry_t));
  if(entries == NULL) {
    return KVSTORE_TABLE_NOMEM;
  }

  g_table.entries = entries;
  g_table.cap = cap;

  return KVSTORE_TABLE_OK;
}

int kvstore_table_init(void) {
  if(g_table.entries != NULL) {
    kvstore_table_destroy();
  }

  g_table.count = 0;
  g_table.cap = 0;
  g_table.entries = NULL;

  return KVSTORE_TABLE_OK;
}

void kvstore_table_destroy(void) {
  if(g_table.entries != NULL) {
    for(size_t i = 0; i < g_table.count; ++i) {
      free(g_table.entries[i].key);
      free(g_table.entries[i].value);
    }

    free(g_table.entries);
  }

  g_table.cap = 0;
  g_table.count = 0;
  g_table.entries = NULL;
}

int kvstore_table_set(const char *key, const char *value) {
  if(key == NULL || value == NULL) {
    return KVSTORE_TABLE_INVALID;
  }

  size_t k_len = strlen(key);
  size_t v_len = strlen(value);

  if(k_len == 0 || k_len > (size_t)KVSTORE_KEY_MAX || v_len == 0 || v_len > (size_t)KVSTORE_VALUE_MAX) {
    return KVSTORE_TABLE_INVALID;
  }

  long idx = kv_find_index(key);
  if(idx >= 0) {
    char *new_value = kvstore_strdup(value);
    if(new_value == NULL) {
      return KVSTORE_TABLE_NOMEM;
    }

    free(g_table.entries[idx].value);
    g_table.entries[idx].value = new_value;
    return KVSTORE_TABLE_OK;
  }

  if(g_table.count >= (size_t)KVSTORE_TABLE_MAX_ENTRIES) {
    return KVSTORE_TABLE_LIMIT;
  }
  
  int rc = kv_ensure_capacity(g_table.count + 1);
  if(rc != KVSTORE_TABLE_OK) {
    return rc;
  }

  char *new_key = kvstore_strdup(key);
  if(new_key == NULL) {
    return KVSTORE_TABLE_NOMEM;
  }

  char *new_value = kvstore_strdup(value);
  if(new_value == NULL) {
    free(new_key);
    return KVSTORE_TABLE_NOMEM;
  }

  g_table.entries[g_table.count].key = new_key;
  g_table.entries[g_table.count].value = new_value;
  g_table.count++;

  return KVSTORE_TABLE_OK;
}

int kvstore_table_mod(const char *key, const char *value) {
  if(key == NULL || value == NULL) {
    return KVSTORE_TABLE_INVALID;
  }

  size_t key_len = strlen(key);
  size_t value_len = strlen(value);

  if(key_len == 0 || key_len > (size_t)KVSTORE_KEY_MAX ||
     value_len == 0 || value_len > (size_t)KVSTORE_VALUE_MAX) {
    return KVSTORE_TABLE_INVALID;
  }

  long idx = kv_find_index(key);
  if(idx < 0) {
    return KVSTORE_TABLE_NOT_FOUND;
  }

  char *new_value = kvstore_strdup(value);
  if(new_value == NULL) {
    return KVSTORE_TABLE_NOMEM;
  } 
  
  free(g_table.entries[idx].value);
  g_table.entries[idx].value = new_value;

  return KVSTORE_TABLE_OK;
}

int kvstore_table_del(const char *key) {
  if(key == NULL || key[0] == '\0') {
    return KVSTORE_TABLE_INVALID;
  }

  long idx = kv_find_index(key);
  if(idx < 0) {
    return KVSTORE_TABLE_NOT_FOUND;
  }

  size_t pos = (size_t)idx;
  size_t last = g_table.count - 1;

  free(g_table.entries[pos].key);
  free(g_table.entries[pos].value);

  if(pos != last) {
    g_table.entries[pos] = g_table.entries[last];
  }

  g_table.entries[last].key = NULL;
  g_table.entries[last].value = NULL;
  g_table.count = last;

  return KVSTORE_TABLE_OK;
}

const char* kvstore_table_get(const char *key) {
  if(key == NULL) {
    return NULL;
  }

  long idx = kv_find_index(key);
  if(idx < 0) {
    return NULL;
  }

  return g_table.entries[idx].value;
}

size_t kvstore_table_count(void) {
  return g_table.count;
}

#elif defined (KVSTORE_TABLE_USE_RBTREE)

#include "MyRedBlackTree.hpp"

#include <cstring>
#include <new>
#include <string>

typedef MyRedBlackTree<std::string, std::string> kv_tree_t;

static kv_tree_t *g_tree = nullptr;

int kvstore_table_init() {
  if(g_tree != nullptr) {
    kvstore_table_destroy();
  }

  try {
    g_tree = new kv_tree_t();
  } catch (const std::bad_alloc &) {
    g_tree = nullptr;
    return KVSTORE_TABLE_NOMEM;
  }

  return KVSTORE_TABLE_OK;
}

void kvstore_table_destroy() {
  if (g_tree != nullptr) {
    delete g_tree;
  }
  g_tree = nullptr; 
}

int kvstore_table_set(const char *key, const char *value) {
  if(key == nullptr || value == nullptr || g_tree == nullptr) {
    return KVSTORE_TABLE_INVALID;
  }

  const size_t k_len = std::strlen(key);
  const size_t v_len = std::strlen(value);

  if(k_len == 0 || k_len > (size_t)KVSTORE_KEY_MAX || v_len == 0 || v_len > (size_t)KVSTORE_VALUE_MAX) {
    return KVSTORE_TABLE_INVALID;
  }

  try {
    std::string new_key(key, k_len);
    std::string *slot = g_tree->find(new_key);

    if(slot != nullptr) {
      *slot = std::string(value, v_len);
      return KVSTORE_TABLE_OK;
    }

    if(g_tree->size() >= (size_t)KVSTORE_TABLE_MAX_ENTRIES) {
      return KVSTORE_TABLE_LIMIT;
    }

    g_tree->insert(new_key, std::string(value, v_len));
    return KVSTORE_TABLE_OK;
  } catch (const std::bad_alloc &) {
    return KVSTORE_TABLE_NOMEM;
  }
}

int kvstore_table_mod(const char *key, const char *value) {
  if(key == nullptr || value == nullptr || g_tree == nullptr) {
    return KVSTORE_TABLE_INVALID;
  }

  const size_t k_len = std::strlen(key);
  const size_t v_len = std::strlen(value);

  if(k_len == 0 || k_len > (size_t)KVSTORE_KEY_MAX || v_len == 0 || v_len > (size_t)KVSTORE_VALUE_MAX) {
    return KVSTORE_TABLE_INVALID;
  }

  try {
    std::string *slot = g_tree->find(std::string(key, k_len));

    if (slot == nullptr) {
      return KVSTORE_TABLE_NOT_FOUND;
    }

    *slot = std::string(value, v_len);
    return KVSTORE_TABLE_OK;
  } catch (const std::bad_alloc&) {
    return KVSTORE_TABLE_NOMEM;
  }
}

int kvstore_table_del(const char *key) {
  if(key == nullptr || key[0] == '\0' || g_tree == nullptr) {
    return KVSTORE_TABLE_INVALID;
  }

  try {
    return g_tree->remove(std::string(key)) ? KVSTORE_TABLE_OK : KVSTORE_TABLE_NOT_FOUND;
  } catch (const std::bad_alloc &) {
    return KVSTORE_TABLE_NOMEM;
  }
}

const char* kvstore_table_get(const char *key) {
  if (key == nullptr || g_tree == nullptr) {
    return nullptr;
  }

  try {
    const std::string *slot = g_tree->find(std::string(key));
    return (slot == nullptr) ? nullptr : slot->c_str(); 
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}

size_t kvstore_table_count() {
  return (g_tree == nullptr) ? 0 : g_tree->size();    
}

#else
#error "kvstore_table.cpp 需要 KVSTORE_TABLE_USE_ARRAY 或 KVSTORE_TABLE_USE_RBTREE 宏，请通过 CMake 选项 KVSTORE_TABLE_BACKEND 选择后端"
#endif
