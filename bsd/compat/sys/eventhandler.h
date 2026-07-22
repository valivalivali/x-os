/* X OS compat: eventhandler - include real header for struct definitions */
/* Use a different guard so we don't block the real sys/eventhandler.h */
#ifndef _XOS_COMPAT_EVENTHANDLER_H
#define _XOS_COMPAT_EVENTHANDLER_H

#include <sys/cdefs.h>
#include <sys/queue.h>
#include <sys/malloc.h>

/* Include the real FreeBSD sys/_eventhandler.h for the typedef */
#include <sys/_eventhandler.h>

/* Include the real FreeBSD sys/eventhandler.h for struct definitions */
/* Use a relative path to bypass the compat include directory */
#include "bsd/sys/eventhandler.h"

#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/power.h>

/* The real eventhandler.h structs and macros are now available via */
/* sys/_eventhandler.h and the includes above. */

/* Functions - implemented in compat_shims.c */
eventhandler_tag eventhandler_register(struct eventhandler_list *list,
    const char *name, void *func, void *arg, int priority);
void eventhandler_deregister(struct eventhandler_list *list, eventhandler_tag tag);
struct eventhandler_list *eventhandler_find_list(const char *name);
/* eventhandler_init is static in subr_eventhandler.c - don't declare here */

#endif /* _XOS_COMPAT_EVENTHANDLER_H */
