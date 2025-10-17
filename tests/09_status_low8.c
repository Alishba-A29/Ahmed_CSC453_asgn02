// 09_status_low8.c
#include <stdio.h>
#include <stdint.h>
#include "lwp.h"

static int ret_mask(void *p){ return (int)(intptr_t)p; }

int main(void){
  lwp_create(ret_mask, (void*)(intptr_t)0xABCDEF11);  // expect 0x11
  lwp_create(ret_mask, (void*)(intptr_t)-5);          // expect 0xFB (== 251)
  lwp_start();

  int s; tid_t t;
  t = lwp_wait(&s); printf("low8 #1: tid=%lu code=%d (expect 17)\n",
                           (unsigned long)t, LWPTERMSTAT(s));
  t = lwp_wait(&s); printf("low8 #2: tid=%lu code=%d (expect 251)\n",
                           (unsigned long)t, LWPTERMSTAT(s));
  t = lwp_wait(&s); printf("done: %lu (expect 0)\n", (unsigned long)t);
  return 0;
}
