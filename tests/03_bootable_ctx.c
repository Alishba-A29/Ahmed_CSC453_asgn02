#include <stdio.h>
#include <stdint.h>
#include "../lwp.h"

static int sample(void *p){ (void)p; return 42; }

int main(void){
    tid_t t = lwp_create(sample, (void*)0xdeadbeef);
    thread th = tid2thread(t);
    if(!th){ puts("tid2thread failed"); return 1; }

    if(th->state.rdi != (unsigned long)sample){ puts("rdi not set"); return 1; }
    if(th->state.rsi != (unsigned long)0xdeadbeef){ puts("rsi not set"); return 1; }

    uintptr_t rsp = (uintptr_t)th->state.rsp;
    if((rsp & 0xF) != 0){ puts("saved rsp not 16B aligned"); return 1; }

    unsigned long retaddr = *(unsigned long*)rsp;
    if(retaddr == 0){ puts("retaddr slot is zero"); return 1; }

    puts("OK: bootable context prepared (opaque)");
    return 0;
}
