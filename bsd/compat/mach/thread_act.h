#pragma once
#include "mach_types.h"
static inline kern_return_t thread_resume(thread_t t) { (void)t; return 0; }
static inline kern_return_t thread_suspend(thread_t t) { (void)t; return 0; }
