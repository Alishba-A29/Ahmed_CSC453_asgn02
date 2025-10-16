// sched_rr.c
#include "lwp.h"
#include <stdlib.h>

// A simple round-robin scheduler implementation
static thread head = NULL, tail = NULL;
static int qcount = 0;

static void rr_init(void){
  head = tail = NULL;
  qcount = 0;
}

static void rr_shutdown(void){
  // nothing to free; threads live outside the scheduler
  head = tail = NULL;
  qcount = 0;
}

static void rr_admit(thread t){
  // Append to tail of queue
  t->sched_one = NULL;
  t->sched_two = NULL;
  if(!tail){ head = tail = t; }
  else { tail->sched_one = t; tail = t; }
  qcount++;
}

static void rr_remove(thread victim){
  thread prev = NULL, cur = head;
  while(cur){
    if(cur == victim){
      if(prev) prev->sched_one = cur->sched_one; else head = cur->sched_one;
      if(cur == tail) tail = prev;
      // do not touch cur->sched_* so caller can inspect if desired
      qcount--;
      return;
    }
    prev = cur;
    cur = cur->sched_one;
  }
  // not found; no-op
}

static thread rr_next(void){
  if(!head) return NULL;
  thread t = head;
  head = t->sched_one;
  if(!head) tail = NULL;
  // Clear link pointers for cleanliness
  qcount--;
  return t;
}

static int rr_qlen(void){ return qcount; }

static struct scheduler RR = {
  .init=rr_init, .shutdown=rr_shutdown, .admit=rr_admit,
  .remove=rr_remove, .next=rr_next, .qlen=rr_qlen
};

scheduler rr_scheduler(void){ 
  return &RR; 
}
