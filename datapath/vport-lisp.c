/*
 * Copyright (c) 2011 Nicira, Inc.
 * Copyright (c) 2013 Cisco Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)

#include <linux/in.h>
#include <linux/ip.h>
#include <linux/net.h>
#include <linux/rculist.h>
#include <linux/udp.h>

#include <net/icmp.h>
#include <net/ip.h>
#include <net/route.h>
#include <net/udp.h>
#include <net/xfrm.h>

#include "datapath.h"
#include "vport.h"

/*
 *  LISP encapsulation header:
 *
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |N|L|E|V|I|flags|            Nonce/Map-Version                  |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                 Instance ID/Locator Status Bits               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */

/**
 * struct lisphdr - LISP header
 * @nonce_present: Flag indicating the presence of a 24 bit nonce value.
 * @locator_status_bits_present: Flag indicating the presence of Locator Status
 *                               Bits (LSB).
 * @solicit_echo_nonce: Flag indicating the use of the echo noncing mechanism.
 * @map_version_present: Flag indicating the use of mapping versioning.
 * @instance_id_present: Flag indicating the presence of a 24 bit Instance ID.
 * @reserved_flags: 3 bits reserved for future flags.
 * @nonce: 24 bit nonce value.
 * @map_version: 24 bit mapping version.
 * @locator_status_bits: Locator Status Bits: 32 bits when instance_id_present
 *                       is not set, 8 bits when it is.
 * @instance_id: 24 bit Instance ID
 */
struct lisphdr {
#ifdef __LITTLE_ENDIAN_BITFIELD
	__u8 reserved_flags:3;
	__u8 instance_id_present:1;
	__u8 map_version_present:1;
	__u8 solicit_echo_nonce:1;
	__u8 locator_status_bits_present:1;
	__u8 nonce_present:1;
#else
	__u8 nonce_present:1;
	__u8 locator_status_bits_present:1;
	__u8 solicit_echo_nonce:1;
	__u8 map_version_present:1;
	__u8 instance_id_present:1;
	__u8 reserved_flags:3;
#endif
	union {
		__u8 nonce[3];
		__u8 map_version[3];
	} u1;
	union {
		__be32 locator_status_bits;
		struct {
			__u8 instance_id[3];
			__u8 locator_status_bits;
		} word2;
	} u2;
};

#define LISP_HLEN (sizeof(struct udphdr) + sizeof(struct lisphdr))

/**
 * struct lisp_port - Keeps track of open UDP ports
 * @dst_port: lisp UDP port no.
 * @list: list element in @lisp_ports.
 * @lisp_rcv_socket: The socket created for this port number.
 * @name: vport name.
 */
struct lisp_port {
	__be16 dst_port;
	struct list_head list;
	struct socket *lisp_rcv_socket;
	char name[IFNAMSIZ];
};

static LIST_HEAD(lisp_ports);

static inline struct lisp_port *lisp_vport(const struct vport *vport)
{
	return vport_priv(vport);
}

static struct lisp_port *lisp_find_port(struct net *net, __be16 port)
{
	struct lisp_port *lisp_port;

	list_for_each_entry_rcu(lisp_port, &lisp_ports, list) {
		if (lisp_port->dst_port == port &&
			net_eq(sock_net(lisp_port->lisp_rcv_socket->sk), net))
			return lisp_port;
	}

	return NULL;
}

static inline struct lisphdr *lisp_hdr(const struct sk_buff *skb)
{
	return (struct lisphdr *)(udp_hdr(skb) + 1);
}

/* Convert 64 bit tunnel ID to 24 bit Instance ID. */
static void tunnel_id_to_instance_id(__be64 tun_id, __u8 *iid)
{

#ifdef __BIG_ENDIAN
	iid[0] = (__force __u8)(tun_id >> 16);
	iid[1] = (__force __u8)(tun_id >> 8);
	iid[2] = (__force __u8)tun_id;
#else
	iid[0] = (__force __u8)((__force u64)tun_id >> 40);
	iid[1] = (__force __u8)((__force u64)tun_id >> 48);
	iid[2] = (__force __u8)((__force u64)tun_id >> 56);
#endif
}

