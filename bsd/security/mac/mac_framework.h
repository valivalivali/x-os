/* X OS compat: MAC framework stub */
#ifndef _SECURITY_MAC_MAC_FRAMEWORK_H_
#define _SECURITY_MAC_MAC_FRAMEWORK_H_

static inline int mac_mbuf_label_init(struct mbuf *m, int flag) { return 0; }
static inline int mac_mbuf_label_associate(const void *cred, struct mbuf *m) { return 0; }
static inline void mac_mbuf_label_destroy(struct mbuf *m) {}
static inline int mac_netinet_fragment(struct mbuf *m, struct mbuf *frag) { return 0; }
static inline int mac_netinet_icmp_reply(struct mbuf *m) { return 0; }
static inline int mac_netinet_tcp_reply(struct mbuf *m) { return 0; }
static inline void mac_inpcb_label_init(struct inpcb *inp) {}
static inline void mac_inpcb_label_destroy(struct inpcb *inp) {}
static inline void mac_inpcb_label_associate(const void *cred, struct inpcb *inp) {}
static inline int mac_inpcb_check_deliver(struct inpcb *inp, struct mbuf *m) { return 0; }
static inline int mac_inpcb_check_visible(struct inpcb *inp, struct mbuf *m) { return 0; }

#endif
