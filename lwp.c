// lwp.c
#include "lwp.h"
#include "fp.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

// Global state
static scheduler cur_sched = NULL;   // current scheduler
static thread    current   = NULL;
static tid_t     next_tid  = 1;
static thread scheduler_main = NULL;   // never admitted to RR queue
static thread term_head = NULL, term_tail = NULL;
static thread all_threads = NULL;

// enqueue onto terminated FIFO (oldest-first)
// static void term_enqueue(thread t){
//     t->exited = NULL;
//     if(!term_head) term_head = term_tail = t;
//     else { term_tail->exited = t; term_tail = t; }
// }


static thread term_dequeue(void){
    if(!term_head) return NULL;
    thread t = term_head;
    term_head = term_head->exited;
    if(!term_head) term_tail = NULL;
    t->exited = NULL;
    return t;
}

// All-threads singly-linked list management
static void add_thread_global(thread t){
    t->lib_one = all_threads;
    all_threads = t;
}

static void remove_thread_global(thread t){
    thread *pp = &all_threads;
    while (*pp) {
        if (*pp == t) {
            *pp = t->lib_one;  // unlink from list
            return;
        }
        pp = &(*pp)->lib_one;
    }
}

// Minimal internal RR scheduler forward (implemented in sched_rr.c)
extern scheduler rr_scheduler(void);

// Ensure the default scheduler is initialized once
static void ensure_scheduler(void){
    if(!cur_sched){
        cur_sched = rr_scheduler();        // factory from sched_rr.c
        if(cur_sched && cur_sched->init)
            cur_sched->init();
    }
}

// Find thread by TID
thread tid2thread(tid_t tid) {
    for (thread p = all_threads; p; p = p->lib_one) {
        if (p->tid == tid) return p;
    }
    // MUST return NULL for a bad tid
    return NULL;
}

// Trampoline function for new LWPs
static void lwp_trampoline(lwpfun f, void *arg){
    int rc = f ? f(arg) : 0;
    lwp_exit(rc);
}


// Create: allocate and initialize a new thread
tid_t lwp_create(lwpfun f, void *arg){
    thread t = (thread)calloc(1, sizeof(*t));
    if(!t) return NO_THREAD;

    // Bookkeeping
    t->tid    = next_tid++;
    t->status = MKTERMSTAT(LWP_LIVE, 0);

    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    size_t stksz  = 1UL<<20;                 // 1 MiB default
    stksz = (stksz + pagesz - 1) & ~(pagesz - 1);  // round up to page size

    // stack allocation
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_STACK
    flags |= MAP_STACK;
#endif
    void *stk = mmap(NULL, stksz, PROT_READ|PROT_WRITE, flags, -1, 0);
    if(stk == MAP_FAILED){
        free(t);
        return NO_THREAD;
    }

    t->stack     = (unsigned long*)stk;
    t->stacksize = stksz;

    // pass (f, arg) in %rdi/%rsi to the trampoline
    t->state.rdi = (unsigned long)f;
    t->state.rsi = (unsigned long)arg;

    // zero GPRs we don't care about initially
    t->state.rax = t->state.rbx = t->state.rcx = t->state.rdx = 0;
    t->state.r8  = t->state.r9  = t->state.r10 = t->state.r11 = 0;
    t->state.r12 = t->state.r13 = t->state.r14 = t->state.r15 = 0;

    // FPU init as provided by fp.h
    t->state.fxsave = FPU_INIT;

    // After 'ret' into lwp_trampoline, ABI wants %rsp ≡ 8 (mod 16).
    // So the saved %rsp itself must be 16B aligned, and the slot at [%rsp]
    // must contain the non-zero return address (lwp_trampoline).
    uintptr_t top = (uintptr_t)t->stack + t->stacksize;      // one past top
    uintptr_t rsp = (top - 8) & ~(uintptr_t)0xFUL;           // 16B-aligned

    *(unsigned long*)rsp = (unsigned long)lwp_trampoline;    // retaddr
    t->state.rsp = rsp;          // saved %rsp (16B)
    t->state.rbp = rsp - 8;     // plausible, aligned %rbp

    // Add to global list and admit to scheduler
    add_thread_global(t);
    ensure_scheduler();
    if(cur_sched && cur_sched->admit) cur_sched->admit(t);

    return t->tid;
}

