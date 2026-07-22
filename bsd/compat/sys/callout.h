/* X OS compat: callout/timer - include real header for struct definitions */
#ifndef _XOS_COMPAT_CALLOUT_H
#define _XOS_COMPAT_CALLOUT_H

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* Include the real sys/_callout.h for struct callout definition */
#include <sys/_callout.h>

/* Include the real sys/callout.h using relative path to bypass compat */
#include "bsd/sys/callout.h"

#endif /* _XOS_COMPAT_CALLOUT_H */
