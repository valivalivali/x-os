/* x-os Minimal mbuf Allocator
 *
 * Provides a simplified mbuf implementation compatible with the
 * BSD networking stack's API. Instead of XNU's complex zone-based
 * mcache system, we use simple heap allocations.
 *
 * mbufs are network buffers used throughout the socket and TCP/IP
 * code for storing packet data, socket addresses, and control data.
 */

#include "compat/compat.h"
#include "uipc_mbuf_xos.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"

/* ------------------------------------------------------------------ */
/* mbuf allocator                                                      */
/* ------------------------------------------------------------------ */

static int mbuf_count = 0;
static int mbuf_alloc_count = 0;

struct mbuf *m_get(int wait, int type) {
    struct mbuf *m = (struct mbuf *)kmalloc(MSIZE);
    if (!m) return NULL;
    memset(m, 0, MSIZE);
    m->m_type = type;
    m->m_data = m->M_dat.M_databuf;
    m->m_flags = 0;
    mbuf_count++;
    mbuf_alloc_count++;
    return m;
}

struct mbuf *m_gethdr(int wait, int type) {
    struct mbuf *m = m_get(wait, type);
    if (!m) return NULL;
    m->m_flags |= M_PKTHDR;
    m->m_pkthdr.len = 0;
    return m;
}

struct mbuf *m_getcl(int wait, int type, int flags) {
    struct mbuf *m = m_gethdr(wait, type);
    if (!m) return NULL;
    /* Allocate a cluster */
    void *cl = kmalloc(MCLBYTES);
    if (!cl) {
        kfree(m);
        mbuf_count--;
        return NULL;
    }
    memset(cl, 0, MCLBYTES);
    m->m_ext.ext_buf = cl;
    m->m_ext.ext_size = MCLBYTES;
    m->m_ext.ext_refcnt = 1;
    m->m_ext.ext_free = NULL;
    m->m_data = cl;
    m->m_flags |= M_EXT;
    return m;
}

void m_free(struct mbuf *m) {
    if (!m) return;
    if (m->m_flags & M_EXT) {
        if (m->m_ext.ext_buf) {
            if (m->m_ext.ext_refcnt > 1) {
                m->m_ext.ext_refcnt--;
            } else if (m->m_ext.ext_free) {
                m->m_ext.ext_free(m->m_ext.ext_buf, m->m_ext.ext_free_arg, m->m_ext.ext_size);
            } else {
                kfree(m->m_ext.ext_buf);
            }
        }
    }
    kfree(m);
    mbuf_count--;
}

void m_freem(struct mbuf *m) {
    struct mbuf *n;
    while (m) {
        n = m->m_next;
        m_free(m);
        m = n;
    }
}

/* ------------------------------------------------------------------ */
/* mbuf manipulation                                                   */
/* ------------------------------------------------------------------ */

void m_adj(struct mbuf *m, int len) {
    if (!m) return;
    if (len >= 0) {
        /* Trim from head */
        while (m && len > 0) {
            if (m->m_len <= len) {
                len -= m->m_len;
                m->m_len = 0;
                m = m->m_next;
            } else {
                m->m_len -= len;
                m->m_data += len;
                len = 0;
            }
        }
    } else {
        /* Trim from tail */
        len = -len;
        int total = 0;
        struct mbuf *p = m;
        while (p) { total += p->m_len; p = p->m_next; }
        if (len >= total) {
            m_freem(m);
            return;
        }
        int remaining = total - len;
        p = m;
        while (p && remaining > 0) {
            if (p->m_len <= remaining) {
                remaining -= p->m_len;
            } else {
                p->m_len = remaining;
                remaining = 0;
            }
            p = p->m_next;
        }
        /* Free any remaining mbufs after the trim point */
        if (p && p->m_next) {
            m_freem(p->m_next);
            p->m_next = NULL;
        }
    }
}

int m_length(struct mbuf *m) {
    int total = 0;
    while (m) {
        total += m->m_len;
        m = m->m_next;
    }
    return total;
}

void m_copydata(const struct mbuf *m, int off, int len, void *dst) {
    if (!m || !dst) return;
    char *p = (char *)dst;
    while (m && off > 0) {
        if (m->m_len > off) break;
        off -= m->m_len;
        m = m->m_next;
    }
    while (m && len > 0) {
        int n = m->m_len - off;
        if (n > len) n = len;
        if (n > 0) {
            memcpy(p, (char *)m->m_data + off, n);
            p += n;
            len -= n;
        }
        off = 0;
        m = m->m_next;
    }
}

struct mbuf *m_copym(struct mbuf *m, int off, int len, int wait) {
    if (!m) return NULL;
    struct mbuf *top = NULL, **mp = &top;
    int remaining = len;

    while (m && remaining > 0) {
        if (m->m_len <= off) {
            off -= m->m_len;
            m = m->m_next;
            continue;
        }
        int n = m->m_len - off;
        if (n > remaining) n = remaining;

        struct mbuf *nmb = m_get(wait, m->m_type);
        if (!nmb) {
            m_freem(top);
            return NULL;
        }
        nmb->m_len = n;
        memcpy(nmb->m_data, (char *)m->m_data + off, n);
        *mp = nmb;
        mp = &nmb->m_next;
        remaining -= n;
        off = 0;
        m = m->m_next;
    }
    return top;
}

void m_cat(struct mbuf *m, struct mbuf *n) {
    while (m->m_next) m = m->m_next;
    m->m_next = n;
    if (m->m_flags & M_PKTHDR) {
        m->m_pkthdr.len += m_length(n);
    }
}

struct mbuf *m_pullup(struct mbuf *m, int len) {
    if (!m) return NULL;
    if (len <= m->m_len) return m;
    if (m->m_flags & M_EXT) {
        /* Can't pullup into external storage easily — just return */
        return m;
    }
    /* Simple case: if we have enough space in the inline buffer */
    int avail = (int)((char *)m->m_pktdat + MHLEN - (char *)m->m_data);
    if (len <= avail + m->m_len) {
        /* Move data to start of buffer */
        memmove(m->m_pktdat, m->m_data, m->m_len);
        m->m_data = m->m_pktdat;
        /* Pull data from next mbuf if needed */
        while (m->m_len < len && m->m_next) {
            int need = len - m->m_len;
            int have = m->m_next->m_len;
            int n = (need < have) ? need : have;
            memcpy((char *)m->m_data + m->m_len, m->m_next->m_data, n);
            m->m_len += n;
            m->m_next->m_len -= n;
            m->m_next->m_data += n;
            if (m->m_next->m_len == 0) {
                struct mbuf *free_me = m->m_next;
                m->m_next = free_me->m_next;
                m_free(free_me);
            }
        }
        if (m->m_len < len) {
            m_free(m);
            return NULL;
        }
        return m;
    }
    m_free(m);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* mbuf initialization                                                 */
/* ------------------------------------------------------------------ */

void mbinit(void) {
    kputs("[mbuf] allocator initialized (heap-based)\n");
    mbuf_count = 0;
    mbuf_alloc_count = 0;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

int mbuf_get_count(void) { return mbuf_count; }
int mbuf_get_alloc_count(void) { return mbuf_alloc_count; }
