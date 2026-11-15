#include "kvstore_table.h"
#include "kvstore_config.h"

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
      cap = KVSTORE_TABLE_MAX_ENTRIES;
      break;
    }

    cap *= 2;
  }

  if(cap < need) {
    return KVSTORE_TABLE_INIT_CAP;
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

  free(g_table.entries[idx].key);
  free(g_table.entries[idx].value);

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