/* Convert 24 bit Instance ID to 64 bit tunnel ID. */
static __be64 instance_id_to_tunnel_id(__u8 *iid)
{
#ifdef __BIG_ENDIAN
	return (iid[0] << 16) | (iid[1] << 8) | iid[2];
#else
	return (__force __be64)(((__force u64)iid[0] << 40) |
				((__force u64)iid[1] << 48) |
				((__force u64)iid[2] << 56));
#endif
}

/* Compute source UDP port for outgoing packet.
 * Currently we use the flow hash.
 */
static u16 ovs_tnl_get_src_port(struct sk_buff *skb)
{
	int low;
	int high;
	unsigned int range;
	struct sw_flow_key *pkt_key = OVS_CB(skb)->pkt_key;
	u32 hash = jhash2((const u32 *)pkt_key,
			  sizeof(*pkt_key) / sizeof(u32), 0);

	inet_get_local_port_range(&low, &high);
	range = (high - low) + 1;
	return (((u64) hash * range) >> 32) + low;
}

static void lisp_build_header(const struct vport *vport,
			      struct sk_buff *skb,
			      int tunnel_hlen)
{
	struct lisp_port *lisp_port = lisp_vport(vport);
	struct udphdr *udph = udp_hdr(skb);
	struct lisphdr *lisph = (struct lisphdr *)(udph + 1);
	const struct ovs_key_ipv4_tunnel *tun_key = OVS_CB(skb)->tun_key;

	udph->dest = lisp_port->dst_port;
	udph->source = htons(ovs_tnl_get_src_port(skb));
	udph->check = 0;
	udph->len = htons(skb->len - skb_transport_offset(skb));

	lisph->nonce_present = 0;	/* We don't support echo nonce algorithm */
	lisph->locator_status_bits_present = 1;	/* Set LSB */
	lisph->solicit_echo_nonce = 0;	/* No echo noncing */
	lisph->map_version_present = 0;	/* No mapping versioning, nonce instead */
	lisph->instance_id_present = 1;	/* Store the tun_id as Instance ID  */
	lisph->reserved_flags = 0;	/* Reserved flags, set to 0  */

	lisph->u1.nonce[0] = 0;
	lisph->u1.nonce[1] = 0;
	lisph->u1.nonce[2] = 0;

	tunnel_id_to_instance_id(tun_key->tun_id, &lisph->u2.word2.instance_id[0]);
	lisph->u2.word2.locator_status_bits = 1;
}

/**
 *	ovs_tnl_rcv - ingress point for generic tunnel code
 *
 * @vport: port this packet was received on
 * @skb: received packet
 * @tun_key: tunnel that carried packet
 *
 * Must be called with rcu_read_lock.
 *
 * Packets received by this function are in the following state:
 * - skb->data points to the inner Ethernet header.
 * - The inner Ethernet header is in the linear data area.
 * - The layer pointers are undefined.
 */
static void ovs_tnl_rcv(struct vport *vport, struct sk_buff *skb,
			struct ovs_key_ipv4_tunnel *tun_key)
{
	struct ethhdr *eh;

	skb_reset_mac_header(skb);
	eh = eth_hdr(skb);

	if (likely(ntohs(eh->h_proto) >= ETH_P_802_3_MIN))
		skb->protocol = eh->h_proto;
	else
		skb->protocol = htons(ETH_P_802_2);

	skb_dst_drop(skb);
	nf_reset(skb);
	skb_clear_rxhash(skb);
	secpath_reset(skb);
	vlan_set_tci(skb, 0);

	if (unlikely(compute_ip_summed(skb, false))) {
		kfree_skb(skb);
		return;
	}

	ovs_vport_receive(vport, skb, tun_key);
}

