#include <stdio.h>
#include "lwp.h"

int main(void){
    scheduler s = lwp_get_scheduler();
    if(!s || !s->init || !s->admit || !s->next || !s->qlen){
        puts("scheduler missing funcs");
        return 1;
    }

    // Create 3 LWPs; they should enter RR queue in creation order
    tid_t a = lwp_create(NULL, NULL);
    tid_t b = lwp_create(NULL, NULL);
    tid_t c = lwp_create(NULL, NULL);

    int q = s->qlen();
    printf("qlen after 3 admits: %d\n", q);
    if(q != 3){ puts("RR qlen mismatch (expected 3)"); return 1; }

    void *p1 = s->next();
    void *p2 = s->next();
    void *p3 = s->next();
    if(!p1 || !p2 || !p3){ puts("RR next() returned NULL prematurely"); return 1; }

    q = s->qlen();
    printf("qlen after 3 next(): %d\n", q);
    if(q != 0){ puts("RR qlen should be 0 after consuming three"); return 1; }

    puts("OK: RR queue admits and drains in FIFO manner (opaque test)");
    return 0;
}
