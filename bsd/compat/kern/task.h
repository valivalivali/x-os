#pragma once
#include "../compat.h"
#include "../mach/mach_types.h"

static inline task_t current_task_(void) { return proc_current(); }
#define current_task() proc_current()

static inline kern_return_t task_reference(task_t t) { (void)t; return 0; }
static inline void task_deallocate(task_t t) { (void)t; }
static inline uint64_t task_size(task_t t) { (void)t; return 0; }
