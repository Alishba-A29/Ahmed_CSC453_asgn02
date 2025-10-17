#include "lwp.h"
#include <stdlib.h>

typedef struct node { thread t; struct node *next; } node;
static node *head = NULL, *tail = NULL;

static void rr_init(void){ head = tail = NULL; }

static void rr_shutdown(void){
  while (head) {
     node *n = head; 
     head = head->next;
      free(n); }
  tail = NULL;
}

static void rr_admit(thread t){
  node *n = (node*)malloc(sizeof(*n));
  if (!n) return;
  n->t = t; n->next = NULL;
  if (!tail) head = tail = n;
  else { tail->next = n; tail = n; }
  t->lib_two = (void*)1;
}

static void rr_remove(thread t){
  node *prev = NULL, *cur = head;
  while (cur) {
    if (cur->t == t) {
      if (prev) prev->next = cur->next; 
      else head = cur->next;
      if (cur == tail) tail = prev;
      free(cur);
      t->lib_two = (void*)0; 
      return;
    }
    prev = cur; cur = cur->next;
  }
}

static thread rr_next(void){
  if (!head) return NULL;
  node *n = head; head = head->next;
  if (!head) tail = NULL;
  thread t = n->t; free(n);
  t->lib_two = (void*)2;  
  return t;
}

static int rr_qlen(void){
  int k = 0; for (node *p=head; p; p=p->next) k++; return k;
}

static struct scheduler RR = {
  .init=rr_init, .shutdown=rr_shutdown, .admit=rr_admit,
  .remove=rr_remove, .next=rr_next, .qlen=rr_qlen
};

scheduler rr_scheduler(void){ return &RR; }