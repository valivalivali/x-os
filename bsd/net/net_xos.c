/* x-os Minimal TCP/IP Stack
 *
 * Provides IP, ICMP, UDP, and TCP layers that sit between the
 * virtio-net driver and the socket layer. This is a simplified
 * implementation focused on basic connectivity (ping, loopback,
 * echo) rather than a full production TCP/IP stack.
 *
 * Architecture:
 *   virtio_net_recv() → ethernet_input() → ip_input() → {icmp, udp, tcp}
 *   socket layer → {tcp, udp}_output() → ip_output() → ethernet_output() → virtio_net_send()
 */

#include "compat/compat.h"
#include "kern/uipc_mbuf_xos.h"
#include "kernel/hal/net/virtio_net.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"

/* ------------------------------------------------------------------ */
/* Network constants                                                   */
/* ------------------------------------------------------------------ */

#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

#define IP_VERSION      4
#define IP_HDR_LEN      20

#define ICMP_ECHO       8
#define ICMP_ECHOREPLY  0

#define TCP_SYN         0x02
#define TCP_ACK         0x10
#define TCP_FIN         0x01
#define TCP_RST         0x04
#define TCP_PSH         0x08

#define ARP_REQUEST     1
#define ARP_REPLY       2

/* ------------------------------------------------------------------ */
/* Network structures                                                  */
/* ------------------------------------------------------------------ */

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed));

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __attribute__((packed));

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Network configuration                                               */
/* ------------------------------------------------------------------ */

static uint8_t  g_mac[ETH_ALEN];
static uint32_t g_ip_addr = 0;       /* Our IP address (host byte order) */
static uint32_t g_gateway = 0;       /* Gateway IP */
static uint32_t g_netmask = 0xFFFFFF00; /* /24 */
static bool     g_net_stack_ready = false;

/* ARP cache */
#define ARP_CACHE_SIZE 16
struct arp_entry {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    bool     valid;
};
static struct arp_entry g_arp_cache[ARP_CACHE_SIZE];

/* ------------------------------------------------------------------ */
/* Checksum helpers                                                    */
/* ------------------------------------------------------------------ */

static uint16_t checksum16(const void *data, int len, uint32_t init_sum) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = init_sum;
    for (int i = 0; i < len - 1; i += 2) {
        sum += (uint16_t)(p[i] << 8 | p[i + 1]);
    }
    if (len & 1) {
        sum += (uint16_t)(p[len - 1] << 8);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static uint16_t ip_checksum(const void *data, int len) {
    return checksum16(data, len, 0);
}

/* TCP/UDP pseudo-header checksum */
static uint32_t tcp_udp_pseudo_checksum(uint32_t src, uint32_t dst,
                                         uint8_t proto, uint16_t len) {
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;
    sum += (uint16_t)proto;
    sum += len;
    return sum;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                 */
/* ------------------------------------------------------------------ */

static void arp_input(struct mbuf *m) {
    if (!m || m->m_len < (int)sizeof(struct arp_hdr)) return;
    struct arp_hdr arp;
    m_copydata(m, 0, sizeof(arp), &arp);

    /* Convert byte order */
    uint16_t oper = (arp.oper >> 8) | (arp.oper << 8);
    uint16_t ptype = (arp.ptype >> 8) | (arp.ptype << 8);

    if (ptype != ETH_TYPE_IP) { m_freem(m); return; }

    uint32_t spa = ntohl(arp.spa);
    uint32_t tpa = ntohl(arp.tpa);

    /* Cache the sender's MAC */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp_cache[i].valid) {
            g_arp_cache[i].ip = spa;
            memcpy(g_arp_cache[i].mac, arp.sha, ETH_ALEN);
            g_arp_cache[i].valid = true;
            break;
        }
    }

    if (oper == ARP_REQUEST && tpa == g_ip_addr) {
        /* Build ARP reply */
        struct {
            struct eth_hdr eth;
            struct arp_hdr arp;
        } __attribute__((packed)) reply;

        memcpy(reply.eth.dst, arp.sha, ETH_ALEN);
        memcpy(reply.eth.src, g_mac, ETH_ALEN);
        reply.eth.type = htons(ETH_TYPE_ARP);

        reply.arp.htype = htons(1);
        reply.arp.ptype = htons(ETH_TYPE_IP);
        reply.arp.hlen = ETH_ALEN;
        reply.arp.plen = 4;
        reply.arp.oper = htons(ARP_REPLY);
        memcpy(reply.arp.sha, g_mac, ETH_ALEN);
        reply.arp.spa = htonl(g_ip_addr);
        memcpy(reply.arp.tha, arp.sha, ETH_ALEN);
        reply.arp.tpa = htonl(spa);

        virtio_net_send(&reply, sizeof(reply));
    }
    m_freem(m);
}

