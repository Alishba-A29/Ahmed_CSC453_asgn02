#include <stdio.h>
#include <stdint.h>
#include "lwp.h"

/* fast exits with code in low 8 bits */
static int fast(void *p){
  int code = (int)(intptr_t)p;
  lwp_yield();                    // give scheduler a chance to interleave
  return code;
}

/* slow burns time so that a waiter will block */
static int slow(void *p){
  (void)p;
  for(int i=0;i<2000;i++) lwp_yield();
  return 99;
}

/* controller thread calls lwp_wait() while others are still running */
static int controller(void *p){
  (void)p;
  tid_t s = lwp_create(slow, NULL);
  tid_t f1 = lwp_create(fast, (void*)11);
  tid_t f2 = lwp_create(fast, (void*)22);
  (void)s; (void)f1; (void)f2;

  int st; tid_t t;

  /* This wait will block until the first fast thread exits */
  t = lwp_wait(&st);
  printf("wait #1: tid=%lu term=%d code=%d (expect code=11 or 22)\n",
         (unsigned long)t, LWPTERMINATED(st), LWPTERMSTAT(st));

  /* This one gets the second fast thread */
  t = lwp_wait(&st);
  printf("wait #2: tid=%lu term=%d code=%d (expect code=11 or 22)\n",
         (unsigned long)t, LWPTERMINATED(st), LWPTERMSTAT(st));

  /* This blocks until the slow thread finally exits */
  t = lwp_wait(&st);
  printf("wait #3: tid=%lu term=%d code=%d (expect code=99)\n",
         (unsigned long)t, LWPTERMINATED(st), LWPTERMSTAT(st));

  /* Now nothing left to reap; should return 0 */
  t = lwp_wait(&st);
  printf("wait #4: tid=%lu (expect 0)\n", (unsigned long)t);
  return 0;
}

int main(void){
  lwp_create(controller, NULL);
  lwp_start();                   // runs until controller returns
  int st; lwp_wait(&st);         // reap controller
  return 0;
}
