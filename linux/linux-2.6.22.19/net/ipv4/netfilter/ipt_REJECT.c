/*
 * This is a module which is used for rejecting packets.
 */

/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2004 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/route.h>
#include <net/dst.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv4/ipt_REJECT.h>
#ifdef CONFIG_BRIDGE_NETFILTER
#include <linux/netfilter_bridge.h>
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Netfilter Core Team <coreteam@netfilter.org>");
MODULE_DESCRIPTION("iptables REJECT target module");

/* Send RST reply */
static void send_reset(struct sk_buff *oldskb, int hook)
{
	struct sk_buff *nskb;
	struct iphdr *oiph, *niph;
	struct tcphdr _otcph, *oth, *tcph;
	unsigned int addr_type;

	/* IP header checks: fragment. */
	if (ip_hdr(oldskb)->frag_off & htons(IP_OFFSET))
		return;

	oth = skb_header_pointer(oldskb, ip_hdrlen(oldskb),
				 sizeof(_otcph), &_otcph);
	if (oth == NULL)
		return;

	/* No RST for RST. */
	if (oth->rst)
		return;

	/* Check checksum */
	if (nf_ip_checksum(oldskb, hook, ip_hdrlen(oldskb), IPPROTO_TCP))
		return;
	oiph = ip_hdr(oldskb);

	nskb = alloc_skb(sizeof(struct iphdr) + sizeof(struct tcphdr) +
			 LL_MAX_HEADER, GFP_ATOMIC);
	if (!nskb)
		return;

	skb_reserve(nskb, LL_MAX_HEADER);

	skb_reset_network_header(nskb);
	niph = (struct iphdr *)skb_put(nskb, sizeof(struct iphdr));
	niph->version	= 4;
	niph->ihl	= sizeof(struct iphdr) / 4;
	niph->tos	= 0;
	niph->id	= 0;
	niph->frag_off	= htons(IP_DF);
	niph->protocol	= IPPROTO_TCP;
	niph->check	= 0;
	niph->saddr	= oiph->daddr;
	niph->daddr	= oiph->saddr;

	skb_reset_transport_header(nskb);
	tcph = (struct tcphdr *)skb_put(nskb, sizeof(struct tcphdr));
	memset(tcph, 0, sizeof(*tcph));
	tcph->source	= oth->dest;
	tcph->dest	= oth->source;
	tcph->doff	= sizeof(struct tcphdr) / 4;

	if (oth->ack)
		tcph->seq = oth->ack_seq;
	else {
		tcph->ack_seq = htonl(ntohl(oth->seq) + oth->syn + oth->fin +
				      oldskb->len - ip_hdrlen(oldskb) -
				      (oth->doff << 2));
		tcph->ack = 1;
	}

	tcph->rst	= 1;
	tcph->check = ~tcp_v4_check(sizeof(struct tcphdr), niph->saddr,
				    niph->daddr, 0);
	nskb->ip_summed = CHECKSUM_PARTIAL;
	nskb->csum_start = (unsigned char *)tcph - nskb->head;
	nskb->csum_offset = offsetof(struct tcphdr, check);

	addr_type = RTN_UNSPEC;
	if (hook != NF_IP_FORWARD
#ifdef CONFIG_BRIDGE_NETFILTER
	    || (nskb->nf_bridge && nskb->nf_bridge->mask & BRNF_BRIDGED)
#endif
	   )
		addr_type = RTN_LOCAL;

	/* ip_route_me_harder expects skb->dst to be set */
	nskb->dst = dst_clone(oldskb->dst);

	nskb->protocol = htons(ETH_P_IP);
	if (ip_route_me_harder(nskb, addr_type))
		goto free_nskb;

	niph->ttl	= dst_metric(nskb->dst, RTAX_HOPLIMIT);

	/* "Never happens" */
	if (nskb->len > dst_mtu(nskb->dst))
		goto free_nskb;

	nf_ct_attach(nskb, oldskb);

	ip_local_out(nskb);
	return;

 free_nskb:
	kfree_skb(nskb);
}

static inline void send_unreach(struct sk_buff *skb_in, int code)
{
	icmp_send(skb_in, ICMP_DEST_UNREACH, code, 0);
}

static unsigned int
reject_target(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct ipt_reject_info *reject = par->targinfo;

	/* WARNING: This code causes reentry within iptables.
	   This means that the iptables jump stack is now crap.  We
	   must return an absolute verdict. --RR */
	switch (reject->with) {
	case IPT_ICMP_NET_UNREACHABLE:
		send_unreach(skb, ICMP_NET_UNREACH);
		break;
	case IPT_ICMP_HOST_UNREACHABLE:
		send_unreach(skb, ICMP_HOST_UNREACH);
		break;
	case IPT_ICMP_PROT_UNREACHABLE:
		send_unreach(skb, ICMP_PROT_UNREACH);
		break;
	case IPT_ICMP_PORT_UNREACHABLE:
		send_unreach(skb, ICMP_PORT_UNREACH);
		break;
	case IPT_ICMP_NET_PROHIBITED:
		send_unreach(skb, ICMP_NET_ANO);
		break;
	case IPT_ICMP_HOST_PROHIBITED:
		send_unreach(skb, ICMP_HOST_ANO);
		break;
	case IPT_ICMP_ADMIN_PROHIBITED:
		send_unreach(skb, ICMP_PKT_FILTERED);
		break;
	case IPT_TCP_RESET:
		send_reset(skb, par->hooknum);
	case IPT_ICMP_ECHOREPLY:
		/* Doesn't happen. */
		break;
	}

	return NF_DROP;
}

static bool reject_check(const struct xt_tgchk_param *par)
{
	const struct ipt_reject_info *rejinfo = par->targinfo;
	const struct ipt_entry *e = par->entryinfo;

	if (rejinfo->with == IPT_ICMP_ECHOREPLY) {
		printk("ipt_REJECT: ECHOREPLY no longer supported.\n");
		return false;
	} else if (rejinfo->with == IPT_TCP_RESET) {
		/* Must specify that it's a TCP packet */
		if (e->ip.proto != IPPROTO_TCP
		    || (e->ip.invflags & XT_INV_PROTO)) {
			printk("ipt_REJECT: TCP_RESET invalid for non-tcp\n");
			return false;
		}
	}
	return true;
}

static struct xt_target ipt_reject_reg __read_mostly = {
	.name		= "REJECT",
	.family		= AF_INET,
	.target		= reject_target,
	.targetsize	= sizeof(struct ipt_reject_info),
	.table		= "filter",
	.hooks		= (1 << NF_IP_LOCAL_IN) | (1 << NF_IP_FORWARD) |
			  (1 << NF_IP_LOCAL_OUT),
	.checkentry	= reject_check,
	.me		= THIS_MODULE,
};

static int __init ipt_reject_init(void)
{
	return xt_register_target(&ipt_reject_reg);
}

static void __exit ipt_reject_fini(void)
{
	xt_unregister_target(&ipt_reject_reg);
}

module_init(ipt_reject_init);
module_exit(ipt_reject_fini);
