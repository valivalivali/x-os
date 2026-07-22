/* X OS compat: module stubs */
#ifndef _SYS_MODULE_H_
#define _SYS_MODULE_H_

#include <sys/cdefs.h>

#define MOD_LOAD 1
#define MOD_UNLOAD 2
#define MOD_SHUTDOWN 3
#define MOD_QUIESCE 4

typedef int module_t;
typedef int (*modeventhand_t)(module_t, int, void *);

typedef struct moduledata {
    const char      *name;
    modeventhand_t  evhand;
    void            *priv;
} moduledata_t;

#define MODULE_VERSION(name, version)
#define MODULE_DEPEND(name, depname, minver, prefver, maxver)

#define DECLARE_MODULE(name, data, sub, order) \
    static int name##_modevent(module_t mod, int type, void *data) { return 0; }

#define DEV_MODULE(name, evh, arg) \
    static int name##_modevent(module_t mod, int type, void *arg) { return 0; }

#define NET_MODULE(name, evh, arg) \
    static int name##_modevent(module_t mod, int type, void *arg) { return 0; }

#define SI_SUB_EXEC SI_SUB_PSEUDO
#define SI_ORDER_MIDDLE 0x10

#endif /* _SYS_MODULE_H_ */
