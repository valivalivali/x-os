/* x-os Socket Layer
 *
 * Minimal socket implementation providing the core BSD socket API.
 * Adapted from XNU's bsd/kern/uipc_socket.c and uipc_syscalls.c,
 * simplified for x-os's single-process networking model.
 */

#include "compat/compat.h"
#include "uipc_mbuf_xos.h"
#include "net/net_xos.h"
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
/* TCP protocol — connects to net_xos.c TCP PCBs                       */
/* ------------------------------------------------------------------ */

static int so_tcp_attach(struct socket *so, int proto) {
    (void)proto;
    int pcb = tcp_create();
    if (pcb < 0) return -1;
    so->so_pcb = (void *)(long)(pcb + 1);  /* store pcb+1 so 0 means unattached */
    so->so_rcv.sb_hiwat = 8192;
    so->so_snd.sb_hiwat = 8192;
    return 0;
}

static int so_tcp_detach(struct socket *so) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb >= 0) tcp_close(pcb);
    so->so_pcb = NULL;
    return 0;
}

static int so_tcp_bind(struct socket *so, struct sockaddr *nam) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)nam;
    uint16_t port = ntohs(sin->sin_port);
    return tcp_bind(pcb, port);
}

static int so_tcp_listen(struct socket *so, int backlog) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) return -1;
    return tcp_listen(pcb, backlog);
}

static int so_tcp_connect(struct socket *so, struct sockaddr *nam) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)nam;
    uint32_t ip = ntohl(sin->sin_addr);
    uint16_t port = ntohs(sin->sin_port);
    int ret = tcp_connect(pcb, ip, port);
    if (ret == 0) so->so_state |= SS_ISCONNECTED;
    return ret;
}

static int so_tcp_accept(struct socket *so, struct sockaddr **nam) {
    (void)nam;
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) return -1;
    /* Poll for incoming connections */
    for (int i = 0; i < 100; i++) {
        net_poll();
        int new_pcb = tcp_accept(pcb);
        if (new_pcb >= 0) {
            so->so_pcb = (void *)(long)(new_pcb + 1);
            so->so_state |= SS_ISCONNECTED;
            return 0;
        }
    }
    return -1;
}

static int so_tcp_disconnect(struct socket *so) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb >= 0) tcp_close(pcb);
    so->so_pcb = NULL;
    so->so_state &= ~SS_ISCONNECTED;
    return 0;
}

static int so_tcp_send(struct socket *so, int flags, struct mbuf *m,
                    struct sockaddr *addr, void *control) {
    (void)flags; (void)addr; (void)control;
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) { m_freem(m); return -1; }
    if (!m) return 0;
    int ret = tcp_send(pcb, m->m_data, m->m_len);
    m_freem(m);
    return (ret > 0) ? 0 : -1;
}

static int so_tcp_shutdown(struct socket *so) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb >= 0) {
        tcp_close(pcb);
        so->so_pcb = NULL;
    }
    so->so_state |= SS_CANTSENDMORE;
    return 0;
}

static int so_tcp_setsockopt(struct socket *so, int level, int name,
                          const void *val, int valsize) {
    (void)so; (void)level; (void)name; (void)val; (void)valsize;
    return 0;
}

static int so_tcp_getsockopt(struct socket *so, int level, int name,
                          void *val, int *valsize) {
    (void)so; (void)level; (void)name; (void)val; (void)valsize;
    return 0;
}

static struct protosw tcp_proto = {
    .pr_type = SOCK_STREAM,
    .pr_domain = AF_INET,
    .pr_protocol = 6,  /* IPPROTO_TCP */
    .pr_attach = so_tcp_attach,
    .pr_detach = so_tcp_detach,
    .pr_bind = so_tcp_bind,
    .pr_listen = so_tcp_listen,
    .pr_connect = so_tcp_connect,
    .pr_accept = so_tcp_accept,
    .pr_disconnect = so_tcp_disconnect,
    .pr_send = so_tcp_send,
    .pr_shutdown = so_tcp_shutdown,
    .pr_setsockopt = so_tcp_setsockopt,
    .pr_getsockopt = so_tcp_getsockopt,
};

/* ------------------------------------------------------------------ */
/* UDP protocol — connects to net_xos.c UDP PCBs                       */
/* ------------------------------------------------------------------ */

static int so_udp_attach(struct socket *so, int proto) {
    (void)proto;
    int pcb = udp_create();
    if (pcb < 0) return -1;
    so->so_pcb = (void *)(long)(pcb + 1);
    so->so_rcv.sb_hiwat = 8192;
    so->so_snd.sb_hiwat = 8192;
    return 0;
}

static int so_udp_detach(struct socket *so) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb >= 0) udp_close(pcb);
    so->so_pcb = NULL;
    return 0;
}

static int so_udp_bind(struct socket *so, struct sockaddr *nam) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)nam;
    uint16_t port = ntohs(sin->sin_port);
    return udp_bind(pcb, port);
}

static int so_udp_connect(struct socket *so, struct sockaddr *nam) {
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)nam;
    uint32_t ip = ntohl(sin->sin_addr);
    uint16_t port = ntohs(sin->sin_port);
    int ret = udp_connect(pcb, ip, port);
    if (ret == 0) so->so_state |= SS_ISCONNECTED;
    return ret;
}

