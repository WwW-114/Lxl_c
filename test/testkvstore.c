#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define MAX_MSG_LENGTH 512
#define TEST_COUNT 1000000

#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

#ifdef ENABLE_LOG
#define ENABLE_LOG 1
#define LOG(_fmt, ...) fprintf(stdout, "[%s:%d]: %s"_fmt, __FILE__, __LINE__, __VAR_ARGS__)
#else
#define LOG(_fmt, ...)
#endif

int send_msg(int connfd, char *msg, int length) {
  int res = send(connfd, msg, length, 0);
  if(res < 0) {
    perror("send");
    exit(1);
  }

  return res;
}

int recv_msg(int connfd, char *msg, int length) {
  int res = recv(connfd, msg, length, 0);
  if(res < 0) {
    perror("recv");
    exit(1);
  }

  return res;
}

void test_case(int connfd, char *msg) {
  if(!msg) {
    return;
  }
  
  int rc = send_msg(connfd, msg, strlen(msg));
  if(rc < 0) {
    return;
  }

  LOG(stdout, "[Send]: %s\n", msg);

  char result[MAX_MSG_LENGTH];
  rc = recv_msg(connfd, result, MAX_MSG_LENGTH);
  if(rc < 0) {
    return;
  }

  LOG(stdout, "[Receive]: %s\n", result);
}

void array_test(int connfd) {
  int i = 0;

  while(i++ < TEST_COUNT) {
    test_case(connfd, "SET user:1 alice\n");
    test_case(connfd, "GET user:1\n");
    test_case(connfd, "MOD user:1 bob\n");
    test_case(connfd, "GET user:1\n");
    test_case(connfd, "DEL user:1\n");
    test_case(connfd, "GET user:1\n");
  }
}

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

int main(int argc, char **argv) {
  int ret = 0;
  int opt;

  char ip[16] = {};
  int port = 0;
  int mode = 1;
  
  while((opt = getopt(argc, argv, "s:p:m:?")) != -1) {
    switch (opt) {
      case 's':
        printf("-s = %s\n", optarg);
        strcpy(ip, optarg);
        break;
      case 'p':
        printf("-p = %s\n", optarg);
        port = atoi(optarg);
        break;
      case 'm':
        printf("-m = %s\n", optarg);
        mode = atoi(optarg);
        break;
      default:
        printf("Unkown command\n");
        return -1;
    }
  }

  int connfd = connect_tcpserver(ip, port);

  struct timeval tv_begin = {0};
  gettimeofday(&tv_begin, NULL);
  
  if(mode & 0x001) {
    array_test(connfd);
  }

  struct timeval tv_end = {0};
  gettimeofday(&tv_end, NULL);

  int timeused = TIME_SUB_MS(tv_end, tv_begin);
  
  printf("timeused = %d, qps = %lld\n",
         timeused, (long long)TEST_COUNT * 6 * 1000 / timeused);

  
  return ret;
}


