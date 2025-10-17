#include <stdio.h>
#include "lwp.h"

int main(void){
  scheduler s = lwp_get_scheduler();
  if(!s){ puts("scheduler null"); return 1; }
  tid_t t = lwp_gettid();
  printf("lwp_gettid() => %lu (expected 0 in Phase 0)\n", (unsigned long)t);
  puts("OK: symbols present");
  return 0;
}

