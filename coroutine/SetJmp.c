#include <stdio.h>
#include <setjmp.h>

jmp_buf env;

void func(int num) {
  printf("The func num is : %d\n", num);
  longjmp(env, ++num);
}

int main(int argc, char **argv) {

  int ret = setjmp(env);
  while(ret <= 5) {
    printf("The ret is : %d\n", ret);
    func(ret);
  }
  
  return 0;
}