/* Called with rcu_read_lock and BH disabled. */
static int lisp_rcv(struct sock *sk, struct sk_buff *skb)
{
	struct lisp_port *lisp_port;
	struct lisphdr *lisph;
	struct iphdr *iph, *inner_iph;
	struct ovs_key_ipv4_tunnel tun_key;
	__be64 key;
	struct ethhdr *ethh;
	__be16 protocol;

	lisp_port = lisp_find_port(dev_net(skb->dev), udp_hdr(skb)->dest);
	if (unlikely(!lisp_port))
		goto error;

	if (unlikely(!pskb_may_pull(skb, LISP_HLEN)))
		goto error;

	lisph = lisp_hdr(skb);

	skb_pull_rcsum(skb, LISP_HLEN);

	if (lisph->instance_id_present != 1)
		key = 0;
	else
		key = instance_id_to_tunnel_id(&lisph->u2.word2.instance_id[0]);

	/* Save outer tunnel values */
	iph = ip_hdr(skb);
	ovs_flow_tun_key_init(&tun_key, iph, key, TUNNEL_KEY);

	/* Drop non-IP inner packets */
	inner_iph = (struct iphdr *)(lisph + 1);
	switch (inner_iph->version) {
	case 4:
		protocol = htons(ETH_P_IP);
		break;
	case 6:
		protocol = htons(ETH_P_IPV6);
		break;
	default:
		goto error;
	}

	/* Add Ethernet header */
	ethh = (struct ethhdr *)skb_push(skb, ETH_HLEN);
	memset(ethh, 0, ETH_HLEN);
	ethh->h_dest[0] = 0x02;
	ethh->h_source[0] = 0x02;
	ethh->h_proto = protocol;

	ovs_skb_postpush_rcsum(skb, skb->data, ETH_HLEN);

	ovs_tnl_rcv(vport_from_priv(lisp_port), skb, &tun_key);
	goto out;

error:
	kfree_skb(skb);
out:
	return 0;
}

/* Arbitrary value.  Irrelevant as long as it's not 0 since we set the handler. */
#define UDP_ENCAP_LISP 1
static int lisp_socket_init(struct lisp_port *lisp_port, struct net *net)
{
	struct sockaddr_in sin;
	int err;

	err = sock_create_kern(AF_INET, SOCK_DGRAM, 0,
			       &lisp_port->lisp_rcv_socket);
	if (err)
		goto error;

	/* release net ref. */
	sk_change_net(lisp_port->lisp_rcv_socket->sk, net);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = lisp_port->dst_port;

	err = kernel_bind(lisp_port->lisp_rcv_socket, (struct sockaddr *)&sin,
			  sizeof(struct sockaddr_in));
	if (err)
		goto error_sock;

	udp_sk(lisp_port->lisp_rcv_socket->sk)->encap_type = UDP_ENCAP_LISP;
	udp_sk(lisp_port->lisp_rcv_socket->sk)->encap_rcv = lisp_rcv;

	udp_encap_enable();

	return 0;

error_sock:
	sk_release_kernel(lisp_port->lisp_rcv_socket->sk);
error:
	pr_warn("cannot register lisp protocol handler: %d\n", err);
	return err;
}

static int lisp_get_options(const struct vport *vport, struct sk_buff *skb)
{
	struct lisp_port *lisp_port = lisp_vport(vport);

	if (nla_put_u16(skb, OVS_TUNNEL_ATTR_DST_PORT, ntohs(lisp_port->dst_port)))
		return -EMSGSIZE;
	return 0;
}

static void lisp_tnl_destroy(struct vport *vport)
{
	struct lisp_port *lisp_port = lisp_vport(vport);

	list_del_rcu(&lisp_port->list);
	/* Release socket */
	sk_release_kernel(lisp_port->lisp_rcv_socket->sk);

	ovs_vport_deferred_free(vport);
}

static struct vport *lisp_tnl_create(const struct vport_parms *parms)
{
	struct net *net = ovs_dp_get_net(parms->dp);
	struct nlattr *options = parms->options;
	struct lisp_port *lisp_port;
	struct vport *vport;
	struct nlattr *a;
	int err;
	u16 dst_port;

	if (!options) {
		err = -EINVAL;
		goto error;
	}

