#ifndef LWPH
#define LWPH
#include <sys/types.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif


#if defined(__x86_64)
#include <fp.h>
typedef struct __attribute__ ((aligned(16))) __attribute__ ((packed))
registers {
  unsigned long rax;            // The sixteen architecturally-visible regs.
  unsigned long rbx;
  unsigned long rcx;
  unsigned long rdx;
  unsigned long rsi;
  unsigned long rdi;
  unsigned long rbp;
  unsigned long rsp;
  unsigned long r8;
  unsigned long r9;
  unsigned long r10;
  unsigned long r11;
  unsigned long r12;
  unsigned long r13;
  unsigned long r14;
  unsigned long r15;
  struct fxsave fxsave __attribute__((aligned(16)));
} rfile;
#else
  #error "This only works on x86_64 for now"
#endif
/* Compile-time guard: ensure fxsave member sits at a 16-byte offset */
#include <stddef.h>
typedef char _fxsave_align_check[(offsetof(rfile, fxsave) % 16) == 0 ? 1 : -1];

typedef unsigned long tid_t;
#define NO_THREAD 0             // An always invalid thread id

typedef struct threadinfo_st *thread;
typedef struct threadinfo_st {
  tid_t         tid;            // lwp id
  unsigned long *stack;         // Base stack
  size_t        stacksize;      // Size stack
  rfile         state;          // saved registers
  unsigned int  status;         // status
  thread        lib_one;
  thread        lib_two;
  thread        sched_one;
  thread        sched_two;
  thread        exited;         // One for lwp_wait()
} context;

typedef int (*lwpfun)(void *);  // type for lwp function

// Tuple that describes a scheduler
typedef struct scheduler {
  void   (*init)(void);            // init structures
  void   (*shutdown)(void);        // tear down structures
  void   (*admit)(thread new);     // add a thread to the pool
  void   (*remove)(thread victim); // remove a thread from the pool
  thread (*next)(void);            // select a thread to schedule
  int    (*qlen)(void);            // number of ready threads
} *scheduler;

// lwp functions
extern tid_t lwp_create(lwpfun,void *);
extern void  lwp_exit(int status);
extern tid_t lwp_gettid(void);
extern void  lwp_yield(void);
extern void  lwp_start(void);
extern tid_t lwp_wait(int *);
extern void  lwp_set_scheduler(scheduler fun);
extern scheduler lwp_get_scheduler(void);
extern thread tid2thread(tid_t tid);

// for lwp_wait 
#define TERMOFFSET        8
#define MKTERMSTAT(a,b)   ( (a)<<TERMOFFSET | ((b) & ((1<<TERMOFFSET)-1)) )
#define LWP_TERM          1
#define LWP_LIVE          0
#define LWPTERMINATED(s)  ( (((s)>>TERMOFFSET)&LWP_TERM) == LWP_TERM )
#define LWPTERMSTAT(s)    ( (s) & ((1<<TERMOFFSET)-1) )

// prototypes for asm functions
void swap_rfiles(rfile *old, rfile *new);

#endif
