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


// static thread term_dequeue(void){
//     if(!term_head) return NULL;
//     thread t = term_head;
//     term_head = term_head->exited;
//     if(!term_head) term_tail = NULL;
//     t->exited = NULL;
//     return t;
// }

// All-threads singly-linked list management
static void add_thread_global(thread t){
    t->lib_one = all_threads;
    all_threads = t;
}

// static void remove_thread_global(thread t){
//     thread *pp = &all_threads;
//     while (*pp) {
//         if (*pp == t) {
//             *pp = t->lib_one;  // unlink from list
//             return;
//         }
//         pp = &(*pp)->lib_one;
//     }
// }

// Minimal internal RR scheduler forward (implemented in sched_rr.c)
extern scheduler rr_scheduler(void);

// Ensure the default scheduler is initialized once
static void ensure_scheduler(void){
    if(!cur_sched){
        cur_sched = rr_scheduler();
        if(cur_sched && cur_sched->init) cur_sched->init();
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
void lwp_exit(int code){
    if (!current) _exit(code & 0xFF);

    // If somehow the system thread tried to exit, just end the process.
    if (current == scheduler_main) {
        _exit(code & 0xFF);
    }

    thread me = current;

    // Mark terminated with 8-bit code
    me->status = MKTERMSTAT(LWP_TERM, code & 0xFF);

    // Remove from ready queue if present
    if (cur_sched && cur_sched->remove) cur_sched->remove(me);

    // Enqueue on terminated list (use a dedicated link; here lib_two)
    me->lib_two = NULL;
    if (!term_head) term_head = term_tail = me;
    else { term_tail->lib_two = me; term_tail = me; }

    // Always return control to main after a thread exits.
    if (scheduler_main) {
        current = scheduler_main;
        swap_rfiles(&me->state, &scheduler_main->state);
        __builtin_unreachable();
    }

    // If no main exists (shouldn’t happen after lwp_start), just exit.
    _exit(LWPTERMSTAT(me->status));
}

// Get TID of current thread (or NO_THREAD if none)
tid_t lwp_gettid(void){
  return current ? current->tid : NO_THREAD;
}

// Yield: voluntarily give up the CPU to another thread
void lwp_yield(void){
    ensure_scheduler();  
    if (current && current != scheduler_main) {
        thread old = current;

        // Re-admit the current worker if it's not terminated
        if (!LWPTERMINATED(old->status) && cur_sched && cur_sched->admit) {
            cur_sched->admit(old);
        }

        // Hand control to the system thread (main)
        if (scheduler_main) {
            current = scheduler_main;
            swap_rfiles(&old->state, &scheduler_main->state);
        }
        return;
    }

    // We are in scheduler_main: run exactly one worker if available.
    if (cur_sched && cur_sched->next) {
        thread next = cur_sched->next();
        if (!next) return;           // nothing to run
        if (next == scheduler_main)  // guard (shouldn't happen)
            return;

        thread old = scheduler_main; // from main to worker
        current = next;
        swap_rfiles(&old->state, &next->state);
    }
}

// Start the LWP system by capturing the current thread as scheduler_main
void lwp_start(void){
    if (scheduler_main) return;

    // Ensure there is a scheduler (default RR is fine)
    if (!cur_sched) cur_sched = lwp_get_scheduler();

    // Create the scheduler_main thread
    scheduler_main = (thread)calloc(1, sizeof(*scheduler_main));
    if (!scheduler_main) _exit(3);

    scheduler_main->tid    = 0;
    scheduler_main->status = MKTERMSTAT(LWP_LIVE, 0);

    // Save current register state so we can resume here later
    swap_rfiles(&scheduler_main->state, NULL);

    current = scheduler_main;
}



// Wait: wait for any thread to terminate; 
tid_t lwp_wait(int *status){
    // Wait for a terminated thread, but stop if no workers remain
    for(;;){
        if (term_head) break;
        int q = cur_sched ? cur_sched->qlen() : 0;
        if (q == 0) return NO_THREAD;  // nothing can terminate anymore
        lwp_yield();
    }

    thread t = term_head;
    term_head = t->lib_two;
    if (!term_head) term_tail = NULL;

    if (status) *status = t->status;
    tid_t tid = t->tid;

    // Free resources for real LWPs (do not free scheduler_main here)
    if (t != scheduler_main) {
        if (t->stack && t->stacksize) munmap((void*)t->stack, t->stacksize);
        // If you keep a global list, also unlink t there.
        free(t);
    }
    return tid;
}



// Set the current scheduler, migrating threads as needed
void lwp_set_scheduler(scheduler newsched){
    if (!newsched || newsched == cur_sched) return;

    // 1) Initialize the new scheduler
    if (newsched->init) newsched->init();

    // 2) Drain ALL runnable LWPs from the old scheduler into a temp list
    thread tmp_head = NULL, tmp_tail = NULL;
    if (cur_sched && cur_sched->next) {
        thread t;
        while ( (t = cur_sched->next()) ){
            if (t == scheduler_main) continue;
            t->lib_two = NULL;
            if (!tmp_head) tmp_head = tmp_tail = t;
            else { tmp_tail->lib_two = t; tmp_tail = t; }
        }
    }

    // 3) Shutdown the old scheduler
    if (cur_sched && cur_sched->shutdown) cur_sched->shutdown();

    // 4) Swap
    cur_sched = newsched;

    // 5) Admit drained LWPs into the new scheduler
    for (thread p = tmp_head; p; p = p->lib_two) {
        if (cur_sched->admit) cur_sched->admit(p);
    }
}




// Get the current scheduler, initializing default if needed
scheduler lwp_get_scheduler(void){
  if(!cur_sched) cur_sched = rr_scheduler();
  return cur_sched;
}
