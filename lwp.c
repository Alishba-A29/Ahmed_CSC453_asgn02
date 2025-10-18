#include "lwp.h"
#include "fp.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

// Global state
typedef struct glnode { thread t; struct glnode *next; } glnode;
static scheduler cur_sched = NULL;   // current scheduler
static thread    current   = NULL;
static tid_t     next_tid  = 1;
static thread scheduler_main = NULL;
static thread term_head = NULL, term_tail = NULL;
static glnode *ghead = NULL;
static struct fxsave FPU_INIT_CONST;
static int FPU_INIT_DONE = 0;

// Initialize FPU state constant
static void init_fpu_const(void){
    if (FPU_INIT_DONE) return;
    struct fxsave tmp = FPU_INIT;
    memcpy(&FPU_INIT_CONST, &tmp, sizeof tmp);
    FPU_INIT_DONE = 1;
}

// Notification rotation state
static int notify_need_live = 0;
static int notify_seen_cnt  = 0;

// Seen TID tracking
#define SEEN_MAX 1024
static tid_t notify_seen[SEEN_MAX];
static int   notify_seen_len = 0;

// Reset notification rotation tracking
static void notify_reset_counts(int live){
    notify_need_live = live;
    notify_seen_cnt  = 0;
    notify_seen_len  = 0;
}

// Mark a TID as seen for notification rotation
static int notify_mark_seen(tid_t tid){
    // Check if already seen
    for(int i=0;i<notify_seen_len;i++){
        if(notify_seen[i] == tid) return 0; // already counted
    }
    if(notify_seen_len < SEEN_MAX){
        notify_seen[notify_seen_len++] = tid;
        notify_seen_cnt++;
        return 1;
    }
    return 0; // set full; behave as already seen
}


// enqueue onto terminated FIFO (oldest-first)
static void term_enqueue(thread t){
    t->exited = NULL;
    if(!term_head) term_head = term_tail = t;
    else { term_tail->exited = t; term_tail = t; }
}

// dequeue from terminated FIFO (oldest-first)
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
    glnode *n = (glnode*)malloc(sizeof(*n));
    if(!n) return; 
    n->t = t; 
    n->next = ghead; 
    ghead = n;
}

