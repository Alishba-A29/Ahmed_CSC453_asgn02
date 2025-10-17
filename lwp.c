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
static thread wait_head = NULL, wait_tail = NULL;
static thread term_head = NULL, term_tail = NULL;
static thread *tidtab = NULL;
static size_t  tidcap = 0;


// Helpers
static void wait_enqueue(thread t){
    t->lib_one = NULL;
    if(!wait_tail) wait_head = wait_tail = t;
    else { wait_tail->lib_one = t; wait_tail = t; }
}
static thread wait_dequeue(void){
    if(!wait_head) return NULL;
    thread t = wait_head;
    wait_head = wait_head->lib_one;
    if(!wait_head) wait_tail = NULL;
    t->lib_one = NULL;
    return t;
}

static void ensure_tidcap(size_t need){
    if (need < tidcap) return;
    size_t newcap = tidcap ? tidcap : 16;
    while (newcap <= need) newcap <<= 1;
    thread *tmp = realloc(tidtab, newcap * sizeof(*tmp));
    if (!tmp) { /* out of memory: bail hard */ _exit(3); }
    // zero-initialize the newly added portion
    for (size_t i = tidcap; i < newcap; ++i) tmp[i] = NULL;
    tidtab = tmp; tidcap = newcap;
}

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
thread tid2thread(tid_t tid){
    if ((size_t)tid < tidcap) return tidtab[tid];
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
    ensure_tidcap(t->tid);
    tidtab[t->tid] = t;

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
    // Top of stack, 16B-align a frame base:
    uintptr_t top = (uintptr_t)t->stack + t->stacksize;
    uintptr_t frame = (top - 16) & ~(uintptr_t)0xFUL;

    *(uintptr_t*)(frame + 0) = 0;
    *(uintptr_t*)(frame + 8) = (uintptr_t)lwp_trampoline;

    // Set registers so that `leave; ret` lands in lwp_trampoline
    t->state.rbp = frame;
    t->state.rsp = frame;
    t->state.rdi = (uintptr_t)f;
    t->state.rsi = (uintptr_t)arg;


    // Add to global list and admit to scheduler
    ensure_scheduler();
    if(cur_sched && cur_sched->admit) cur_sched->admit(t);

    return t->tid;
}

// Exit: terminate the current thread
void lwp_exit(int code){
    if (!current) _exit(code & 0xFF);

    // If the system thread tries to exit, end the process.
    if (current == scheduler_main) {
        _exit(code & 0xFF);
    }

    thread me = current;

    // Mark terminated with 8-bit code
    me->status = MKTERMSTAT(LWP_TERM, code & 0xFF);

    // Remove from ready queue if present
    if (cur_sched && cur_sched->remove) cur_sched->remove(me);

    // If someone is waiting
    thread waiter = wait_dequeue();
    if (waiter) {
        waiter->exited = me;
        if (cur_sched && cur_sched->admit) cur_sched->admit(waiter);
    } else {
        // No waiters yet → enqueue onto terminated FIFO (oldest-first)
        me->lib_two = NULL;
        if (!term_head) term_head = term_tail = me;
        else { term_tail->lib_two = me; term_tail = me; }
    }

    // Return control to main (your bounce-through-main design)
    if (scheduler_main) {
        current = scheduler_main;
        swap_rfiles(&me->state, &scheduler_main->state);
        __builtin_unreachable();
    }

    _exit(LWPTERMSTAT(me->status)); // Shouldn't happen after lwp_start
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

        // Re-queue the worker if it hasn't terminated
        if (!LWPTERMINATED(old->status) && cur_sched && cur_sched->admit) {
            cur_sched->admit(old);
        }

        // Hand control back to main
        if (scheduler_main) {
            current = scheduler_main;
            swap_rfiles(&old->state, &scheduler_main->state);
        }
        return;
    }

    if (!cur_sched || !cur_sched->next) {
        // No scheduler: nothing to do
        return;
    }

    // Ask scheduler for the next runnable LWP
    thread next = cur_sched->next();

    if (!next) {
        // No runnable LWPs remain → terminate the process with main's status.
        int code = LWPTERMSTAT(scheduler_main ? scheduler_main->status : 0);
        _exit(code);  // do not return
    }

    // Switch from main to the next runnable LWP
    thread old = scheduler_main;
    current = next;
    swap_rfiles(&old->state, &next->state);
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

    ensure_tidcap(0);
    tidtab[0] = scheduler_main;

    swap_rfiles(&scheduler_main->state, NULL);

    current = scheduler_main;
}



// Wait: wait for any thread to terminate; 
tid_t lwp_wait(int *status){
    // 1) If there is already a terminated thread, return the oldest (FIFO)
    if (term_head){
        thread t = term_head;
        term_head = t->lib_two;
        if (!term_head) term_tail = NULL;

        if (status) *status = t->status;
        tid_t tid = t->tid;
        if ((size_t)tid < tidcap) tidtab[tid] = NULL; 

        if (t != scheduler_main) {
            if (t->stack && t->stacksize) munmap((void*)t->stack, t->stacksize);
            free(t);
        }
        return tid;
    }

    // 2) No terminated threads. Decide if we can block.
    int q = cur_sched && cur_sched->qlen ? cur_sched->qlen() : 0;
    if (q == 0){
        // No runnable LWPs remain → blocking would be forever → NO_THREAD
        return NO_THREAD;
    }

    // 3) Block the caller: deschedule and enqueue on waiters FIFO
    // NOTE: lwp_wait may be called by ANY thread (main or a worker).
    thread waiter = current;

    // If the caller is presently in the ready queue, remove it.
    if (cur_sched && cur_sched->remove) cur_sched->remove(waiter);

    waiter->exited = NULL;   // will be set by lwp_exit
    wait_enqueue(waiter);

    // Yield control (bounce to main in your design)
    if (scheduler_main && waiter != scheduler_main) {
        current = scheduler_main;
        swap_rfiles(&waiter->state, &scheduler_main->state);
        // resumed here when admitted by lwp_exit()
    } else {
        // If we're already on main (not in ready queue), just schedule others
        lwp_yield();
        // resumed here when a worker admits us back (rare case)
    }

    // 4) We were awakened. 
    thread dead = waiter->exited;
    waiter->exited = NULL;

    // If a racer admitted us before anyone exited
    if (!dead){
        // Try again (now there might be something in term_head)
        return lwp_wait(status);
    }

    if (status) *status = dead->status;
    tid_t tid = dead->tid;
    if ((size_t)tid < tidcap) tidtab[tid] = NULL;

    if (dead != scheduler_main) {
        if (dead->stack && dead->stacksize) 
        munmap((void*)dead->stack, dead->stacksize);
        free(dead);
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
