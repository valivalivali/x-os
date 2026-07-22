/* X OS compat: KASSERT/MPASS/CTASSERT - make them no-ops in release builds */
#ifndef _SYS_KASSERT_H_
#define _SYS_KASSERT_H_

#include <sys/cdefs.h>

/* CTASSERT - compile-time assertion */
#ifndef CTASSERT
#define CTASSERT(x)     _Static_assert(x, "compile-time assertion failed")
#endif

/* KASSERT - runtime assertion, stubbed to nothing for now */
#define KASSERT(exp, msg) do { } while (0)

/* MPASS and friends */
#define MPASS4(ex, what, file, line)  do { } while (0)
#define MPASS(ex)               MPASS4(ex, #ex, __FILE__, __LINE__)
#define MPASS2(ex, what)        MPASS4(ex, what, __FILE__, __LINE__)
#define MPASS3(ex, file, line)  MPASS4(ex, #ex, file, line)

/* MPASSERT - stub */
#define MPASSERT(exp, mp, msg) do { } while (0)

/* Pointer assertion */
#define ASSERT_ATOMIC_LOAD(p, width) do { } while (0)

#endif /* _SYS_KASSERT_H_ */
