// test_min.c
#include "lwp.h"
#include <stdio.h>

static int worker(void *p){
  int id = (int)(long)p;
  for(int i=0;i<3;i++){
    printf("T%d: i=%d\n", id, i);
    lwp_yield();
  }
  return id*10;
}

int main(void){
  lwp_create(worker,(void*)1);
  lwp_create(worker,(void*)2);
  lwp_start();

  int s1,s2;
  tid_t t;
  t = lwp_wait(&s1); printf("wait got tid=%lu status=%d\n", (unsigned long)t, LWPTERMSTAT(s1));
  t = lwp_wait(&s2); printf("wait got tid=%lu status=%d\n", (unsigned long)t, LWPTERMSTAT(s2));
  return 0;
}
