/* x-os Socket Layer
 *
 * Minimal socket implementation providing the core BSD socket API.
 * Adapted from XNU's bsd/kern/uipc_socket.c and uipc_syscalls.c,
 * simplified for x-os's single-process networking model.
 */

#include "compat/compat.h"
#include "uipc_mbuf_xos.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"

/* ------------------------------------------------------------------ */
/* Socket constants                                                    */
/* ------------------------------------------------------------------ */

#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    30

#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_RAW     3

#define SOL_SOCKET   0xffff

#define SO_DEBUG        0x0001
#define SO_ACCEPTCONN   0x0002
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_DONTROUTE    0x0010
#define SO_BROADCAST    0x0020
#define SO_USELOOPBACK  0x0040
#define SO_LINGER       0x0080
#define SO_OOBINLINE    0x0100
#define SO_REUSEPORT    0x0200
#define SO_TIMESTAMP    0x0400
#define SO_NOSIGPIPE    0x1024
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008

#define SOMAXCONN   128

#define SS_NOFDREF     0x0001
#define SS_ISCONNECTED 0x0002
#define SS_ISCONNECTING 0x0004
#define SS_ISDISCONNECTING 0x0008
#define SS_CANTSENDMORE 0x0010
#define SS_CANTRCVMORE 0x0020

/* ------------------------------------------------------------------ */
/* Socket structures                                                   */
/* ------------------------------------------------------------------ */

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

struct msghdr {
    void *msg_name;
    int msg_namelen;
    void *msg_control;
    int msg_controllen;
    int msg_flags;
};

/* Socket buffer — simplified */
struct sockbuf {
    struct mbuf *sb_mb;       /* first mbuf in chain */
    struct mbuf *sb_lastrecord; /* last record in chain */
    int sb_cc;                /* character count */
    int sb_hiwat;             /* high water mark */
    int sb_lowat;             /* low water mark */
    int sb_flags;             /* flags */
};

#define SB_MAX (256 * 1024)

/* Protocol switch — defines how a socket behaves */
struct protosw {
    int pr_type;              /* SOCK_STREAM, SOCK_DGRAM, etc. */
    int pr_domain;            /* AF_INET, AF_UNIX, etc. */
    int pr_protocol;          /* IPPROTO_TCP, IPPROTO_UDP, etc. */
    int (*pr_attach)(struct socket *so, int proto);
    int (*pr_detach)(struct socket *so);
    int (*pr_bind)(struct socket *so, struct sockaddr *nam);
    int (*pr_listen)(struct socket *so, int backlog);
    int (*pr_connect)(struct socket *so, struct sockaddr *nam);
    int (*pr_accept)(struct socket *so, struct sockaddr **nam);
    int (*pr_disconnect)(struct socket *so);
    int (*pr_send)(struct socket *so, int flags, struct mbuf *m,
                   struct sockaddr *addr, void *control);
    int (*pr_shutdown)(struct socket *so);
    int (*pr_setsockopt)(struct socket *so, int level, int name,
                         const void *val, int valsize);
    int (*pr_getsockopt)(struct socket *so, int level, int name,
                         void *val, int *valsize);
};

/* Socket structure */
struct socket {
    int so_type;              /* SOCK_STREAM, etc. */
    int so_options;           /* SO_* flags */
    int so_linger;            /* linger time */
    int so_error;             /* error code */
    int so_state;             /* SS_* flags */
    struct sockbuf so_rcv;    /* receive buffer */
    struct sockbuf so_snd;    /* send buffer */
    struct protosw *so_proto; /* protocol switch */
    void *so_pcb;             /* protocol control block */
    int so_fd;                /* file descriptor */
};

/* ------------------------------------------------------------------ */
/* Socket table — x-os uses fixed-size tables                          */
/* ------------------------------------------------------------------ */

#define MAX_SOCKETS 64

static struct socket g_sockets[MAX_SOCKETS];
static int g_sockets_inited = 0;

