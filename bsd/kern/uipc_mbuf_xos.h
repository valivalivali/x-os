#pragma once
#include "compat/compat.h"

/* ------------------------------------------------------------------ */
/* mbuf constants                                                      */
/* ------------------------------------------------------------------ */

#define MSIZE       256
#define MCLBYTES    2048
#define MJUMBYTES   9216
#define MHLEN       (MSIZE - sizeof(struct m_hdr))
#define MINCLSIZE   (MHLEN + 4)

#define M_EXT       0x0001
#define M_PKTHDR    0x0002
#define M_EOR       0x0004
#define M_RDONLY    0x0008
#define M_FREELIST  0x8000

#define MT_FREE     0
#define MT_DATA     1
#define MT_HEADER   2
#define MT_SONAME   3
#define MT_CONTROL  4
#define MT_FTABLE   5
#define MT_RIGHTS   6

/* ------------------------------------------------------------------ */
/* mbuf structures                                                     */
/* ------------------------------------------------------------------ */

struct m_hdr {
    struct mbuf *mh_next;
    struct mbuf *mh_nextpkt;
    void *mh_data;
    int mh_len;
    int mh_flags;
    short mh_type;
};

struct pkthdr {
    int32_t len;
    int32_t rcvif;
    uint16_t flowhash;
    uint16_t csum_flags;
    uint32_t pktid;
};

struct m_ext {
    void *ext_buf;
    void *ext_free_arg;
    uint32_t ext_size;
    void (*ext_free)(void *, void *, uint32_t);
    uint32_t ext_refcnt;
};

struct mbuf {
    struct m_hdr m_hdr;
    union {
        struct {
            struct pkthdr MH_pkthdr;
            union {
                struct m_ext MH_ext;
                char MH_databuf[MHLEN];
            } MH_dat;
        } MH;
        char M_databuf[MSIZE - sizeof(struct m_hdr)];
    } M_dat;
};

#define m_next      m_hdr.mh_next
#define m_nextpkt   m_hdr.mh_nextpkt
#define m_data      m_hdr.mh_data
#define m_len       m_hdr.mh_len
#define m_flags     m_hdr.mh_flags
#define m_type      m_hdr.mh_type

#define m_pkthdr    M_dat.MH.MH_pkthdr
#define m_ext       M_dat.MH.MH_dat.MH_ext
#define m_pktdat    M_dat.MH.MH_dat.MH_databuf

/* ------------------------------------------------------------------ */
/* mbuf API                                                            */
/* ------------------------------------------------------------------ */

struct mbuf *m_get(int wait, int type);
struct mbuf *m_gethdr(int wait, int type);
struct mbuf *m_getcl(int wait, int type, int flags);
void m_free(struct mbuf *m);
void m_freem(struct mbuf *m);
void m_adj(struct mbuf *m, int len);
int m_length(struct mbuf *m);
void m_copydata(const struct mbuf *m, int off, int len, void *dst);
struct mbuf *m_copym(struct mbuf *m, int off, int len, int wait);
void m_cat(struct mbuf *m, struct mbuf *n);
struct mbuf *m_pullup(struct mbuf *m, int len);
void mbinit(void);
