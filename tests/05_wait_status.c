#include <stdio.h>
#include "../lwp.h"

static int wa(void *p){ (void)p; lwp_yield(); lwp_exit(7);  return 7; }
static int wb(void *p){ (void)p; lwp_yield(); lwp_yield(); lwp_exit(42); return 42; }

int main(void){
    (void)lwp_create(wa, NULL);
    (void)lwp_create(wb, NULL);

    lwp_start(); // run until the run queue drains

    int s1=0, s2=0;
    tid_t t1 = lwp_wait(&s1);
    tid_t t2 = lwp_wait(&s2);
    tid_t t3 = lwp_wait(NULL);

    printf("wait 1: tid=%lu term=%d code=%u\n",
           (unsigned long)t1, LWPTERMINATED(s1), LWPTERMSTAT(s1));
    printf("wait 2: tid=%lu term=%d code=%u\n",
           (unsigned long)t2, LWPTERMINATED(s2), LWPTERMSTAT(s2));
    printf("wait 3: tid=%lu (expected 0)\n", (unsigned long)t3);

    return 0;
}