static void sockets_init(void) {
    if (g_sockets_inited) return;
    memset(g_sockets, 0, sizeof(g_sockets));
    g_sockets_inited = 1;
}

static int sock_alloc(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (g_sockets[i].so_type == 0) {
            memset(&g_sockets[i], 0, sizeof(struct socket));
            return i;
        }
    }
    return -1;
}

static struct socket *sock_get(int fd) {
    if (fd < 0 || fd >= MAX_SOCKETS) return NULL;
    if (g_sockets[fd].so_type == 0) return NULL;
    return &g_sockets[fd];
}

/* ------------------------------------------------------------------ */
/* Protocol stubs — will be connected to TCP/UDP implementations       */
/* ------------------------------------------------------------------ */

/* For now, we provide a loopback-only protocol that echoes data.
 * This allows basic socket API testing without a real network stack. */

static int loop_attach(struct socket *so, int proto) {
    (void)proto;
    so->so_rcv.sb_hiwat = 8192;
    so->so_snd.sb_hiwat = 8192;
    return 0;
}

static int loop_detach(struct socket *so) {
    (void)so;
    return 0;
}

static int loop_bind(struct socket *so, struct sockaddr *nam) {
    (void)so; (void)nam;
    return 0;
}

static int loop_listen(struct socket *so, int backlog) {
    (void)so; (void)backlog;
    return 0;
}

static int loop_connect(struct socket *so, struct sockaddr *nam) {
    (void)nam;
    so->so_state |= SS_ISCONNECTED;
    return 0;
}

static int loop_accept(struct socket *so, struct sockaddr **nam) {
    (void)so; (void)nam;
    return 0;
}

static int loop_disconnect(struct socket *so) {
    so->so_state &= ~SS_ISCONNECTED;
    return 0;
}

static int loop_send(struct socket *so, int flags, struct mbuf *m,
                     struct sockaddr *addr, void *control) {
    (void)flags; (void)addr; (void)control;
    /* Echo: move data from send buffer to receive buffer */
    if (!m) return 0;
    /* Append to receive buffer */
    if (!so->so_rcv.sb_mb) {
        so->so_rcv.sb_mb = m;
    } else {
        struct mbuf *p = so->so_rcv.sb_mb;
        while (p->m_next) p = p->m_next;
        p->m_next = m;
    }
    so->so_rcv.sb_cc += m_length(m);
    return 0;
}

static int loop_shutdown(struct socket *so) {
    so->so_state |= SS_CANTSENDMORE;
    return 0;
}

static int loop_setsockopt(struct socket *so, int level, int name,
                           const void *val, int valsize) {
    (void)so; (void)level; (void)name; (void)val; (void)valsize;
    return 0;
}

static int loop_getsockopt(struct socket *so, int level, int name,
                           void *val, int *valsize) {
    (void)so; (void)level; (void)name; (void)val; (void)valsize;
    return 0;
}

static struct protosw loop_proto = {
    .pr_type = SOCK_STREAM,
    .pr_domain = AF_INET,
    .pr_protocol = 0,
    .pr_attach = loop_attach,
    .pr_detach = loop_detach,
    .pr_bind = loop_bind,
    .pr_listen = loop_listen,
    .pr_connect = loop_connect,
    .pr_accept = loop_accept,
    .pr_disconnect = loop_disconnect,
    .pr_send = loop_send,
    .pr_shutdown = loop_shutdown,
    .pr_setsockopt = loop_setsockopt,
    .pr_getsockopt = loop_getsockopt,
};

static struct protosw dgram_proto = {
    .pr_type = SOCK_DGRAM,
    .pr_domain = AF_INET,
    .pr_protocol = 0,
    .pr_attach = loop_attach,
    .pr_detach = loop_detach,
    .pr_bind = loop_bind,
    .pr_listen = loop_listen,
    .pr_connect = loop_connect,
    .pr_accept = loop_accept,
    .pr_disconnect = loop_disconnect,
    .pr_send = loop_send,
    .pr_shutdown = loop_shutdown,
    .pr_setsockopt = loop_setsockopt,
    .pr_getsockopt = loop_getsockopt,
};

