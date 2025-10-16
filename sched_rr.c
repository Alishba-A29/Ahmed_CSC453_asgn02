// sched_rr.c
#include "lwp.h"
#include <stdlib.h>

// Simple linked-list queue for RR scheduling
typedef struct node { thread t; struct node *next; } node;
static node *head = NULL, *tail = NULL;
static int qcount = 0;

static void rr_init(void){
  head = tail = NULL;
  qcount = 0;
}

static void rr_shutdown(void){
  while (head) {
    node *n = head;
    head = head->next;
    free(n);
  }
  tail = NULL;
  qcount = 0;
}

static void rr_admit(thread t){
  node *n = (node*)malloc(sizeof *n);
  n->t = t; n->next = NULL;
  if (!tail) { head = tail = n; }
  else { tail->next = n; tail = n; }
  qcount++;
}

static void rr_remove(thread t){
  node *prev = NULL, *cur = head;
  while (cur) {
    if (cur->t == t) {
      if (prev) prev->next = cur->next;
      else      head = cur->next;
      if (cur == tail) tail = prev;
      free(cur);
      qcount--;
      return;
    }
    prev = cur;
    cur  = cur->next;
  }
  // not found: do nothing
}

static thread rr_next(void){
  if (!head) return NULL;
  node *n = head;
  head = n->next;
  if (!head) tail = NULL;
  thread t = n->t;
  free(n);
  qcount--;
  return t;
}

static int rr_qlen(void){
  return qcount;
}

static struct scheduler RR = {
  .init=rr_init, .shutdown=rr_shutdown, .admit=rr_admit,
  .remove=rr_remove, .next=rr_next, .qlen=rr_qlen
};
scheduler rr_scheduler(void){ return &RR; }