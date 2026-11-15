#include "kvstore.h"
#include "kvstore_table.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

typedef enum kv_cmd {
  KV_CMD_UNKNOWN = 0,
  KV_CMD_SET,
  KV_CMD_GET,
  KV_CMD_MOD,
  KV_CMD_DEL,
} kv_cmd_t;

static int kv_is_space(const char c) {
  return c == ' ';
}

static kv_cmd_t kv_parse_cmd(const char *s, size_t len) {
  if(len == 3 && memcmp(s, "SET", 3) == 0) return KV_CMD_SET;
  if(len == 3 && memcmp(s, "GET", 3) == 0) return KV_CMD_GET;
  if(len == 3 && memcmp(s, "MOD", 3) == 0) return KV_CMD_MOD;
  if(len == 3 && memcmp(s, "DEL", 3) == 0) return KV_CMD_DEL;
  return KV_CMD_UNKNOWN;
}

static int kv_vaild_key(const char *s, size_t len) {
  if(len == 0 || len > (size_t)KVSTORE_KEY_MAX) {
    return 0;
  }

  for(size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)s[i];

    if(c == ' ' || c < 0x20 || c == 0x7f) {
      return 0;
    }
  }

  return 1;
}

static int kv_vaild_value(const char *s, size_t len) {
  if(len == 0 || len > (size_t)KVSTORE_VALUE_MAX) {
    return 0;
  }

  for(size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)s[i];
    if(c == '\0' || c == '\r' || c == '\n' || c < 0x20 || c == 0x7f) {
      return 0;
    }
  }
  
  return 1;
}

static int resp_append(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
  if(*used >= cap) {
    return -1;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
  va_end(ap);

  if(n < 0) {
    return -1;
  }

  if((size_t) n >= cap - *used) {
    return -1;
  }

  *used += n;
  return 0;
}

static int kv_reply_error(char *response, size_t cap, size_t *len,
                          const char *code, const char *message) {
  return resp_append(response, cap, len, "-ERR %s %s", code, message);
}

static int kv_reply_not_found(char *response, size_t cap, size_t *len,
                              const char *key) {
  return resp_append(response, cap, len, "-ERR NOTFOUND %s", key);
}

static int kv_map_table_status(int status, const char *key,
                               char *response, size_t cap, size_t *len) {
  switch(status) {
    case KVSTORE_TABLE_OK:
      return resp_append(response, cap, len, "+OK");
    case KVSTORE_TABLE_NOT_FOUND:
      return kv_reply_not_found(response, cap, len, key);
    case KVSTORE_TABLE_NOMEM:
      return resp_append(response, cap, len,
                         "-ERR NOMEM out of memory");
    case KVSTORE_TABLE_LIMIT:
      return resp_append(response, cap, len,
                         "-ERR LIMIT store is full");
    case KVSTORE_TABLE_INVALID:
    default:
      return resp_append(response, cap, len,
                         "-ERR BADREQ invalid key or value");
  }
}

int kvstore_handle_request(const char *request, size_t request_len,
                                  char *response, size_t response_cap, size_t *response_len) {
  if(request == NULL || response == NULL || response_len == NULL || response_cap == 0) {
    return 0;
  }

  response[0] = '\0';
  response_len = 0;

  if(memchr(request, '\0', request_len) != NULL) {
    return kv_reply_error(response, response_cap, response_len, "BADREQ", "embedded null byte");
  }

  size_t pos = 0;
  
  while(pos < request_len && kv_is_space(request[pos])) {
    ++pos;
  }

  if(pos == request_len) {
    return 0;
  }

  size_t cmd_start = pos;
  while(pos < request_len && !kv_is_space(request[pos])) {
    ++pos;
  }

  int cmd_len = pos - cmd_start;

  kv_cmd_t cmd_type = kv_parse_cmd(request + cmd_start, cmd_len);
  if(cmd_type == KV_CMD_UNKNOWN) {
    kv_reply_error(response, response_cap, response_len, "BADREQ", "unknown command");
  }

  while(pos < request_len && kv_is_space(request[pos])) {
    ++pos;
  }

  if(pos == request_len) {
    kv_reply_error(response, response_cap, response_len, "BADREQ", "missing key");
  }

  size_t key_start = pos;
  while(pos < request_len && !kv_is_space(request[pos])) {
    ++pos;
  }

  size_t key_len = pos - key_start;

  if(key_len > (size_t)KVSTORE_KEY_MAX) {
    kv_reply_error(response, response_cap, response_len, "TOOLONG", "key too long");
  }

  if(kv_vaild_key(request + key_start, key_len) != 1) {
    kv_reply_error(response, response_cap, response_len, "BADREQ", "invalid key");
  }
  
  char tmp_key[KVSTORE_KEY_MAX + 1];
  memcpy(tmp_key, request + key_start, key_len);
  tmp_key[key_len] = '\0';

  while(pos < request_len && kv_is_space(response[pos])) {
    ++pos;
  }

  if(cmd_type == KV_CMD_GET || cmd_type == KV_CMD_DEL) {
    if(pos != request_len) {
      kv_reply_error(response, response_cap, response_len, "BADREQ", "unexpected arguments");
    }

    if(cmd_type == KV_CMD_GET) {
      const char *value = kvstore_table_get(tmp_key);
      if(value == NULL) {
        kv_reply_not_found(response, response_cap, response_len, tmp_key);
      }

      return resp_append(response, response_cap, response_len, "+VALUE %s", value);
    }

    int rc = kvstore_table_del(tmp_key);
    return kv_map_table_status(rc, tmp_key, response, response_cap, response_len);
  }

  if(pos == request_len) {
    kv_reply_error(response, response_cap, response_len, "BADREQ", "missing value");
  }

  size_t value_len = request_len - pos;

  if(value_len > (size_t)KVSTORE_VALUE_MAX) {
    return kv_reply_error(response, response_cap, response_len,
                          "TOOLONG", "value too long");
  }

  if(!kv_vaild_value(request + pos, value_len)) {
    return kv_reply_error(response, response_cap, response_len,
                          "BADREQ", "invalid value");
  }

  char value[KVSTORE_VALUE_MAX + 1];
  memcpy(value, request + pos, value_len);
  value[value_len] = '\0';

  int rc = (cmd_type == KV_CMD_SET)
               ? kvstore_table_set(tmp_key, value)
               : kvstore_table_mod(tmp_key, value);

  return kv_map_table_status(rc, tmp_key, response,
                             response_cap, response_len);
}