static struct protosw *proto_lookup(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    if (type == SOCK_STREAM) return &loop_proto;
    if (type == SOCK_DGRAM) return &dgram_proto;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Socket API — called by syscall implementations                      */
/* ------------------------------------------------------------------ */

int socreate(int domain, int type, int protocol) {
    sockets_init();
    struct protosw *pr = proto_lookup(domain, type, protocol);
    if (!pr) return -1;

    int fd = sock_alloc();
    if (fd < 0) return -1;

    struct socket *so = &g_sockets[fd];
    so->so_type = type;
    so->so_proto = pr;
    so->so_fd = fd;
    so->so_rcv.sb_hiwat = 8192;
    so->so_snd.sb_hiwat = 8192;

    if (pr->pr_attach) {
        if (pr->pr_attach(so, protocol) != 0) {
            so->so_type = 0;
            return -1;
        }
    }
    return fd;
}

int sobind(int fd, struct sockaddr *nam, int namelen) {
    (void)namelen;
    struct socket *so = sock_get(fd);
    if (!so || !so->so_proto || !so->so_proto->pr_bind) return -1;
    return so->so_proto->pr_bind(so, nam);
}

int solisten(int fd, int backlog) {
    if (backlog < 1) backlog = 1;
    if (backlog > SOMAXCONN) backlog = SOMAXCONN;
    struct socket *so = sock_get(fd);
    if (!so || !so->so_proto || !so->so_proto->pr_listen) return -1;
    if (so->so_proto->pr_listen(so, backlog) != 0) return -1;
    so->so_options |= SO_ACCEPTCONN;
    return 0;
}

int soaccept(int fd, struct sockaddr *nam, int *namelen) {
    (void)nam; (void)namelen;
    struct socket *so = sock_get(fd);
    if (!so) return -1;
    if (!(so->so_options & SO_ACCEPTCONN)) return -1;
    /* Create a new socket for the accepted connection */
    int newfd = sock_alloc();
    if (newfd < 0) return -1;
    struct socket *newso = &g_sockets[newfd];
    newso->so_type = so->so_type;
    newso->so_proto = so->so_proto;
    newso->so_fd = newfd;
    newso->so_state = SS_ISCONNECTED;
    newso->so_rcv.sb_hiwat = 8192;
    newso->so_snd.sb_hiwat = 8192;
    if (so->so_proto->pr_attach) so->so_proto->pr_attach(newso, 0);
    return newfd;
}

int soconnect(int fd, struct sockaddr *nam, int namelen) {
    (void)namelen;
    struct socket *so = sock_get(fd);
    if (!so || !so->so_proto || !so->so_proto->pr_connect) return -1;
    return so->so_proto->pr_connect(so, nam);
}

int sosend(int fd, const void *buf, size_t len, int flags,
           struct sockaddr *addr, int addrlen) {
    struct socket *so = sock_get(fd);
    if (!so) return -1;
    if (so->so_state & SS_CANTSENDMORE) return -1;

    /* Create mbuf for data */
    struct mbuf *m = m_gethdr(0, MT_DATA);
    if (!m) return -1;

    int copylen = (len > MCLBYTES) ? MCLBYTES : (int)len;
    /* Use inline data for small packets, allocate cluster for larger */
    if (copylen > MHLEN) {
        void *cl = kmalloc(MCLBYTES);
        if (!cl) { m_free(m); return -1; }
        memcpy(cl, buf, copylen);
        m->m_ext.ext_buf = cl;
        m->m_ext.ext_size = MCLBYTES;
        m->m_ext.ext_refcnt = 1;
        m->m_data = cl;
        m->m_flags |= M_EXT;
    } else {
        memcpy(m->m_pktdat, buf, copylen);
        m->m_data = m->m_pktdat;
    }
    m->m_len = copylen;
    m->m_pkthdr.len = copylen;

    if (so->so_proto && so->so_proto->pr_send) {
        int err = so->so_proto->pr_send(so, flags, m, addr, NULL);
        if (err) {
            m_freem(m);
            return -1;
        }
    } else {
        m_freem(m);
        return -1;
    }
    return copylen;
}

int soreceive(int fd, void *buf, size_t len, int flags,
              struct sockaddr *addr, int *addrlen) {
    (void)addr; (void)addrlen; (void)flags;
    struct socket *so = sock_get(fd);
    if (!so) return -1;
    if (!so->so_rcv.sb_mb || so->so_rcv.sb_cc == 0) {
        if (so->so_state & SS_CANTRCVMORE) return 0;
        return -1;  /* would block */
    }

    /* Copy data from receive buffer mbuf chain */
    int copied = 0;
    int want = (int)len;
    struct mbuf *m = so->so_rcv.sb_mb;
    while (m && want > 0) {
        int n = m->m_len;
        if (n > want) n = want;
        memcpy((char *)buf + copied, m->m_data, n);
        copied += n;
        want -= n;
        m->m_len -= n;
        m->m_data += n;
        if (m->m_len == 0) {
            struct mbuf *next = m->m_next;
            m->m_next = NULL;
            m_free(m);
            so->so_rcv.sb_mb = next;
            m = next;
        }
    }
    so->so_rcv.sb_cc -= copied;
    return copied;
}

int soshutdown(int fd, int how) {
    struct socket *so = sock_get(fd);
    if (!so) return -1;
    if (how == 1 || how == 2) {
        so->so_state |= SS_CANTSENDMORE;
        if (so->so_proto && so->so_proto->pr_shutdown)
            so->so_proto->pr_shutdown(so);
    }
    if (how == 0 || how == 2) {
        so->so_state |= SS_CANTRCVMORE;
        /* Free receive buffer */
        m_freem(so->so_rcv.sb_mb);
        so->so_rcv.sb_mb = NULL;
        so->so_rcv.sb_cc = 0;
    }
    return 0;
}

int soclose(int fd) {
    struct socket *so = sock_get(fd);
    if (!so) return 0;
    if (so->so_proto && so->so_proto->pr_detach)
        so->so_proto->pr_detach(so);
    m_freem(so->so_rcv.sb_mb);
    m_freem(so->so_snd.sb_mb);
    memset(so, 0, sizeof(struct socket));
    return 0;
}

int sosetsockopt(int fd, int level, int name, const void *val, int valsize) {
    struct socket *so = sock_get(fd);
    if (!so) return -1;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_RCVBUF:
            if (valsize >= 4) so->so_rcv.sb_hiwat = *(int *)val;
            return 0;
        case SO_SNDBUF:
            if (valsize >= 4) so->so_snd.sb_hiwat = *(int *)val;
            return 0;
        case SO_REUSEADDR:
            so->so_options |= SO_REUSEADDR;
            return 0;
        default:
            return 0;
        }
    }
    if (so->so_proto && so->so_proto->pr_setsockopt)
        return so->so_proto->pr_setsockopt(so, level, name, val, valsize);
    return 0;
}

int sogetsockopt(int fd, int level, int name, void *val, int *valsize) {
    struct socket *so = sock_get(fd);
    if (!so) return -1;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_TYPE:
            if (valsize && *valsize >= 4) {
                *(int *)val = so->so_type;
                *valsize = 4;
            }
            return 0;
        case SO_ERROR:
            if (valsize && *valsize >= 4) {
                *(int *)val = so->so_error;
                so->so_error = 0;
                *valsize = 4;
            }
            return 0;
        default:
            return 0;
        }
    }
    if (so->so_proto && so->so_proto->pr_getsockopt)
        return so->so_proto->pr_getsockopt(so, level, name, val, valsize);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

void socketinit(void) {
    sockets_init();
    kputs("[socket] socket layer initialized (loopback)\n");
}