static bool arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    /* Check cache first */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            memcpy(mac_out, g_arp_cache[i].mac, ETH_ALEN);
            return true;
        }
    }

    /* Send ARP request */
    struct {
        struct eth_hdr eth;
        struct arp_hdr arp;
    } __attribute__((packed)) req;

    memset(req.eth.dst, 0xFF, ETH_ALEN);  /* broadcast */
    memcpy(req.eth.src, g_mac, ETH_ALEN);
    req.eth.type = htons(ETH_TYPE_ARP);

    req.arp.htype = htons(1);
    req.arp.ptype = htons(ETH_TYPE_IP);
    req.arp.hlen = ETH_ALEN;
    req.arp.plen = 4;
    req.arp.oper = htons(ARP_REQUEST);
    memcpy(req.arp.sha, g_mac, ETH_ALEN);
    req.arp.spa = htonl(g_ip_addr);
    memset(req.arp.tha, 0, ETH_ALEN);
    req.arp.tpa = htonl(ip);

    virtio_net_send(&req, sizeof(req));

    /* Wait briefly for reply (simplified — poll a few times) */
    for (int attempt = 0; attempt < 100; attempt++) {
        uint8_t buf[VIRTIO_NET_MTU];
        int n = virtio_net_recv(buf, sizeof(buf));
        if (n > 0) {
            /* Re-inject as mbuf for processing */
            struct mbuf *m = m_gethdr(0, MT_DATA);
            if (m) {
                memcpy(m->m_pktdat, buf, n);
                m->m_data = m->m_pktdat;
                m->m_len = n;
                m->m_pkthdr.len = n;
                /* Check if it's an ARP reply */
                struct eth_hdr *eh = (struct eth_hdr *)m->m_data;
                uint16_t etype = ntohs(eh->type);
                if (etype == ETH_TYPE_ARP) {
                    arp_input(m);
                } else {
                    /* Not ARP — put back for later processing */
                    /* TODO: queue for IP processing */
                    m_freem(m);
                }
            }
        }
        /* Check cache again */
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
                memcpy(mac_out, g_arp_cache[i].mac, ETH_ALEN);
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* IP input — dispatch to ICMP/UDP/TCP                                 */
/* ------------------------------------------------------------------ */

static void icmp_input(struct mbuf *m, int ip_hdr_len) {
    if (!m || m->m_len < ip_hdr_len + (int)sizeof(struct icmp_hdr)) {
        m_freem(m); return;
    }

    struct icmp_hdr icmp;
    m_copydata(m, ip_hdr_len, sizeof(icmp), &icmp);

    if (icmp.type == ICMP_ECHO) {
        /* Build echo reply — swap src/dst, change type to ECHOREPLY */
        struct ip_hdr iph;
        m_copydata(m, 0, sizeof(iph), &iph);

        int total_len = ntohs(iph.len);
        uint8_t *pkt = (uint8_t *)kmalloc(total_len);
        if (!pkt) { m_freem(m); return; }
        m_copydata(m, 0, total_len, pkt);

        /* Swap IP src/dst */
        uint32_t tmp = iph.src;
        ((struct ip_hdr *)pkt)->src = iph.dst;
        ((struct ip_hdr *)pkt)->dst = tmp;
        /* Recompute IP checksum */
        ((struct ip_hdr *)pkt)->checksum = 0;
        ((struct ip_hdr *)pkt)->checksum = htons(ip_checksum(pkt, IP_HDR_LEN));

        /* Change ICMP type to ECHOREPLY */
        ((struct icmp_hdr *)(pkt + ip_hdr_len))->type = ICMP_ECHOREPLY;
        /* Recompute ICMP checksum */
        ((struct icmp_hdr *)(pkt + ip_hdr_len))->checksum = 0;
        int icmp_len = total_len - ip_hdr_len;
        ((struct icmp_hdr *)(pkt + ip_hdr_len))->checksum =
            htons(ip_checksum(pkt + ip_hdr_len, icmp_len));

        /* Send via Ethernet */
        uint32_t dst_ip = ntohl(iph.src);
        uint8_t dst_mac[ETH_ALEN];
        if (arp_resolve(dst_ip, dst_mac)) {
            struct {
                struct eth_hdr eth;
                uint8_t data[1500];
            } __attribute__((packed)) frame;
            memcpy(frame.eth.dst, dst_mac, ETH_ALEN);
            memcpy(frame.eth.src, g_mac, ETH_ALEN);
            frame.eth.type = htons(ETH_TYPE_IP);
            memcpy(frame.data, pkt, total_len);
            virtio_net_send(&frame, ETH_HLEN + total_len);
        }
        kfree(pkt);
    }
    m_freem(m);
}

static void ip_input(struct mbuf *m) {
    if (!m || m->m_len < (int)sizeof(struct ip_hdr)) { m_freem(m); return; }

    struct ip_hdr iph;
    m_copydata(m, 0, sizeof(iph), &iph);

    /* Validate version */
    if ((iph.ver_ihl >> 4) != IP_VERSION) { m_freem(m); return; }

    int ip_hdr_len = (iph.ver_ihl & 0xF) * 4;
    if (ip_hdr_len < IP_HDR_LEN) { m_freem(m); return; }

    /* Check destination — accept if it's our IP or broadcast */
    uint32_t dst = ntohl(iph.dst);
    if (dst != g_ip_addr && dst != 0xFFFFFFFF && dst != (g_ip_addr | ~g_netmask)) {
        m_freem(m); return;
    }

    /* Dispatch by protocol */
    switch (iph.proto) {
    case IP_PROTO_ICMP:
        icmp_input(m, ip_hdr_len);
        break;
    case IP_PROTO_UDP:
        /* TODO: UDP input */
        kprintf("[net] UDP packet received (not yet handled)\n");
        m_freem(m);
        break;
    case IP_PROTO_TCP:
        /* TODO: TCP input */
        kprintf("[net] TCP packet received (not yet handled)\n");
        m_freem(m);
        break;
    default:
        m_freem(m);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Ethernet input — called from network poll                           */
/* ------------------------------------------------------------------ */

void ethernet_input(struct mbuf *m) {
    if (!m || m->m_len < (int)sizeof(struct eth_hdr)) { m_freem(m); return; }

    struct eth_hdr eh;
    m_copydata(m, 0, sizeof(eh), &eh);
    uint16_t etype = ntohs(eh.type);

    /* Strip Ethernet header */
    m_adj(m, ETH_HLEN);

    switch (etype) {
    case ETH_TYPE_IP:
        ip_input(m);
        break;
    case ETH_TYPE_ARP:
        /* Re-add ethernet header for ARP processing */
        m_adj(m, -ETH_HLEN);
        arp_input(m);
        break;
    default:
        m_freem(m);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Network poll — call from kernel to process incoming packets         */
/* ------------------------------------------------------------------ */

void net_poll(void) {
    if (!g_net_stack_ready) return;

    uint8_t buf[VIRTIO_NET_MTU];
    int n;
    while ((n = virtio_net_recv(buf, sizeof(buf))) > 0) {
        struct mbuf *m = m_gethdr(0, MT_DATA);
        if (!m) break;
        if (n > MHLEN) {
            /* Use cluster for larger packets */
            void *cl = kmalloc(n);
            if (!cl) { m_free(m); break; }
            memcpy(cl, buf, n);
            m->m_ext.ext_buf = cl;
            m->m_ext.ext_size = n;
            m->m_ext.ext_refcnt = 1;
            m->m_data = cl;
            m->m_flags |= M_EXT;
        } else {
            memcpy(m->m_pktdat, buf, n);
            m->m_data = m->m_pktdat;
        }
        m->m_len = n;
        m->m_pkthdr.len = n;
        ethernet_input(m);
    }
}

/* ------------------------------------------------------------------ */
/* Network initialization                                              */
/* ------------------------------------------------------------------ */

void net_init(void) {
    if (g_net_stack_ready) return;

    if (!virtio_net_is_ready()) {
        kputs("[net] virtio-net not ready, skipping stack init\n");
        return;
    }

    virtio_net_get_mac(g_mac);

    /* Default IP: 10.0.2.15 (QEMU user-mode networking default) */
    g_ip_addr = (10 << 24) | (0 << 16) | (2 << 8) | 15;
    g_gateway = (10 << 24) | (0 << 16) | (2 << 8) | 2;
    g_netmask = 0xFFFFFF00;

    memset(g_arp_cache, 0, sizeof(g_arp_cache));
    g_net_stack_ready = true;

    kprintf("[net] TCP/IP stack ready: IP %d.%d.%d.%d\n",
            (g_ip_addr >> 24) & 0xFF, (g_ip_addr >> 16) & 0xFF,
            (g_ip_addr >> 8) & 0xFF, g_ip_addr & 0xFF);
}

/* ------------------------------------------------------------------ */
/* IP output — build IP packet and send via Ethernet                   */
/* ------------------------------------------------------------------ */

int ip_output(uint32_t dst_ip, uint8_t proto, const void *data, int len) {
    if (!g_net_stack_ready) return -1;

    int total = IP_HDR_LEN + len;
    if (total > VIRTIO_NET_MTU) return -1;

    uint8_t *pkt = (uint8_t *)kmalloc(total);
    if (!pkt) return -1;

    struct ip_hdr *iph = (struct ip_hdr *)pkt;
    iph->ver_ihl = (IP_VERSION << 4) | (IP_HDR_LEN / 4);
    iph->tos = 0;
    iph->len = htons(total);
    iph->id = htons(1);
    iph->frag_off = 0;
    iph->ttl = 64;
    iph->proto = proto;
    iph->checksum = 0;
    iph->src = htonl(g_ip_addr);
    iph->dst = htonl(dst_ip);
    iph->checksum = htons(ip_checksum(iph, IP_HDR_LEN));

    memcpy(pkt + IP_HDR_LEN, data, len);

    /* Resolve destination MAC */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & g_netmask) != (g_ip_addr & g_netmask)) {
        next_hop = g_gateway;
    }

    uint8_t dst_mac[ETH_ALEN];
    if (!arp_resolve(next_hop, dst_mac)) {
        kfree(pkt);
        return -1;
    }

    /* Build Ethernet frame */
    int frame_len = ETH_HLEN + total;
    uint8_t *frame = (uint8_t *)kmalloc(frame_len);
    if (!frame) { kfree(pkt); return -1; }

    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memcpy(eh->dst, dst_mac, ETH_ALEN);
    memcpy(eh->src, g_mac, ETH_ALEN);
    eh->type = htons(ETH_TYPE_IP);
    memcpy(frame + ETH_HLEN, pkt, total);

    int sent = virtio_net_send(frame, frame_len);

    kfree(frame);
    kfree(pkt);
    return sent;
}

/* ------------------------------------------------------------------ */
/* ICMP echo request (ping)                                            */
/* ------------------------------------------------------------------ */

int icmp_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq,
                      const void *data, int data_len) {
    int icmp_len = sizeof(struct icmp_hdr) + data_len;
    uint8_t *icmp = (uint8_t *)kmalloc(icmp_len);
    if (!icmp) return -1;

    struct icmp_hdr *h = (struct icmp_hdr *)icmp;
    h->type = ICMP_ECHO;
    h->code = 0;
    h->checksum = 0;
    h->id = htons(id);
    h->seq = htons(seq);
    memcpy(icmp + sizeof(struct icmp_hdr), data, data_len);
    h->checksum = htons(ip_checksum(icmp, icmp_len));

    int ret = ip_output(dst_ip, IP_PROTO_ICMP, icmp, icmp_len);
    kfree(icmp);
    return ret;
}
