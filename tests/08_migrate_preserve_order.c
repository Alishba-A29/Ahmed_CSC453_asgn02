// 08_migrate_preserve_order.c
#include <stdio.h>
#include "lwp.h"

static int tag(void *p){
  printf("run %ld\n", (long)p);
  lwp_yield();
  printf("exit %ld\n", (long)p);
  return (int)(long)p;
}

int main(void){
  // Enqueue in known order: 1,2,3,4,5
  for(long i=1;i<=5;i++) lwp_create(tag, (void*)i);

  // No-op migration: should preserve ready-queue order
  lwp_set_scheduler(lwp_get_scheduler());

  lwp_start();

  // Collect in FIFO termination order (1..5)
  int s; tid_t t;
  for(int i=1;i<=5;i++){
    t = lwp_wait(&s);
    printf("wait %d => tid=%lu code=%d\n", i, (unsigned long)t, LWPTERMSTAT(s));
  }
  t = lwp_wait(&s); printf("done => %lu (expect 0)\n", (unsigned long)t);
  return 0;
}