static int so_udp_send(struct socket *so, int flags, struct mbuf *m,
                    struct sockaddr *addr, void *control) {
    (void)flags; (void)control;
    int pcb = (int)(long)so->so_pcb - 1;
    if (pcb < 0) { m_freem(m); return -1; }
    if (!m) return 0;

    /* If addr is provided, use sendto semantics */
    if (addr) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        uint32_t ip = ntohl(sin->sin_addr);
        uint16_t port = ntohs(sin->sin_port);
        udp_connect(pcb, ip, port);
    }

    int ret = udp_send(pcb, m->m_data, m->m_len);
    m_freem(m);
    return (ret > 0) ? 0 : -1;
}

static int so_udp_shutdown(struct socket *so) {
    so->so_state |= SS_CANTSENDMORE;
    return 0;
}

static int so_udp_setsockopt(struct socket *so, int level, int name,
                          const void *val, int valsize) {
    (void)so; (void)level; (void)name; (void)val; (void)valsize;
    return 0;
}

static int so_udp_getsockopt(struct socket *so, int level, int name,
                          void *val, int *valsize) {
    (void)so; (void)level; (void)name; (void)val; (void)valsize;
    return 0;
}

static struct protosw udp_proto = {
    .pr_type = SOCK_DGRAM,
    .pr_domain = AF_INET,
    .pr_protocol = 17,  /* IPPROTO_UDP */
    .pr_attach = so_udp_attach,
    .pr_detach = so_udp_detach,
    .pr_bind = so_udp_bind,
    .pr_listen = NULL,
    .pr_connect = so_udp_connect,
    .pr_accept = NULL,
    .pr_disconnect = NULL,
    .pr_send = so_udp_send,
    .pr_shutdown = so_udp_shutdown,
    .pr_setsockopt = so_udp_setsockopt,
    .pr_getsockopt = so_udp_getsockopt,
};

static struct protosw *proto_lookup(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return NULL;
    if (type == SOCK_STREAM) return &tcp_proto;
    if (type == SOCK_DGRAM) return &udp_proto;
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
    if (!so->so_proto || !so->so_proto->pr_accept) return -1;

    /* Create a new socket for the accepted connection */
    int newfd = sock_alloc();
    if (newfd < 0) return -1;
    struct socket *newso = &g_sockets[newfd];
    newso->so_type = so->so_type;
    newso->so_proto = so->so_proto;
    newso->so_fd = newfd;
    newso->so_rcv.sb_hiwat = 8192;
    newso->so_snd.sb_hiwat = 8192;

    /* Let the protocol accept the connection and set up the PCB */
    if (so->so_proto->pr_accept(newso, NULL) != 0) {
        newso->so_type = 0;
        return -1;
    }
    newso->so_state = SS_ISCONNECTED;
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

    /* If no data in socket buffer, try polling the network and
     * pulling data from the TCP/UDP PCB */
    if (!so->so_rcv.sb_mb || so->so_rcv.sb_cc == 0) {
        if (so->so_state & SS_CANTRCVMORE) return 0;

        /* Poll network for incoming packets */
        net_poll();

        /* Try to pull data from the PCB */
        int pcb = (int)(long)so->so_pcb - 1;
        if (pcb >= 0) {
            if (so->so_type == SOCK_STREAM) {
                char tmpbuf[4096];
                int n = tcp_recv(pcb, tmpbuf, sizeof(tmpbuf));
                if (n > 0) {
                    struct mbuf *m = m_gethdr(0, MT_DATA);
                    if (m) {
                        if (n > MHLEN) {
                            void *cl = kmalloc(n);
                            if (cl) {
                                memcpy(cl, tmpbuf, n);
                                m->m_ext.ext_buf = cl;
                                m->m_ext.ext_size = n;
                                m->m_ext.ext_refcnt = 1;
                                m->m_data = cl;
                                m->m_flags |= M_EXT;
                            }
                        } else {
                            memcpy(m->m_pktdat, tmpbuf, n);
                            m->m_data = m->m_pktdat;
                        }
                        m->m_len = n;
                        m->m_pkthdr.len = n;
                        so->so_rcv.sb_mb = m;
                        so->so_rcv.sb_cc = n;
                    }
                }
            } else if (so->so_type == SOCK_DGRAM) {
                char tmpbuf[2048];
                uint32_t src_ip;
                uint16_t src_port;
                int n = udp_recv(pcb, tmpbuf, sizeof(tmpbuf), &src_ip, &src_port);
                if (n > 0) {
                    struct mbuf *m = m_gethdr(0, MT_DATA);
                    if (m) {
                        memcpy(m->m_pktdat, tmpbuf, n);
                        m->m_data = m->m_pktdat;
                        m->m_len = n;
                        m->m_pkthdr.len = n;
                        so->so_rcv.sb_mb = m;
                        so->so_rcv.sb_cc = n;
                    }
                }
            }
        }

        if (!so->so_rcv.sb_mb || so->so_rcv.sb_cc == 0) {
            return -1;  /* still no data — would block */
        }
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
    kputs("[socket] socket layer initialized (TCP/UDP)\n");
}
