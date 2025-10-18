#include "lwp.h"
#include <stdlib.h>

typedef struct node { thread t; struct node *next; } node;
static node *head = NULL, *tail = NULL;

// Remove a thread from the RR queue
static void rr_remove(thread t);

static void rr_init(void){
  head = tail = NULL;
}

// Tear down the RR scheduler
static void rr_shutdown(void){
  while (head) {
    node *n = head;
    head = head->next;
    free(n);
  }
  tail = NULL;
}

// Remove a thread from the RR queue
static void rr_remove(thread t){
  if (!t || !head) return;

  node **pp = &head;
  while (*pp) {
    if ((*pp)->t == t) {
      node *dead = *pp;
      *pp = dead->next;
      if (dead == tail) {
        tail = NULL;
        for (node *p = head; p; p = p->next) tail = p;
      }

      free(dead);
      return;
    }
    pp = &(*pp)->next;
  }
}

// Admit a thread to the RR queue
static void rr_admit(thread t){
  if (!t) return;

  rr_remove(t);

  node *n = (node*)malloc(sizeof(*n));
  if (!n) return;
  n->t = t;
  n->next = NULL;

  if (!tail) {
    head = tail = n;
  } else {
    tail->next = n;
    tail = n;
  }
}

// Select the next thread from the RR queue
static thread rr_next(void){
  if (!head) return NULL;
  node *n = head;
  head = head->next;
  if (!head) tail = NULL;
  thread t = n->t;
  free(n);
  return t;
}

// Get the length of the RR queue
static int rr_qlen(void){
  int k = 0;
  for (node *p = head; p; p = p->next) k++;
  return k;
}

// The RR scheduler instance
static struct scheduler RR = {
  .init     = rr_init,
  .shutdown = rr_shutdown,
  .admit    = rr_admit,
  .remove   = rr_remove,
  .next     = rr_next,
  .qlen     = rr_qlen
};

scheduler rr_scheduler(void){ return &RR; }
