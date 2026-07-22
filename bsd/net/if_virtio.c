/* if_virtio.c — FreeBSD ifnet wrapper for X OS virtio-net driver.
 *
 * Bridges the X OS virtio-net hardware driver to the FreeBSD network stack
 * by providing an ifnet interface with ether_ifattach().
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/kernel.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_arp.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>

/* X OS virtio-net driver — forward declarations to avoid header conflicts */
#include <stdint.h>
#include <stdbool.h>
extern bool virtio_net_is_ready(void);
extern void virtio_net_get_mac(uint8_t mac[6]);
extern int virtio_net_send(const void *data, int len);
extern int virtio_net_recv(void *buf, int maxlen);

static struct ifnet *vioifp = NULL;

/* Network configuration for QEMU user-mode networking:
 * IP: 10.0.2.15/24, Gateway: 10.0.2.2 */
#define VIOIF_IP_ADDR   0x0A00020F  /* 10.0.2.15 */
#define VIOIF_NETMASK   0xFFFFFF00  /* /24 */
#define VIOIF_GW_ADDR   0x0A000202  /* 10.0.2.2 */

static void
vioif_init(void *arg)
{
	struct ifnet *ifp = (struct ifnet *)arg;

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
}

static int
vioif_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		vioif_init(ifp);
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (error);
}

static int
vioif_send_mbuf(struct mbuf *m)
{
	int total = m->m_pkthdr.len;
	uint8_t buf[2048];
	int off = 0;
	struct mbuf *curr;

	if (total > (int)sizeof(buf))
		total = sizeof(buf);
	for (curr = m; curr != NULL && off < total; curr = curr->m_next) {
		int n = curr->m_len;
		if (off + n > total)
			n = total - off;
		memcpy(buf + off, mtod(curr, const void *), n);
		off += n;
	}
	return virtio_net_send(buf, off);
}

static void
vioif_start(struct ifnet *ifp)
{
	struct mbuf *m;

	for (;;) {
		IFQ_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		int len = m->m_pkthdr.len;
		int rc = vioif_send_mbuf(m);
		if (rc < 0) {
			if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		} else {
			if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
			if_inc_counter(ifp, IFCOUNTER_OBYTES, len);
		}
		m_freem(m);
	}
}

static int
vioif_transmit(struct ifnet *ifp, struct mbuf *m)
{
	int rc = vioif_send_mbuf(m);
	m_freem(m);
	if (rc < 0)
		return (ENOBUFS);
	return (0);
}

/* Poll for received packets and inject into FreeBSD network stack.
 * Called from a periodic timer or the RX interrupt handler. */
void
vioif_rx_poll(void)
{
	struct mbuf *m;
	uint8_t buf[2048];
	int len;

	if (vioifp == NULL)
		return;

	while ((len = virtio_net_recv(buf, sizeof(buf))) > 0) {
		MGETHDR(m, M_NOWAIT, MT_DATA);
		if (m == NULL)
			break;
		MCLGET(m, M_NOWAIT);
		if ((m->m_flags & M_EXT) == 0) {
			m_free(m);
			break;
		}
		m->m_len = len;
		m->m_pkthdr.len = len;
		m->m_pkthdr.rcvif = vioifp;
		memcpy(mtod(m, void *), buf, len);

		/* Hand off to Ethernet input path via if_input callback */
		vioifp->if_input(vioifp, m);
	}
}

/* Initialize the virtio-net ifnet interface and configure IP address. */
void
vioif_attach(void)
{
	uint8_t mac[6];
	struct ifnet *ifp;

	if (!virtio_net_is_ready()) {
		kputs("[vioif] virtio-net not ready\n");
		return;
	}
	virtio_net_get_mac(mac);

	ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		kputs("[vioif] if_alloc failed\n");
		return;
	}

	if_initname(ifp, "vio", 0);
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = vioif_ioctl;
	ifp->if_start = vioif_start;
	ifp->if_init = vioif_init;
	ifp->if_transmit = vioif_transmit;
	ifp->if_mtu = ETHERMTU;
	ifp->if_baudrate = 1000000000;  /* 1 Gbps */

	ether_ifattach(ifp, mac);

	vioifp = ifp;

	kprintf("[vioif] interface attached: %s, IP will be 10.0.2.15/24\n",
	    ifp->if_xname);
	kprintf("[vioif] if_inet=%p if_lladdr=%p\n",
	    ifp->if_inet, IF_LLADDR(ifp));

	/* Configure IP address using in_control(SIOCAIFADDR) */
	{
		struct in_aliasreq ifra;
		memset(&ifra, 0, sizeof(ifra));
		strncpy(ifra.ifra_name, ifp->if_xname, IFNAMSIZ);

		/* Address: 10.0.2.15 */
		ifra.ifra_addr.sin_len = sizeof(struct sockaddr_in);
		ifra.ifra_addr.sin_family = AF_INET;
		ifra.ifra_addr.sin_addr.s_addr = htonl(VIOIF_IP_ADDR);

		/* Netmask: 255.255.255.0 */
		ifra.ifra_mask.sin_len = sizeof(struct sockaddr_in);
		ifra.ifra_mask.sin_family = AF_INET;
		ifra.ifra_mask.sin_addr.s_addr = htonl(VIOIF_NETMASK);

		/* Broadcast: 10.0.2.255 */
		ifra.ifra_broadaddr.sin_len = sizeof(struct sockaddr_in);
		ifra.ifra_broadaddr.sin_family = AF_INET;
		ifra.ifra_broadaddr.sin_addr.s_addr =
		    htonl(VIOIF_IP_ADDR | ~VIOIF_NETMASK);

		int error = in_control(NULL, SIOCAIFADDR, (caddr_t)&ifra, ifp, NULL);
		if (error) {
			kprintf("[vioif] SIOCAIFADDR failed: %d\n", error);
		} else {
			kputs("[vioif] IP address 10.0.2.15/24 configured\n");
		}
	}

	/* Bring interface up */
	ifp->if_flags |= IFF_UP;
	vioif_init(ifp);

	kputs("[vioif] interface up\n");
}

struct ifnet *
vioif_get_ifp(void)
{
	return (vioifp);
}

uint32_t
vioif_get_gateway(void)
{
	return (htonl(VIOIF_GW_ADDR));
}
