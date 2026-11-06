#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define TEST_MESSAGE "Payload=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\n\0"
#define RBUFFER_SIZE 128
#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

struct test_context_s {
  char server_ip[16];
  uint32_t port;
  uint32_t thread_num;
  uint32_t conns;
  uint32_t requestion;
  uint32_t failed;
}; 

typedef struct test_context_s test_context_t;

int connect_tcpserver(const char *ip, unsigned short port) {
  int connfd = socket(AF_INET, SOCK_STREAM, 0);
  if(connfd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in tcpserver_addr;
  tcpserver_addr.sin_family = AF_INET;
  tcpserver_addr.sin_addr.s_addr = inet_addr(ip);
  tcpserver_addr.sin_port = htons(port);

  if((connect(connfd, (struct sockaddr*)&tcpserver_addr, sizeof(tcpserver_addr))) == -1) {
    perror("connect");
    close(connfd);
    return -1;
  }

  return connfd;
}

int send_recv_tcppkt(int fd) {
  int res = send(fd, TEST_MESSAGE, sizeof(TEST_MESSAGE), 0);
  if(res < 0) {
    exit(1);
  }

  char rbuffer[RBUFFER_SIZE] = {0};
  int n = recv(fd, rbuffer, RBUFFER_SIZE, 0);
  if(n <= 0) {
    exit(1);
  }
  
  if(strcmp(rbuffer, TEST_MESSAGE) != 0) {
    printf("failed = '%s' != '%s'\n", rbuffer, TEST_MESSAGE);
    return -1;
  }

  //printf("server echo is: %s\n", rbuffer);
  
  return 0;
}

static void* test_qps_entry(void *arg) {
  test_context_t *ctx = (test_context_t*)arg;
  
  int connfd = connect_tcpserver(ctx->server_ip, ctx->port);
  if(connfd < 0) {
    printf("connect_tcpserver failed\n");
    ctx->failed++;
    return NULL;
  }

  int count = ctx->requestion / ctx->thread_num;
  
  for(int i = 0; i < count; ++i) {
    int res = send_recv_tcppkt(connfd);
    if(res != 0) {
      printf("send_recv_tcppkt failed\n");
      ctx->failed++;
      return NULL;
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  int ret = 0;
  int opt;

  test_context_t ctx;
  
  while((opt = getopt(argc, argv, "s:p:t:c:n:?")) != -1) {
    switch (opt) {
      
      case 's':
        printf("-s = %s\n", optarg);
        strcpy(ctx.server_ip, optarg);
        break;
      case 'p':
        printf("-p = %s\n", optarg);
        ctx.port = atoi(optarg);
        break;
      case 't':
        printf("-t = %s\n", optarg);
        ctx.thread_num = atoi(optarg);
        break;
      case 'c':
        printf("-c = %s\n", optarg);
        ctx.conns = atoi(optarg);
        break;
      case 'n':
        printf("-n = %s\n", optarg);
        ctx.requestion = atoi(optarg);
        break;
      default:
        printf("Unkown command\n");
        return -1;
    }
  }
  
  pthread_t *ptid = (pthread_t*)malloc(ctx.thread_num * sizeof(pthread_t));
  if(ptid == NULL) {
    printf("thread malloc failed\n");
    return -1;
  }

  struct timeval tv_begin = {0};
  gettimeofday(&tv_begin, NULL);
  
  for(int i = 0; i < ctx.thread_num; ++i) {
    pthread_create(&ptid[i], NULL, test_qps_entry, &ctx);
  }

  for(int i = 0; i < ctx.thread_num; ++i) {
    pthread_join(ptid[i], NULL);
  }

  struct timeval tv_end = {0};
  gettimeofday(&tv_end, NULL);

  int timeused = TIME_SUB_MS(tv_end, tv_begin);
  
  printf("success conns = %d, failed conns = %d, timeused = %d, qps = %d\n",
         ctx.requestion - ctx.failed, ctx.failed, timeused, ctx.requestion * 1000 / timeused);
  
  free(ptid);
  
  return ret;
}
