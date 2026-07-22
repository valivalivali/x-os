/* X OS compat: prison stubs */
#ifndef _SYS_PRISON_H_
#define _SYS_PRISON_H_

#include <sys/cdefs.h>

struct prison {
    int pr_id;
    char pr_host[256];
    char pr_name[256];
};

extern struct prison prison0;

#define pr_root prison0
#define prison_ref(p) (p)
#define prison_free(p) do { } while (0)
#define prison_hold(p) do { } while (0)

/* prison_check_ip4, prison_local_ip4, prison_remote_ip4, prison_check_af,
 * jailed_without_vnet are declared extern in sys/jail.h and implemented
 * in compat_shims.c */

#endif /* _SYS_PRISON_H_ */
