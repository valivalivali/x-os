#pragma once
#include "../compat.h"
static inline uint64_t clock_get_system_value(void) { return timer_ticks(); }
