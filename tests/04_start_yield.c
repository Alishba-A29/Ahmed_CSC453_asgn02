#include <stdio.h>
#include "../lwp.h"

static int worker(void *p){
    const char *name = (const char*)p;
    for(int i=0;i<3;i++){
        printf("%s step %d\n", name, i);
        lwp_yield();
    }
    lwp_exit(0);
    return 0; // not reached
}

int main(void){
    // create two runnable LWPs
    lwp_create(worker, "A");
    lwp_create(worker, "B");

    // turn this thread into an LWP and start scheduling
    lwp_start();

    // when both workers have exited, control returns here
    puts("back in main after scheduler drained");
    return 0;
}
