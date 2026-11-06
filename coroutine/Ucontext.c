#include <ucontext.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <poll.h>
#include <sys/epoll.h>

#define _GNU_SOURCE
#include <dlfcn.h>

ucontext_t cxt[3];
ucontext_t main_cxt;
int count = 0;
uint32_t sockfd = -1;

typedef ssize_t (*read_t) (int fd, void *buf, size_t size);
read_t read_f = NULL;

typedef  ssize_t (*write_t) (int fd, const void* buf, size_t size);
write_t write_f = NULL;

ssize_t read(int fd, void *buf, size_t size) {
  struct pollfd fds[1] = {0};

  fds[0].fd = sockfd;
  fds[0].events = POLLIN;

  int res = poll(&fds[0], 1, 0);
  if(res <= 0) {
    //epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
    //swapcontext(, );
  }
  
  const ssize_t ret = read_f(fd, buf, size);
  printf("read %d bytes\n", (int)ret);
  return ret;
}

ssize_t write(int fd, const void *buf, size_t size) {
  printf("write %s\n", (const char*)buf);
  return write_f(fd, buf, size);
}

void init_hook() {
  if(!read_f) {
    read_f = dlsym(RTLD_NEXT, "read");
  }

  if(!write_f) {
    write_f = dlsym(RTLD_NEXT, "write");
  }
}

void func1(void) {
  while(count++ < 20) {
    printf("1. count = %d\n", count);
    swapcontext(&cxt[0], &main_cxt);
    printf("4. count = %d\n", count);
  } 
}

void func2(void) {
  while(count++ < 20) {
    printf("2. count = %d\n", count);
    swapcontext(&cxt[1], &main_cxt);
    printf("5. count = %d\n", count);
  }
}

void func3(void) {
  while(count++ < 20) {
    printf("3. count = %d\n", count);
    swapcontext(&cxt[2], &main_cxt);
    printf("6. count = %d\n", count);
  }
}

int main(void) {
  init_hook();

  int fd = open("a.txt", O_CREAT | O_RDWR);
  if(fd < 0) {
    return -1;
  }

  const char *str = "1234567890";
  write(fd, str, strlen(str));

  lseek(fd, 0, SEEK_SET); //fd共享指针，write写入数据时候，
  //偏移量已经到了数据末尾，要是不reset,就只能读到EOF
  
  char buf[128] = {0};
  read(fd, buf, sizeof(buf));
  printf("buf is %s\n", (char*)buf);
  
#if 0
  char stk_1[2048] = {0};
  char stk_2[2048] = {0};
  char stk_3[2048] = {0};
  
  getcontext(&cxt[0]);
  cxt[0].uc_stack.ss_sp = stk_1;
  cxt[0].uc_stack.ss_size = sizeof(stk_1);
  cxt[0].uc_link = &main_cxt;
  makecontext(&cxt[0], func1, 0); 
 
  getcontext(&cxt[1]);
  cxt[1].uc_stack.ss_sp = stk_2;
  cxt[1].uc_stack.ss_size = sizeof(stk_2);
  cxt[1].uc_link = &main_cxt;
  makecontext(&cxt[1], func2, 0);

  getcontext(&cxt[2]);
  cxt[2].uc_stack.ss_sp = stk_3;
  cxt[2].uc_stack.ss_size = sizeof(stk_3);
  cxt[2].uc_link = &main_cxt;
  makecontext(&cxt[2], func3, 0);

  
  printf("swapcontext\n");

  while(count < 20) {
    swapcontext(&main_cxt, &cxt[count % 3]);
  } 
  
  printf("over\n");
#endif
  
  return 0;
}