	a = nla_find_nested(options, OVS_TUNNEL_ATTR_DST_PORT);
	if (a && nla_len(a) == sizeof(u16)) {
		dst_port = nla_get_u16(a);
	} else {
		/* Require destination port from userspace. */
		err = -EINVAL;
		goto error;
	}

	/* Verify if we already have a socket created for this port */
	if (lisp_find_port(net, htons(dst_port))) {
		err = -EEXIST;
		goto error;
	}

	vport = ovs_vport_alloc(sizeof(struct lisp_port),
				&ovs_lisp_vport_ops, parms);
	if (IS_ERR(vport))
		return vport;

	lisp_port = lisp_vport(vport);
	lisp_port->dst_port = htons(dst_port);
	strncpy(lisp_port->name, parms->name, IFNAMSIZ);

	err = lisp_socket_init(lisp_port, net);
	if (err)
		goto error_free;

	list_add_tail_rcu(&lisp_port->list, &lisp_ports);
	return vport;

error_free:
	ovs_vport_free(vport);
error:
	return ERR_PTR(err);
}

static bool need_linearize(const struct sk_buff *skb)
{
	int i;

	if (unlikely(skb_shinfo(skb)->frag_list))
		return true;

	/*
	 * Generally speaking we should linearize if there are paged frags.
	 * However, if all of the refcounts are 1 we know nobody else can
	 * change them from underneath us and we can skip the linearization.
	 */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		if (unlikely(page_count(skb_frag_page(&skb_shinfo(skb)->frags[i])) > 1))
			return true;

	return false;
}

static struct sk_buff *handle_offloads(struct sk_buff *skb)
{
	int err;

	forward_ip_summed(skb, true);


	if (skb_is_gso(skb)) {
		struct sk_buff *nskb;
		char cb[sizeof(skb->cb)];

		memcpy(cb, skb->cb, sizeof(cb));

		nskb = __skb_gso_segment(skb, 0, false);
		if (IS_ERR(nskb)) {
			err = PTR_ERR(nskb);
			goto error;
		}

		consume_skb(skb);
		skb = nskb;
		while (nskb) {
			memcpy(nskb->cb, cb, sizeof(cb));
			nskb = nskb->next;
		}
	} else if (get_ip_summed(skb) == OVS_CSUM_PARTIAL) {
		/* Pages aren't locked and could change at any time.
		 * If this happens after we compute the checksum, the
		 * checksum will be wrong.  We linearize now to avoid
		 * this problem.
		 */
		if (unlikely(need_linearize(skb))) {
			err = __skb_linearize(skb);
			if (unlikely(err))
				goto error;
		}

		err = skb_checksum_help(skb);
		if (unlikely(err))
			goto error;
	}

	set_ip_summed(skb, OVS_CSUM_NONE);

	return skb;

error:
	return ERR_PTR(err);
}

