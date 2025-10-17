#include <stdio.h>
#include <stdint.h>
#include "../lwp.h"

static int sample(void *p){ (void)p; return 42; }

int main(void){
    tid_t t = lwp_create(sample, (void*)0xdeadbeef);
    thread th = tid2thread(t);
    if(!th){ puts("tid2thread failed"); return 1; }

    // ABI args
    if(th->state.rdi != (unsigned long)sample){ puts("rdi not set"); return 1; }
    if(th->state.rsi != (unsigned long)0xdeadbeef){ puts("rsi not set"); return 1; }

    // saved rsp/rbp relation for leave; ret
    uintptr_t rsp = (uintptr_t)th->state.rsp;
    uintptr_t rbp = (uintptr_t)th->state.rbp;

    if((rsp & 0xF) != 0){ puts("saved rsp not 16B aligned"); return 1; }
    if(rbp != rsp){ puts("rbp != rsp (expected equal for fake frame)"); return 1; }

    unsigned long dummy = *(unsigned long*)rsp;         // old rbp (dummy) — allowed to be 0
    (void)dummy;
    unsigned long retaddr = *(unsigned long*)(rsp + 8); // return address slot for 'ret'
    if(retaddr == 0){ puts("retaddr slot (rsp+8) is zero"); return 1; }

    puts("OK: bootable context prepared (correct frame layout for leave; ret)");
    return 0;
}
