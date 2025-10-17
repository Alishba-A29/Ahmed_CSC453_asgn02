#include <stdio.h>
#include "lwp.h"

static int nop(void* p){ (void)p; return 0; }

int main(void){
    (void)lwp_create(nop, NULL);
    (void)lwp_create(nop, NULL);
    scheduler s = lwp_get_scheduler();
    int before = s->qlen ? s->qlen() : -1;

    lwp_set_scheduler(lwp_get_scheduler()); // no-op swap
    s = lwp_get_scheduler();
    int after = s->qlen ? s->qlen() : -1;

    printf("qlen before=%d after=%d\n", before, after);
    return (before == after && before == 2) ? 0 : 1;
}
