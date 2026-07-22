/* X OS compat: bus stubs */
#ifndef _SYS_BUS_H_
#define _SYS_BUS_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>

/* device_t, bus_addr_t, bus_size_t come from sys/types.h when _KERNEL is defined */
#ifndef _KERNEL
typedef int device_t;
typedef uintptr_t bus_addr_t;
typedef size_t bus_size_t;
#endif
typedef void *bus_space_tag_t;
typedef void *bus_space_handle_t;

#define BUS_PROBE_DEFAULT 0
#define BUS_PROBE_LOW_PRIORITY -40
#define BUS_PROBE_GENERIC -100

/* Interrupt type - from sys/bus.h */
enum intr_type {
    INTR_TYPE_NET       = 0x00000002,
    INTR_TYPE_BIO       = 0x00000004,
    INTR_TYPE_CAM       = 0x00000008,
    INTR_TYPE_AV        = 0x00000010,
    INTR_TYPE_CLK       = 0x00000020,
    INTR_TYPE_MISC      = 0x00000040,
    INTR_TYPE_TTY       = 0x00000080,
    INTR_TYPE_TTY_FAST  = 0x00000100,
    INTR_TYPE_FAST      = 0x00000200,
    INTR_TYPE_KILL      = 0x00000400,
    INTR_TYPE_RES2      = 0x00000800,
    INTR_TYPE_RES3      = 0x00001000,
    INTR_TYPE_RES4      = 0x00002000,
    INTR_TYPE_RES5      = 0x00004000,
    INTR_TYPE_RES6      = 0x00008000,
    INTR_TYPE_RES7      = 0x00010000,
    INTR_EXCL           = 0x01000000,
    INTR_MPSAFE         = 0x02000000,
    INTR_ENTROPY        = 0x04000000,
    INTR_MD1            = 0x08000000,
    INTR_MD2            = 0x10000000,
    INTR_MD3            = 0x20000000,
    INTR_MD4            = 0x40000000,
};

/* Driver interrupt types - from sys/bus.h */
typedef int driver_filter_t(void *);
typedef void driver_intr_t(void *);

/* device methods - stubs */
static inline device_t device_get_parent(device_t dev) { return 0; }
static inline const char *device_get_nameunit(device_t dev) { return "net0"; }
static inline const char *device_get_desc(device_t dev) { return "network"; }
static inline int device_get_unit(device_t dev) { return 0; }
static inline void *device_get_softc(device_t dev) { return (void *)0; }
static inline int device_is_attached(device_t dev) { return 1; }
static inline int device_printf(device_t dev, const char *fmt, ...) { return 0; }

/* bus methods - stubs */
#define bus_generic_attach(dev) (0)
#define bus_generic_detach(dev) (0)
#define bus_generic_shutdown(dev) (0)
#define bus_generic_suspend(dev) (0)
#define bus_generic_resume(dev) (0)

/* DEVMETHOD */
#define DEVMETHOD(name, func) { (void *)func }
#define DEVMETHOD_END { NULL }

/* driver_t / devclass_t */
struct driver {
    const char *name;
    int (*methods)(void);
};
typedef int devclass_t;

#define DRIVER_MODULE(name, busname, driver, devclass, mode) \
    static struct driver name##_##busname##_driver = { #name, NULL }

#endif /* _SYS_BUS_H_ */
