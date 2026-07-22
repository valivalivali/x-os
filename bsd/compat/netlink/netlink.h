/* X OS compat: netlink stub */
#ifndef _NET_NETLINK_NETLINK_H_
#define _NET_NETLINK_NETLINK_H_

/* Stub - no netlink support in X OS */

/* IFLA attribute types - stubs */
#define IFLA_LINKINFO    0
#define IFLA_INFO_KIND   0

/* netlink writer stubs */
struct nl_writer;
static inline int nlattr_add_nested(struct nl_writer *nw, int type) { return 0; }
static inline int nlattr_add_string(struct nl_writer *nw, int type, const char *str) { return 0; }

#endif /* _NET_NETLINK_NETLINK_H_ */
