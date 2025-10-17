// 11_tid_lookup.c
#include <stdio.h>
#include "lwp.h"

static int echo(void *unused){
  (void)unused;
  tid_t me = lwp_gettid();
  printf("thread sees tid=%lu\n", (unsigned long)me);

  // tid2thread should resolve a live TID; bogus should be NULL
  void *hit = tid2thread(me);
  void *miss = tid2thread(999999);
  printf("tid2thread(me)=%p tid2thread(999999)=%p\n", hit, miss);
  return 7;
}

int main(void){
  lwp_create(echo, NULL);
  lwp_start();

  int s; tid_t t = lwp_wait(&s);
  printf("main waited tid=%lu code=%d\n", (unsigned long)t, LWPTERMSTAT(s));
  return 0;
}
