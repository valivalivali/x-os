/* X OS compat: stub out all SYSCTL machinery */
#ifndef _SYS_SYSCTL_H_
#define _SYS_SYSCTL_H_

#include <sys/cdefs.h>
#include <sys/_stdint.h>
#include <sys/queue.h>

/* Forward declarations for sysctl types */
struct sysctl_oid;
struct sysctl_oid_list;

/* Minimal sysctl_oid for SYSCTL_CHILDREN macro */
struct sysctl_oid_list {
    struct sysctl_oid *slh_first;
};
struct sysctl_oid {
    struct sysctl_oid_list oid_children;
    void *oid_arg1;
    intptr_t oid_arg2;
};

/* Minimal sysctl_ctx_list */
struct sysctl_ctx_entry {
    struct sysctl_oid *entry;
    TAILQ_ENTRY(sysctl_ctx_entry) link;
};
struct sysctl_ctx_list {
    TAILQ_HEAD(sysctl_ctx_list_head, sysctl_ctx_entry) ctx_list;
};

#define SYSCTL_CHILDREN(oid_ptr) (&(oid_ptr)->oid_children)

/* Minimal sysctl_req for counter code */
struct sysctl_req {
    struct thread   *td;            /* used for access checking */
    void    *oldptr;
    size_t   oldlen;
    size_t   oldidx;
    void    *newptr;
    size_t   newlen;
    size_t   newidx;
    int     (*oldfunc)(struct sysctl_req *, const void *, size_t);
};

/* OID_AUTO is used as a sentinel for dynamic OID assignment */
#define OID_AUTO        (-1)

/* CTLTYPE flags */
#define CTLTYPE         0xf
#define CTLTYPE_NODE    1
#define CTLTYPE_INT     2
#define CTLTYPE_STRING  3
#define CTLTYPE_S64     4
#define CTLTYPE_OPAQUE  5
#define CTLTYPE_STRUCT  5
#define CTLTYPE_UINT    6
#define CTLTYPE_LONG    7
#define CTLTYPE_ULONG   8
#define CTLTYPE_U64     9
#define CTLTYPE_U8      0xa
#define CTLTYPE_U16     0xb
#define CTLTYPE_S8      0xc
#define CTLTYPE_S16     0xd
#define CTLTYPE_S32     0xe
#define CTLTYPE_U32     0xf

/* CTLFLAG flags */
#define CTLFLAG_RD      0x80000000
#define CTLFLAG_WR      0x40000000
#define CTLFLAG_RW      (CTLFLAG_RD|CTLFLAG_WR)
#define CTLFLAG_RDTUN   (CTLFLAG_RD|0x8000000)
#define CTLFLAG_RWTUN   (CTLFLAG_RW|0x8000000)
#define CTLFLAG_MPSAFE  0x40000000
#define CTLFLAG_SKIP    0x10000000
#define CTLFLAG_CAPRD   0x00080000
#define CTLFLAG_CAPWR   0x00040000
#define CTLFLAG_CAPRW   (CTLFLAG_CAPRD|CTLFLAG_CAPWR)
#define CTLFLAG_TUN     0x8000000
#define CTLFLAG_NOFETCH 0x4000000
#define CTLFLAG_VNET    0x2000000

/* KIPC OID numbers (used as nbr args) */
#define KIPC_MAXSOCKBUF         1
#define KIPC_SOCKBUF_WASTE      2
#define KIPC_SOMAXCONN          3
#define KIPC_MAX_LINKHDR        4
#define KIPC_MAX_PROTOHDR       5
#define KIPC_MAX_HDR            6
#define KIPC_MAX_DATALEN        7

/* CTL_* top-level identifiers */
#define CTL_UNSPEC      0
#define CTL_KERN        1
#define CTL_VM          2
#define CTL_VFS         3
#define CTL_NET         4
#define CTL_DEBUG       5
#define CTL_HW          6
#define CTL_MACHDEP     7
#define CTL_USER        8
#define CTL_P1003_1B    9
#define CTL_SYSCTL      10

/* SYSCTL_CT_ASSERT_MASK */
#define SYSCTL_CT_ASSERT_MASK 0

/* SYSCTL_NULL_INT_PTR */
#define SYSCTL_NULL_INT_PTR ((int *)NULL)

