/* x-os BSD Networking Initialization
 *
 * Entry point for the BSD networking subsystem, adapted from XNU's
 * bsd/kern/bsd_init.c. Initializes mbuf allocator and socket layer.
 */

#include "compat/compat.h"

/* Real implementations from x-os adapted BSD files */
extern void mbinit(void);
extern void socketinit(void);

static int bsd_net_inited = 0;

/* Stubs — will be replaced as more of the stack is adapted */
static void domaininit(void) { kputs("[bsd] domains initialized (stub)\n"); }
static void dlil_init(void) { kputs("[bsd] dlil initialized (stub)\n"); }
static void loopattach(void) { kputs("[bsd] loopback attached (stub)\n"); }

void bsd_net_init(void) {
    if (bsd_net_inited) return;
    bsd_net_inited = 1;

    kputs("[bsd] initializing networking subsystem\n");
    mbinit();
    socketinit();
    domaininit();
    dlil_init();
    loopattach();
    kputs("[bsd] networking subsystem ready\n");
}
