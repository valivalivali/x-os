/* X OS compat: sleep queue / wakeup stubs */
#ifndef _SYS_SLEEPQUEUE_H_
#define _SYS_SLEEPQUEUE_H_

#include <sys/cdefs.h>

#define SLEEPQ_SLEEP 0x01
#define SLEEPQ_MSLEEP 0x02
#define SLEEPQ_WAIT 0x03
#define SLEEPQ_PAUSE 0x04

static inline void sleepq_lock(void *wchan) { (void)wchan; }
static inline void sleepq_unlock(void *wchan) { (void)wchan; }
static inline void sleepq_add(void *wchan, void *lock, const char *wmesg, int flags, int queue) { (void)wchan; (void)lock; (void)wmesg; (void)flags; (void)queue; }
static inline void sleepq_remove(void *wchan, void *wchan2) { (void)wchan; (void)wchan2; }
static inline int sleepq_wait_sig(void *wchan, int pri) { (void)wchan; (void)pri; return 0; }
static inline void sleepq_wait(void *wchan, int pri) { (void)wchan; (void)pri; }
static inline void sleepq_broadcast(void *wchan, int flags, int queue) { (void)wchan; (void)flags; (void)queue; }
static inline void sleepq_signal(void *wchan, int flags, int queue, int prop) { (void)wchan; (void)flags; (void)queue; (void)prop; }
static inline int sleepq_abort(void *wchan, int trap) { (void)wchan; (void)trap; return 0; }
static inline int sleepq_sleepcnt(void *wchan, int queue) { (void)wchan; (void)queue; return 0; }

#endif /* _SYS_SLEEPQUEUE_H_ */
