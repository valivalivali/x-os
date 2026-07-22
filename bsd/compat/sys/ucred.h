/* X OS compat: ucred stubs */
#ifndef _SYS_UCRED_H_
#define _SYS_UCRED_H_

#include <sys/cdefs.h>
#include <sys/types.h>

/* Forward declare struct prison */
struct prison;

struct ucred {
    uint32_t cr_ref;
    uid_t    cr_uid;
    uid_t    cr_ruid;
    uid_t    cr_svuid;
    int      cr_ngroups;
    gid_t    cr_groups[16];
    gid_t    cr_rgid;
    gid_t    cr_svgid;
    struct prison *cr_prison;
    void    *cr_audit;
    void    *cr_label;
    void    *cr_pairs;
    void    *cr_uidinfo;   /* per euid resource consumption */
    void    *cr_ruidinfo;  /* per ruid resource consumption */
    int      cr_flags;     /* credential flags */
};

#define CRED_FLAG_CAPMODE  0x00000001

struct xucred {
    u_int   cr_version;
    uid_t   cr_uid;
    short   cr_ngroups;
    gid_t   cr_groups[16];
};

#define XUCRED_VERSION  0

struct ucred *crget(void);
struct ucred *crhold(struct ucred *cr);
void crfree(struct ucred *cr);
int crcmp(struct ucred *cr1, struct ucred *cr2);
struct ucred *crcop(struct ucred *cr);

#endif /* _SYS_UCRED_H_ */
