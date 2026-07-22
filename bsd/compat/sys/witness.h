/* X OS compat: witness stubs */
#ifndef _SYS_WITNESS_H_
#define _SYS_WITNESS_H_

#include <sys/cdefs.h>

#define WITNESS_INIT(lops, type) do { } while (0)
#define WITNESS_DESTROY(lops) do { } while (0)
#define WITNESS_CHECKORDER(lock, flags, file, line, lop) do { } while (0)
#define WITNESS_LOCK(lock, flags, file, line) do { } while (0)
#define WITNESS_UNLOCK(lock, flags, file, line) do { } while (0)
#define WITNESS_CHECK(flags, lock, what) do { } while (0)
#define WITNESS_WARN(flags, lock, fmt, ...) do { } while (0)
#define WITNESS_SAVE_DECL(n)
#define WITNESS_SAVE(lops, n) do { } while (0)
#define WITNESS_RESTORE(lops, n) do { } while (0)
#define WITNESS_NOSLEEP() do { } while (0)
#define WITNESS_SLEEP(type, lock, file, line) do { } while (0)
#define WITNESS_SLEEP_CHECKOP(op, lock) (0)

#endif /* _SYS_WITNESS_H_ */
