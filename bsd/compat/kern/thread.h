#pragma once
#include "../compat.h"
#include "../mach/mach_types.h"

#define current_thread() proc_current()

static inline void thread_reference(thread_t t) { (void)t; }
static inline void thread_deallocate(thread_t t) { (void)t; }
