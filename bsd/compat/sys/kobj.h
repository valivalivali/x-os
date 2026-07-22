/* X OS compat: kobj stubs */
#ifndef _SYS_KOBJ_H_
#define _SYS_KOBJ_H_

#include <sys/cdefs.h>

struct kobj;
struct kobj_class;
struct kobj_method;

#define KOBJ_CLASS_FIELDS \
    const char *name; \
    void *ops

#define KOBJ_FIELDS \
    struct kobj_class *ops

static inline void kobj_class_compile(struct kobj_class *cls) { (void)cls; }
static inline void kobj_class_compile_static(struct kobj_class *cls, void *ops) { (void)cls; (void)ops; }
static inline void kobj_class_free(struct kobj_class *cls) { (void)cls; }

static inline void kobj_init(struct kobj *obj, struct kobj_class *cls) { (void)obj; (void)cls; }
static inline void kobj_delete(struct kobj *obj, void *mtype) { (void)obj; (void)mtype; }

#define KOBJMETHOD(name, func) { (void *)func }
#define KOBJMETHOD_END { NULL }

#endif /* _SYS_KOBJ_H_ */
