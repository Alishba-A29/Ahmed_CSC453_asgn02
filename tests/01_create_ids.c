#include <stdio.h>
#include "lwp.h"

static int dummy(void* p){ (void)p; return 0; }

int main(void){
  tid_t a = lwp_create(dummy,NULL);
  tid_t b = lwp_create(dummy,NULL);
  tid_t c = lwp_create(dummy,NULL);
  if(a==0 || b==0 || c==0){ puts("create failed"); return 1; }
  printf("created TIDs: %lu %lu %lu\n",
         (unsigned long)a,(unsigned long)b,(unsigned long)c);
  puts("OK: Phase 0 create() stubs work");
  return 0;
}
