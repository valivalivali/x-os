/* X OS compat: condvar primitives */
#ifndef _SYS_CONDVAR_H_
#define _SYS_CONDVAR_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

struct cv {
    volatile int waiting;
};

#define cv_init(cv, desc) do { (cv)->waiting = 0; } while (0)
#define cv_destroy(cv) do { } while (0)

/* We can't really sleep/wake in our environment yet, so stub */
#define cv_wait(cv, lock) do { (void)(cv); (void)(lock); } while (0)
#define cv_wait_sig(cv, lock) (0)
#define cv_timedwait(cv, lock, timo) (0)
#define cv_timedwait_sig(cv, lock, timo) (0)

#define cv_signal(cv) do { (cv)->waiting = 0; } while (0)
#define cv_broadcast(cv) do { (cv)->waiting = 0; } while (0)
#define cv_broadcastpri(cv, pri) do { (cv)->waiting = 0; } while (0)

#define cv_wmesg(cv) ((const char *)0)

#endif /* _SYS_CONDVAR_H_ */
