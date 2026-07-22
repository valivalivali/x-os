/* X OS compat: smr - include real header for definitions */
#ifndef _XOS_COMPAT_SMR_H
#define _XOS_COMPAT_SMR_H

#include <sys/cdefs.h>
/* Ensure atomic functions are declared before smr.h uses them */
#include <machine/atomic.h>

/* Include the real sys/smr.h using relative path to bypass compat */
#include "bsd/sys/smr.h"

#endif /* _XOS_COMPAT_SMR_H */
