// 12_fpu_context.c
#include <stdio.h>
#include <math.h>
#include "lwp.h"

// Two threads accumulate different floating sums while yielding.
// If FPU context isn't saved/restored, their sums corrupt.

static volatile double a=0.0, b=0.0;

static int fa(void *p){
  (void)p;
  for(int i=1;i<=20000;i++){
    a += 1.0/(double)i;   // harmonic-ish
    if((i & 0x3F)==0) lwp_yield();
  }
  return 1;
}

static int fb(void *p){
  (void)p;
  for(int i=1;i<=20000;i++){
    b += sin((double)i)*1e-6;
    if((i & 0x7F)==0) lwp_yield();
  }
  return 2;
}

int main(void){
  lwp_create(fa,NULL);
  lwp_create(fb,NULL);
  lwp_start();

  int s; lwp_wait(&s); lwp_wait(&s);
  printf("FPU sums: a=%.9f b=%.9f\n", a, b);

  // crude sanity: both should be finite and within expected ballpark
  if(!isfinite(a) || !isfinite(b)) puts("FPU FAIL (non-finite)");
  else if(a < 9.0 || a > 11.0)      puts("FPU WARN (a out-of-range)");
  else                               puts("FPU OK");
  return 0;
}
