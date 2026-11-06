#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define TEST_MESSAGE "Payload=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\n\0"
#define RBUFFER_SIZE 128

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

  printf("server echo is: %s\n", rbuffer);
  
  return 0;
}

int main(int argc, char **argv) {
  int opt;
  
  char server_ip[16] = {0};
  uint32_t port = 0;
  uint32_t thread_num = 0;
  uint32_t conns = 0;
  uint32_t requestion = 0;

  while((opt = getopt(argc, argv, "s:p:t:c:n:?")) != -1) {
    switch (opt) {
      
      case 's':
        printf("-s = %s\n", optarg);
        strcpy(server_ip, optarg);
        break;
      case 'p':
        printf("-p = %s\n", optarg);
        port = atoi(optarg);
        break;
      case 't':
        printf("-t = %s\n", optarg);
        thread_num = atoi(optarg);
        break;
      case 'c':
        printf("-c = %s\n", optarg);
        conns = atoi(optarg);
        break;
      case 'n':
        printf("-n = %s\n", optarg);
        requestion = atoi(optarg);
        break;
      default:
        printf("Unkown command\n");
        return -1;
    }
  }

  int connfd = connect_tcpserver(server_ip, port);
  if(connfd < 0) {
    printf("connect_tcpserver failed\n");
    return -1;
  }

  int res = send_recv_tcppkt(connfd);
  if(res != 0) {
    printf("send_recv_tcppkt failed\n");
    return -1;
  }

  printf("success!!!\n");
  
  return 0;
}