// Remove thread from global list
static void remove_thread_global(thread t){
    glnode **pp = &ghead;
    while (*pp) {
        if ((*pp)->t == t) {
            glnode *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
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
thread tid2thread(tid_t tid){
    for (glnode *p = ghead; p; p = p->next){
        if (p->t->tid == tid) return p->t;
    }
    return NULL; // MUST return NULL for a bad tid
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
    size_t stksz  = 1UL<<20;                      // 1 MiB default
    stksz = (stksz + pagesz - 1) & ~(pagesz - 1); // page aligned

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

    // Pass (f,arg) to trampoline per SysV AMD64 ABI
    t->state.rdi = (unsigned long)f;
    t->state.rsi = (unsigned long)arg;

    // Zero other GPRs
    t->state.rax = t->state.rbx = t->state.rcx = t->state.rdx = 0;
    t->state.r8  = t->state.r9  = t->state.r10 = t->state.r11 = 0;
    t->state.r12 = t->state.r13 = t->state.r14 = t->state.r15 = 0;
    t->state.rbp = 0;

    // FPU init (safe memcpy to avoid alignment faults)
    init_fpu_const();
    memcpy(&t->state.fxsave, &FPU_INIT_CONST, sizeof t->state.fxsave);

    // Build boot frame for magic64.S (leave; ret)
    uintptr_t top    = (uintptr_t)t->stack + t->stacksize;
    /* Pick a 16B-aligned base well inside the mapping, then add +8 so:
    - after 'leave':  rsp = frame+8  => %16 == 0
    - after 'ret'  :  rsp = frame+16 => %16 == 8  (ABI on function entry) */
    uintptr_t frame  = ((top - 24) & ~(uintptr_t)0xFUL) + 8;

    *(unsigned long *)(frame + 0) = 0UL;
    *(unsigned long *)(frame + 8) = (unsigned long)(uintptr_t)lwp_trampoline;

    t->state.rbp = frame;   // 'leave' uses this
    t->state.rsp = frame;   // 'ret' pops trampoline


    // Register thread and admit to scheduler
    add_thread_global(t);
    ensure_scheduler();
    if(cur_sched && cur_sched->admit) cur_sched->admit(t);

    return t->tid;
}

// Exit: terminate the current thread
void lwp_exit(int code){
    thread me = current;
    if (!me) return;

    me->status = MKTERMSTAT(LWP_TERM, code & 0xFF);

    if (cur_sched && cur_sched->remove) cur_sched->remove(me);
    if (me != scheduler_main) term_enqueue(me);

    // Context switch to another thread
    int live = 0;
    for (glnode *p = ghead; p; p = p->next){
        thread t = p->t;
        if (t && t != scheduler_main && !LWPTERMINATED(t->status)) live++;
    }
    notify_reset_counts(live);

    // Find next thread to run
    thread next = (cur_sched && cur_sched->next) ? cur_sched->next() : NULL;

    if(next == me){
        next = (cur_sched && cur_sched->next) 
            ? cur_sched->next() : NULL;
        if(next == me) next = NULL;
    }

    if (next){
        current = next;
        swap_rfiles(&me->state, &next->state);  /* does not return */
        return;
    }

    if (scheduler_main){
        current = scheduler_main;
        swap_rfiles(&me->state, &scheduler_main->state); /* does not return */
    }
}

// Get TID of current thread (or NO_THREAD if none)
tid_t lwp_gettid(void){
  return current ? current->tid : NO_THREAD;
}

// Yield: voluntarily give up the CPU to another thread
void lwp_yield(void){
    ensure_scheduler();

    // Ensure main thread exists
    if(!scheduler_main){
        scheduler_main = (thread)calloc(1, sizeof(*scheduler_main));
        if(!scheduler_main) return;
        scheduler_main->tid    = next_tid++;
        scheduler_main->status = MKTERMSTAT(LWP_LIVE, 0);
        add_thread_global(scheduler_main);
    }

    // Current thread (or main if none)
    thread old = current ? current : scheduler_main;

    // Notification rotation handling
    if (notify_need_live > 0 &&
        old != scheduler_main &&
        !LWPTERMINATED(old->status))
    {
        if (notify_mark_seen(old->tid)) {
            if (notify_seen_cnt >= notify_need_live) {
                if (cur_sched && cur_sched->admit)    // keep old in RR
                    cur_sched->admit(old);
                notify_reset_counts(0);               // consume rotation
                current = scheduler_main;             // wake main exactly once
                swap_rfiles(&old->state, &scheduler_main->state);
                return;
            }
        }
    }

    thread next = (cur_sched && cur_sched->next) ? cur_sched->next() : NULL;
    if(!next){
        if(old == scheduler_main) return;
        if(!LWPTERMINATED(old->status)) return;
        current = scheduler_main;
        swap_rfiles(&old->state, &scheduler_main->state);
        return;
    }
    if(next == old){
        return;
    }
    if(old != scheduler_main && !LWPTERMINATED(old->status) 
        && next != old
        && cur_sched->admit){
        cur_sched->admit(old);
    }
    current = next;
    swap_rfiles(&old->state, &current->state);
}

// Start: begin scheduling threads
void lwp_start(void){
    if(scheduler_main) return;
    ensure_scheduler();

    scheduler_main = (thread)calloc(1, sizeof(*scheduler_main));
    if(!scheduler_main) return;
    scheduler_main->tid    = next_tid++;
    scheduler_main->status = MKTERMSTAT(LWP_LIVE, 0);
    add_thread_global(scheduler_main);

    current = scheduler_main;
    lwp_yield();
}

// Wait: wait for any thread to terminate; 
tid_t lwp_wait(int *status){
    ensure_scheduler();

    if(!scheduler_main){
        scheduler_main = (thread)calloc(1, sizeof(*scheduler_main));
        if(!scheduler_main) return NO_THREAD;
        scheduler_main->tid    = next_tid++;
        scheduler_main->status = MKTERMSTAT(LWP_LIVE, 0);
        add_thread_global(scheduler_main);
        current = scheduler_main;
    }

    // Run until someone finishes or no live LWPs remain
    while(!term_head){
        thread before = current;
        lwp_yield();

        if(!term_head && current == before){
            // No context switch happened; check if any live LWPs exist
            int any_live = 0;
            for (glnode *p = ghead; p; p = p->next){
                thread t = p->t;
                if (t && t != scheduler_main && !LWPTERMINATED(t->status)){
                    any_live = 1; break;
                }
            }
            if(!any_live) return NO_THREAD;
        }
    }

    // A thread finished:
    thread t = term_dequeue();
    tid_t tid = t->tid;
    if(status) *status = t->status;

    if(t != scheduler_main){
        if(t->stack && t->stacksize) munmap((void*)t->stack, t->stacksize);
        remove_thread_global(t);
        free(t);
    }
    return tid;
}

// Set the current scheduler, migrating threads as needed
void lwp_set_scheduler(scheduler newsched){
  ensure_scheduler();
  if(!newsched) newsched = rr_scheduler();
  if(newsched == cur_sched) return;

  if(newsched->init) newsched->init();

  scheduler old = cur_sched;

  if(old){

    // Migrate all threads except scheduler_main and current
    for (glnode *n = ghead; n; n = n->next) {
      thread t = n->t;
      if (!t) continue;
      if (t == scheduler_main) continue;
      if (t == current)        continue;
      if (LWPTERMINATED(t->status)) continue;

      if (old->remove) old->remove(t);
      if (newsched->admit) newsched->admit(t);
    }
    if (old->shutdown) old->shutdown();
  }

  cur_sched = newsched;

  // If no current thread, yield to scheduler_main
  if (!current || current == scheduler_main) {
    lwp_yield();
  }
}

// Get the current scheduler, initializing default if needed
scheduler lwp_get_scheduler(void){
  if(!cur_sched) cur_sched = rr_scheduler();
  return cur_sched;
}