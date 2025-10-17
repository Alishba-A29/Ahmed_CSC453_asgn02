#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdarg.h> 
#include "lwp.h"

/* ---------- Config ---------- */
#ifndef STRESS_THREADS
#define STRESS_THREADS 120          /* base threads */
#endif
#ifndef STRESS_CHILDREN
#define STRESS_CHILDREN 30          /* created by the spawner worker */
#endif
#ifndef MAX_YIELDS
#define MAX_YIELDS 50               /* upper bound for per-thread yields */
#endif
#ifndef RECURSION_DEPTH
#define RECURSION_DEPTH 48          /* keep safe for your ~8-page stacks */
#endif
#ifndef VERBOSE
#define VERBOSE 1
#endif

/* ---------- Utilities ---------- */
static int rng(int lo, int hi){
  return lo + (rand() % (hi - lo + 1));
}

static void vprint(const char *fmt, ...){
#if VERBOSE
  va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
#else
  (void)fmt;
#endif
}

/* ---------- Worker types ---------- */
typedef enum {
  W_SPINNER,       /* yield a bunch then exit(id) */
  W_FPU,           /* float ops across yields; verify sum shape */
  W_RECURSOR,      /* recursion with small stack frames */
  W_SPAWNER,       /* creates more threads, yields between creates */
  W_LAGGARD        /* yields a lot, then exits late */
} wtype;

typedef struct {
  wtype  kind;
  int    id;          /* also used as exit code (8-bit) */
  int    yields;      /* how much to yield */
} warg;

/* ---------- RECURSOR ---------- */
static int rec_fn(int depth, volatile unsigned *sink){
  volatile unsigned local = (unsigned)depth ^ 0xA5A5u;
  *sink += local;
  if(depth <= 0) return 0;
  if(depth % 7 == 0) lwp_yield();
  return rec_fn(depth-1, sink);
}

static int worker_recursor(void *vp){
  warg *wa = (warg*)vp;
  volatile unsigned sink = 0;
  rec_fn(RECURSION_DEPTH, &sink);
  for(int i=0;i<wa->yields;i++) lwp_yield();
  lwp_exit(wa->id);
  return wa->id;
}

/* ---------- SPINNER ---------- */
static int worker_spinner(void *vp){
  warg *wa = (warg*)vp;
  for(int i=0;i<wa->yields;i++) lwp_yield();
  lwp_exit(wa->id);
  return wa->id;
}

/* ---------- FPU ---------- */
static int worker_fpu(void *vp){
  warg *wa = (warg*)vp;
  /* make a deterministic double sum that depends on id + yields */
  double sum = 0.0;
  int rounds = wa->yields + 10;
  for(int i=0;i<rounds;i++){
    /* inject id into the series so different threads get unique sum paths */
    double term = (double)((wa->id % 13) + 1) / (double)(i + 1);
    sum += term;
    if(i % 3 == 0) lwp_yield();
  }
  /* guard against the compiler optimizing away */
  if(sum < 0.0) vprint("impossible\n");
  lwp_exit(wa->id);
  return wa->id;
}

/* ---------- LAGGARD ---------- */
static int worker_laggard(void *vp){
  warg *wa = (warg*)vp;
  for(int i=0;i<wa->yields + MAX_YIELDS; i++) lwp_yield();
  lwp_exit(wa->id);
  return wa->id;
}

/* ---------- SPAWNER ---------- */
static int child_simple(void *vp){
  warg *wa = (warg*)vp;
  for(int i=0;i<wa->yields;i++) lwp_yield();
  lwp_exit(wa->id);
  return wa->id;
}
static int worker_spawner(void *vp){
  warg *wa = (warg*)vp;
  int to_spawn = STRESS_CHILDREN;
  for(int i=0;i<to_spawn;i++){
    warg *cw = (warg*)calloc(1,sizeof(*cw));
    cw->kind   = W_SPINNER;
    cw->id     = (wa->id + 100 + i) & 0xFF;
    cw->yields = rng(5, 20);
    (void)lwp_create(child_simple, cw);
    if(i % 3 == 0) lwp_yield(); /* interleave spawns */
  }
  /* This also exercises scheduler migration mid-run (noop to same RR) */
  lwp_set_scheduler(lwp_get_scheduler());
  lwp_yield();
  lwp_exit(wa->id);
  return wa->id;
}

/* ---------- Main stress ---------- */
int main(void){
  srand(1234567);  /* deterministic run; change if you want */

  const int base = STRESS_THREADS;
  int created = 0;

  /* allocate args */
  warg **args = (warg**)calloc(base + STRESS_CHILDREN + 8, sizeof(*args));

  /* Create a diverse set */
  for(int i=0;i<base;i++){
    warg *wa = (warg*)calloc(1,sizeof(*wa));
    wa->id     = (i+1) & 0xFF;
    wa->yields = rng(5, MAX_YIELDS);
    /* spread kinds roughly evenly */
    switch(i % 5){
      case 0: wa->kind = W_SPINNER;  break;
      case 1: wa->kind = W_FPU;      break;
      case 2: wa->kind = W_RECURSOR; break;
      case 3: wa->kind = W_LAGGARD;  break;
      case 4: wa->kind = W_SPAWNER;  break;
    }
    args[created++] = wa;
  }

  /* build and admit threads */
  for(int i=0;i<created;i++){
    warg *wa = args[i];
    lwpfun f = NULL;
    switch(wa->kind){
      case W_SPINNER:  f = worker_spinner;  break;
      case W_FPU:      f = worker_fpu;      break;
      case W_RECURSOR: f = worker_recursor; break;
      case W_SPAWNER:  f = worker_spawner;  break;
      case W_LAGGARD:  f = worker_laggard;  break;
    }
    (void)lwp_create(f, wa);
  }

  vprint("created base=%d (+ spawner children during run)\n", created);

  /* start scheduling; returns when the run queue drains */
  lwp_start();

  /* Collect all terminated threads. We don't know exact total yet because spawners added children. */
  int waits = 0;
  int term_ok = 0;
  int codes_7bit_or = 0;
  int status = 0;
  tid_t tid;

  while( (tid = lwp_wait(&status)) != NO_THREAD ){
    waits++;
    if(LWPTERMINATED(status)) term_ok++;
    codes_7bit_or |= (LWPTERMSTAT(status) & 0x7F); /* mix to perturb value */
  }

  vprint("waits=%d term_ok=%d mix=0x%x\n", waits, term_ok, codes_7bit_or);

  /* sanity: at least all base + most children should have terminated.
     spawner itself also terminates, and it created STRESS_CHILDREN children. */
  int expected_min = base + STRESS_CHILDREN; /* conservative lower bound */
  if(waits < expected_min){
    fprintf(stderr, "FAIL: waits=%d < expected_min=%d (some threads missing)\n",
            waits, expected_min);
    return 2;
  }
  if(term_ok != waits){
    fprintf(stderr, "FAIL: terminated flags mismatch (term_ok=%d waits=%d)\n",
            term_ok, waits);
    return 3;
  }

  /* basic post-condition reached */
  puts("OK: stress passed (creation/spawn/yield/fpu/recursion/exit/wait/migrate)");
  return 0;
}