// Exit: terminate the current thread
void lwp_exit(int code) {
    thread me = current;

    // 1) mark terminated with low 8 bits
    me->status = MKTERMSTAT(LWP_TERM, code & 0xFF);

    // 2) remove from scheduler if present
    if (cur_sched && cur_sched->remove) {
        cur_sched->remove(me);
    }

    // 3) enqueue on terminated list (FIFO)
    me->exited = NULL;
    if (!term_head) {
        term_head = term_tail = me;
    } else {
        term_tail->exited = me;
        term_tail = me;
    }

    // 4) switch back to system thread so lwp_start() returns
    current = NULL;
    swap_rfiles(&me->state, &scheduler_main->state);

    // never returns here
}





// Get TID of current thread (or NO_THREAD if none)
tid_t lwp_gettid(void){
  return current ? current->tid : NO_THREAD;
}

// Yield: voluntarily give up the CPU to another thread
void lwp_yield(void){
  ensure_scheduler();
  thread old = current;

  // 1) Ask scheduler for next thread to run
  thread next = NULL;
  if (cur_sched && cur_sched->next)
    next = cur_sched->next();

  /* 2) Re-admit 'old' ONLY if:
        - it's not the system thread,
        - it's still live (not exiting),
        - and the scheduler did not choose 'old' to run again right now. */
  if (old && old != scheduler_main 
            && !LWPTERMINATED(old->status) 
            && next != old) {
    if (cur_sched->admit) cur_sched->admit(old);
  }

  // 3) If no next thread, run the system thread
  if (!next) {
    current = scheduler_main;
    swap_rfiles(&old->state, &scheduler_main->state);
    return;
  }

  // 4) If next is old, no context switch needed
  if (next == old) return;

  // 5) Context switch to 'next'
  current = next;
  swap_rfiles(&old->state, &next->state);
}



// Start the LWP system by capturing the current thread as scheduler_main
void lwp_start(void){
    if(scheduler_main) return;
    ensure_scheduler();

    scheduler_main = (thread)calloc(1, sizeof(*scheduler_main));
    if(!scheduler_main) return;
    scheduler_main->tid    = next_tid++;
    scheduler_main->status = MKTERMSTAT(LWP_LIVE, 0);
    add_thread_global(scheduler_main);

    //
    swap_rfiles(&scheduler_main->state, NULL);

    current = scheduler_main;
    lwp_yield();
}

// Wait: wait for any thread to terminate; 
// return its TID and status
// If no threads exist or are runnable, 
// return NO_THREAD immediately.
tid_t lwp_wait(int *status){
    while(!term_head){
        // nothing left to wait for
        if(!cur_sched || cur_sched->qlen()==0) return NO_THREAD;
        lwp_yield();
    }

    // Pop oldest terminated
    thread t = term_dequeue();
    tid_t tid = t->tid;

    // Reap any other terminated threads while we're here
    if(status) *status = t->status;

    // Cleanup for real LWPs (not the captured scheduler_main)
    if(t != scheduler_main){
        if(t->stack && t->stacksize) 
            munmap((void*)t->stack, t->stacksize);
        remove_thread_global(t);
        free(t);
    }
    return tid;
}

// Set the current scheduler, migrating threads as needed
void lwp_set_scheduler(scheduler newsched) {
  if (!newsched) newsched = rr_scheduler();
  if (newsched == cur_sched) return;

  // 1) Init new scheduler
  if (newsched->init) newsched->init();

  //  2) Migrate all live threads (except current)
  if (cur_sched) {
    for (thread p = all_threads; p; p = p->lib_one) {
      if (p == current) continue;
      if (LWPTERMINATED(p->status)) continue;
      if (cur_sched->remove) cur_sched->remove(p);
      if (newsched->admit)  newsched->admit(p);
    }

    // 3) Shutdown old scheduler
    if (cur_sched->shutdown) cur_sched->shutdown();
  }

  // 4) Switch
  cur_sched = newsched;
}



// Get the current scheduler, initializing default if needed
scheduler lwp_get_scheduler(void){
  if(!cur_sched) cur_sched = rr_scheduler();
  return cur_sched;
}