static int ovs_tnl_send(struct vport *vport, struct sk_buff *skb,
			u8 ipproto, int tunnel_hlen,
			void (*build_header)(const struct vport *,
					     struct sk_buff *,
					     int tunnel_hlen))
{
	int min_headroom;
	struct rtable *rt;
	__be32 saddr;
	int sent_len = 0;
	int err;
	struct sk_buff *nskb;

	/* Route lookup */
	saddr = OVS_CB(skb)->tun_key->ipv4_src;
	rt = find_route(ovs_dp_get_net(vport->dp),
			&saddr,
			OVS_CB(skb)->tun_key->ipv4_dst,
			ipproto,
			OVS_CB(skb)->tun_key->ipv4_tos,
			skb_get_mark(skb));
	if (IS_ERR(rt)) {
		err = PTR_ERR(rt);
		goto error;
	}

	tunnel_hlen += sizeof(struct iphdr);

	min_headroom = LL_RESERVED_SPACE(rt_dst(rt).dev) + rt_dst(rt).header_len
			+ tunnel_hlen
			+ (vlan_tx_tag_present(skb) ? VLAN_HLEN : 0);

	if (skb_headroom(skb) < min_headroom || skb_header_cloned(skb)) {
		int head_delta = SKB_DATA_ALIGN(min_headroom -
						skb_headroom(skb) +
						16);

		err = pskb_expand_head(skb, max_t(int, head_delta, 0),
					0, GFP_ATOMIC);
		if (unlikely(err))
			goto err_free_rt;
	}

	/* Offloading */
	nskb = handle_offloads(skb);
	if (IS_ERR(nskb)) {
		err = PTR_ERR(nskb);
		goto err_free_rt;
	}
	skb = nskb;

	/* Reset SKB */
	nf_reset(skb);
	secpath_reset(skb);
	skb_dst_drop(skb);
	skb_clear_rxhash(skb);

	while (skb) {
		struct sk_buff *next_skb = skb->next;
		struct iphdr *iph;
		int frag_len;

		skb->next = NULL;

		if (unlikely(vlan_deaccel_tag(skb)))
			goto next;

		frag_len = skb->len;
		skb_push(skb, tunnel_hlen);
		skb_reset_network_header(skb);
		skb_set_transport_header(skb, sizeof(struct iphdr));

		if (next_skb)
			skb_dst_set(skb, dst_clone(&rt_dst(rt)));
		else
			skb_dst_set(skb, &rt_dst(rt));

		/* Push Tunnel header. */
		build_header(vport, skb, tunnel_hlen);

		/* Push IP header. */
		iph = ip_hdr(skb);
		iph->version	= 4;
		iph->ihl	= sizeof(struct iphdr) >> 2;
		iph->protocol	= ipproto;
		iph->daddr	= OVS_CB(skb)->tun_key->ipv4_dst;
		iph->saddr	= saddr;
		iph->tos	= OVS_CB(skb)->tun_key->ipv4_tos;
		iph->ttl	= OVS_CB(skb)->tun_key->ipv4_ttl;
		iph->frag_off	= OVS_CB(skb)->tun_key->tun_flags &
				  TUNNEL_DONT_FRAGMENT ?  htons(IP_DF) : 0;
		/*
		 * Allow our local IP stack to fragment the outer packet even
		 * if the DF bit is set as a last resort.  We also need to
		 * force selection of an IP ID here with __ip_select_ident(),
		 * as ip_select_ident() assumes a proper ID is not needed when
		 * when the DF bit is set.
		 */
		skb->local_df = 1;
		__ip_select_ident(iph, skb_dst(skb), 0);

		memset(IPCB(skb), 0, sizeof(*IPCB(skb)));

		err = ip_local_out(skb);
		if (unlikely(net_xmit_eval(err)))
			goto next;

		sent_len += frag_len;

next:
		skb = next_skb;
	}

	return sent_len;

err_free_rt:
	ip_rt_put(rt);
error:
	return err;
}

static int lisp_tnl_send(struct vport *vport, struct sk_buff *skb)
{
	int tnl_len;
	int network_offset = skb_network_offset(skb);

	if (unlikely(!OVS_CB(skb)->tun_key))
		return -EINVAL;

	/* We only encapsulate IPv4 and IPv6 packets */
	switch (skb->protocol) {
	case htons(ETH_P_IP):
	case htons(ETH_P_IPV6):
		/* Pop off "inner" Ethernet header */
		skb_pull(skb, network_offset);
		tnl_len = ovs_tnl_send(vport, skb, IPPROTO_UDP,
				LISP_HLEN, lisp_build_header);
		return tnl_len > 0 ? tnl_len + network_offset : tnl_len;
	default:
		kfree_skb(skb);
		return 0;
	}
}

static const char *lisp_get_name(const struct vport *vport)
{
	struct lisp_port *lisp_port = lisp_vport(vport);
	return lisp_port->name;
}

const struct vport_ops ovs_lisp_vport_ops = {
	.type		= OVS_VPORT_TYPE_LISP,
	.create		= lisp_tnl_create,
	.destroy	= lisp_tnl_destroy,
	.get_name	= lisp_get_name,
	.get_options	= lisp_get_options,
	.send		= lisp_tnl_send,
};
#else
#warning LISP tunneling will not be available on kernels before 2.6.26
#endif /* Linux kernel < 2.6.26 */
