#pragma once
#include "../compat.h"

static inline void thread_wakeup(void *chan) { (void)chan; }
static inline void assert_wait_timeout(void *chan, int type, uint64_t timeout, int units) {
    (void)chan; (void)type; (void)timeout; (void)units;
}
#define THREAD_ABORTSAFE 1
#define TIMEOUT_URGENCY_USER_NORMAL 0
