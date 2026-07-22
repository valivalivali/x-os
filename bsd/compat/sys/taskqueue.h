/* X OS compat: taskqueue - include real header for struct definitions */
#ifndef _XOS_COMPAT_TASKQUEUE_H
#define _XOS_COMPAT_TASKQUEUE_H

#include <sys/cdefs.h>
#include <sys/_stdint.h>
#include <sys/malloc.h>

/* Include the real sys/_task.h for struct task definition */
#include <sys/_task.h>

/* Include the real sys/taskqueue.h using relative path to bypass compat */
#include "bsd/sys/taskqueue.h"

/* Global taskqueue pointers - defined in compat_shims.c */
extern struct taskqueue *taskqueue_fast;
extern struct taskqueue *taskqueue_swi;
extern struct taskqueue *taskqueue_swi_giant;

/* TASK_INITIALIZER - from real taskqueue.h */
#ifndef TASK_INITIALIZER
#define TASK_INITIALIZER(priority, func, context) \
    { .ta_pending = 0, .ta_priority = (priority), .ta_func = (func), \
      .ta_context = (context) }
#endif

#endif /* _XOS_COMPAT_TASKQUEUE_H */