/* Stub all SYSCTL macros to nothing */
#define SYSCTL_OID(parent, nbr, name, kind, a1, a2, handler, fmt, descr)
#define SYSCTL_OID_WITH_LABEL(parent, nbr, name, kind, a1, a2, handler, fmt, descr, label)
#define SYSCTL_OID_RAW(id, parent_child_head, nbr, name, kind, a1, a2, handler, fmt, descr, label)

#define SYSCTL_NODE(parent, nbr, name, access, handler, descr) \
    static int __sysctl_node_##name __attribute__((unused))
#define SYSCTL_NODE_WITH_LABEL(parent, nbr, name, access, handler, descr, label) \
    static int __sysctl_node_##name __attribute__((unused))
#define SYSCTL_NODE_CHILDREN(parent, name)
#define SYSCTL_TIMEVAL_SEC(parent, nbr, name, access, ptr, descr)

#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_INT_WITH_LABEL(parent, nbr, name, access, ptr, val, descr, label)
#define SYSCTL_UINT(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_LONG(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ULONG(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_S8(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_S16(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_S32(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_S64(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_U8(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_U16(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_U32(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_U64(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_STRING(parent, nbr, name, access, arg, len, descr)
#define SYSCTL_STRUCT(parent, nbr, name, access, ptr, type, descr)
#define SYSCTL_DECL(name)

#define SYSCTL_HANDLER_ARGS struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req

#define SYSCTL_PROC(parent, nbr, name, access, ptr, val, handler, fmt, descr)
#define SYSCTL_OPAQUE(parent, nbr, name, access, ptr, len, fmt, descr)

#define SYSCTL_ADD_OID(ctx, parent, nbr, name, kind, a1, a2, handler, fmt, descr)
#define SYSCTL_ADD_NODE(ctx, parent, nbr, name, access, handler, descr) ((struct sysctl_oid *)0)
#define SYSCTL_ADD_NODE_WITH_LABEL(ctx, parent, nbr, name, access, handler, descr, label) ((struct sysctl_oid *)0)
#define SYSCTL_ADD_ROOT_NODE(ctx, nbr, name, access, handler, descr) ((struct sysctl_oid *)0)
#define SYSCTL_STATIC_CHILDREN(parent) ((struct sysctl_oid_list *)0)
#define SYSCTL_ADD_STRING(ctx, parent, nbr, name, access, arg, len, descr)
#define SYSCTL_ADD_CONST_STRING(ctx, parent, nbr, name, access, arg, descr)
#define SYSCTL_ADD_BOOL(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_S8(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_U8(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_S16(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_U16(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_S32(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_U32(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_S64(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_U64(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_INT(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_UINT(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_LONG(ctx, parent, nbr, name, access, ptr, descr)
#define SYSCTL_ADD_ULONG(ctx, parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_ADD_PROC(ctx, parent, nbr, name, access, ptr, arg, handler, fmt, descr)
#define SYSCTL_ADD_STRUCT(ctx, parent, nbr, name, access, ptr, type, descr)

#define SYSCTL_BOOL(parent, nbr, name, access, ptr, val, descr)
#define SYSCTL_COUNTER_U64(parent, nbr, name, access, ptr, descr)
#define SYSCTL_COUNTER_U64_ARRAY(parent, nbr, name, access, ptr, len, descr)

#define SYSCTL_FEATURE(name, desc)

#define SYSCTL_UMA_CUR(parent, nbr, name, access, zptr, descr)
#define SYSCTL_UMA_MAX(parent, nbr, name, access, zptr, descr)

/* SYSCTL_HANDLER stubs - real function stubs */
typedef int sysctl_handle_args_t(struct sysctl_oid *oidp, void *arg1,
    intptr_t arg2, struct sysctl_req *req);
int sysctl_handle_int(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req);
int sysctl_handle_long(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req);
int sysctl_handle_string(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req);
int sysctl_handle_opaque(struct sysctl_oid *oidp, void *arg1, intptr_t arg2, struct sysctl_req *req);

/* SYSCTL_PARENT */
#define SYSCTL_PARENT(oidp) NULL

/* sysctl_ctx_init / sysctl_ctx_free */
struct sysctl_ctx_list;
static inline void sysctl_ctx_init(struct sysctl_ctx_list *c) { (void)c; }
static inline int sysctl_ctx_free(struct sysctl_ctx_list *c) { (void)c; return 0; }

/* sysctl_malloc_type */
struct sysctl_oid;

#endif /* _SYS_SYSCTL_H_ */
