#ifndef KVSTORE_TABLE_H_
#define KVSTORE_TABLE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

enum {
  KVSTORE_TABLE_OK = 0,
  KVSTORE_TABLE_NOT_FOUND = -1,
  KVSTORE_TABLE_NOMEM = -2,
  KVSTORE_TABLE_INVALID = -3,
  KVSTORE_TABLE_LIMIT = -4
};

int kvstore_table_init(void);
void kvstore_table_destroy(void);

int kvstore_table_set(const char *key, const char *value);
int kvstore_table_del(const char *key);
int kvstore_table_mod(const char *key, const char *value);
const char* kvstore_table_get(const char *key);
size_t kvstore_table_count(void);

#ifdef __cplusplus
}
#endif

#endif
