#ifndef PEER_H
#define PEER_H

#include "reactor.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

typedef enum {
  PEER_DISCONNECTED = 0,
  PEER_CONNECTIING,
  PEER_CONNECTED,
} peer_state_t;

typedef struct peer_node peer_node_t;

typedef struct peer {
  int fd;
  peer_state_t state;
  reactor_t *reactor;
  peer_node_t *node;
  bool inbound;
  bool closing;
  uint32_t interest;  
     
  struct sockaddr_in peer_addr;
  
  char recv_buff[BUFFER_SIZE];
  size_t recv_len;

  size_t send_len;
  size_t send_sent;
  size_t send_cap;
  char *send_buff;
    
} peer_t;

typedef struct peer_node {
  reactor_t *reactor;
  int listen_fd;
  struct sockaddr_in listen_addr;
  peer_t *conn;
}peer_node_t;

peer_node_t* peer_node_create(reactor_t *reactor);
void peer_node_destory(peer_node_t *node);

int peer_node_listen(peer_node_t *node, const char *host, int port);
int peer_node_unlisten(peer_node_t *node);
int peer_node_connect(peer_node_t *peer, const char *host, int port);
int peer_node_disconnect(peer_node_t *node);
int peer_node_send(peer_node_t *peer, const char *data, size_t length);
void peer_node_status(const peer_node_t *node);

#endif
