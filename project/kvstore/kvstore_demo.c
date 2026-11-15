#include "kvstore.h"
#include "kvstore_table.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  if(kvstore_table_init() != KVSTORE_TABLE_OK) {
    fprintf(stderr, "kvstore_table_init failed\n");
    return 1;
  }

  char line[KVSTORE_REQUEST_MAX + 2];

  while(fgets(line, (int)sizeof(line), stdin) != NULL) {
    size_t len = strlen(line);

    while(len > 0 &&
          (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }

    char response[KVSTORE_RESPONSE_MAX];
    size_t response_len = 0;

    if(kvstore_handle_request(line, len,
                              response, sizeof(response),
                              &response_len) < 0) {
      fprintf(stderr, "kvstore internal error\n");
      break;
    }

    if(response_len > 0) {
      printf("%s\n", response);
      fflush(stdout);
    }
  }

  kvstore_table_destroy();
  return 0;
}
