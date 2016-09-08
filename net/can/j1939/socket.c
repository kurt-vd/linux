/*
 * Copyright (c) 2010-2011 EIA Electronics
 *
 * Authors:
 * Kurt Van Dijck <kurt.van.dijck@eia.be>
 * Pieter Beyens <pieter.beyens@eia.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/socket.h>
#include <linux/list.h>
#include <linux/if_arp.h>
#include <net/tcp_states.h>

#include <linux/can/core.h>
#include <linux/can/skb.h>
#include <linux/can/j1939.h>
#include "j1939-priv.h"

#define J1939_MIN_NAMELEN required_size(can_addr.j1939, struct sockaddr_can)

/* list of sockets */
static struct list_head j1939_socks = LIST_HEAD_INIT(j1939_socks);
static DEFINE_SPINLOCK(j1939_socks_lock);

struct j1939_sock {
	struct sock sk; /* must be first to skip with memset */
	struct list_head list;

	int state;

#define JSK_BOUND BIT(0)
#define JSK_CONNECTED BIT(1)
#define PROMISC BIT(2)
#define RECV_OWN BIT(3)

	int ifindex_started; /* ifindex of netdev */

	struct {
		name_t src;
		name_t dst;
		pgn_t pgn;

		u8 sa, da;
	} addr;

	struct j1939_filter *filters;
	int nfilters;

	/* j1939 may emit equal PGN (!= equal CAN-id's) out of order
	 * when transport protocol comes in.
	 * To allow emitting in order, keep a 'pending' nr. of packets
	 */
	int skb_pending;
	spinlock_t lock;
	wait_queue_head_t waitq;
};

static inline struct j1939_sock *j1939_sk(const struct sock *sk)
{
	return container_of(sk, struct j1939_sock, sk);
}

/* skb_pending issues */
static inline int j1939_sock_pending_add_first(struct sock *sk)
{
	int saved;
	struct j1939_sock *jsk = j1939_sk(sk);

	spin_lock_bh(&jsk->lock);
	if (!jsk->skb_pending) {
		++jsk->skb_pending;
		saved = 1;
	} else {
		saved = 0;
	}
	spin_unlock_bh(&jsk->lock);
	return saved;
}

static inline void j1939_sock_pending_add(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);

	spin_lock_bh(&jsk->lock);
	++jsk->skb_pending;
	spin_unlock_bh(&jsk->lock);
}

void j1939_sock_pending_del(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);
	int saved;

	spin_lock_bh(&jsk->lock);
	--jsk->skb_pending;
	saved = jsk->skb_pending;
	spin_unlock_bh(&jsk->lock);
	if (!saved)
		wake_up(&jsk->waitq);
}

static inline int j1939_no_address(const struct sock *sk)
{
	const struct j1939_sock *jsk = j1939_sk(sk);

	return (jsk->addr.sa == J1939_NO_ADDR) && !jsk->addr.src;
}

/* matches skb control buffer (addr) with a j1939 filter */
static inline int packet_match(const struct j1939_sk_buff_cb *skcb,
			       const struct j1939_filter *f, int nfilter)
{
	if (!nfilter)
		/* receive all when no filters are assigned */
		return 1;

	/* Filters relying on the addr for static addressing _should_ get
	 * packets from dynamic addressed ECU's too if they match their SA.
	 * Sockets using dynamic addressing in their filters should not set it.
	 */
	for (; nfilter; ++f, --nfilter) {
		if ((skcb->pgn & f->pgn_mask) != (f->pgn & f->pgn_mask))
			continue;
		if ((skcb->srcaddr & f->addr_mask) != (f->addr & f->addr_mask))
			continue;
		if ((skcb->srcname & f->name_mask) != (f->name & f->name_mask))
			continue;
		return 1;
	}
	return 0;
}

/* callback per socket, called from j1939_recv */
static void j1939sk_recv_skb(struct sk_buff *oskb, struct j1939_sock *jsk)
{
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *skcb = (void *)oskb->cb;

	if (!(jsk->state & (JSK_BOUND | JSK_CONNECTED)))
		return;
	if (jsk->sk.sk_bound_dev_if &&
	    (jsk->sk.sk_bound_dev_if != oskb->skb_iif))
		/* this socket does not take packets from this iface */
		return;
	if (!(jsk->state & PROMISC)) {
		if (jsk->addr.src) {
			/* reject message for other destinations */
			if (skcb->dstname &&
			    (skcb->dstname != jsk->addr.src))
				/* the msg is not destined for the name
				 * that the socket is bound to
				 */
				return;
		} else {
			/* reject messages for other destination addresses */
			if (j1939_address_is_unicast(skcb->dstaddr) &&
			    (skcb->dstaddr != jsk->addr.sa))
				/* the msg is not destined for the name
				 * that the socket is bound to
				 */
				return;
		}
	}

	if ((skcb->insock == &jsk->sk) && !(jsk->state & RECV_OWN))
		/* own message */
		return;

	if (!packet_match(skcb, jsk->filters, jsk->nfilters))
		return;

	skb = skb_clone(oskb, GFP_ATOMIC);
	if (!skb) {
		j1939_warning("skb clone failed\n");
		return;
	}
	skcb = (void *)skb->cb;
	skcb->msg_flags &= ~(MSG_DONTROUTE | MSG_CONFIRM);
	if (skcb->insock)
		skcb->msg_flags |= MSG_DONTROUTE;
	if (skcb->insock == &jsk->sk)
		skcb->msg_flags |= MSG_CONFIRM;

	if (sock_queue_rcv_skb(&jsk->sk, skb) < 0)
		kfree_skb(skb);
}

void j1939_recv(struct sk_buff *skb)
{
	struct j1939_sock *jsk;

	spin_lock_bh(&j1939_socks_lock);
	list_for_each_entry(jsk, &j1939_socks, list) {
		j1939sk_recv_skb(skb, jsk);
	}
	spin_unlock_bh(&j1939_socks_lock);
}
EXPORT_SYMBOL_GPL(j1939_recv);

static int j1939sk_init(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);

	INIT_LIST_HEAD(&jsk->list);
	spin_lock_init(&jsk->lock);
	init_waitqueue_head(&jsk->waitq);
	jsk->sk.sk_priority = j1939_to_sk_priority(6);
	jsk->sk.sk_reuse = 1; /* per default */
	jsk->addr.sa = J1939_NO_ADDR;
	jsk->addr.da = J1939_NO_ADDR;
	jsk->addr.pgn = J1939_NO_PGN;
	return 0;
}

/* helper: return <0 for error, >0 for error to notify */
static int j1939_ifindex_start(int ifindex)
{
	int ret;
	struct net_device *netdev;

	netdev = dev_get_by_index(&init_net, ifindex);
	if (!netdev)
		return -ENODEV;

	/* no need to test for CAN device,
	 * done by j1939_netdev_start
	 */
	ret = j1939_netdev_start(netdev);

	dev_put(netdev);
	return ret;
}

static void j1939_ifindex_stop(int ifindex)
{
	struct net_device *netdev;

	netdev = dev_get_by_index(&init_net, ifindex);
	if (netdev) {
		j1939_netdev_stop(netdev);
		dev_put(netdev);
	}
}

static int j1939sk_bind(struct socket *sock, struct sockaddr *uaddr, int len)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct j1939_sock *jsk = j1939_sk(sock->sk);
	int ret, bound_dev_if;
	struct j1939_priv *priv;

	if (len < J1939_MIN_NAMELEN)
		return -EINVAL;
	if (addr->can_family != AF_CAN)
		return -EINVAL;

	lock_sock(sock->sk);

	/* bind to device ... */
	bound_dev_if = jsk->sk.sk_bound_dev_if;
	/* copy netdev info */
	if (!bound_dev_if && addr->can_ifindex) {
		bound_dev_if = addr->can_ifindex;
	} else if (bound_dev_if && addr->can_ifindex) {
		/* do netdev */
		if (bound_dev_if != addr->can_ifindex) {
			ret = -EBUSY;
			goto fail_locked;
		}
	}
	/* start j1939 */
	if (bound_dev_if && bound_dev_if != jsk->ifindex_started) {
		if (jsk->ifindex_started) {
			ret = -EBUSY;
			goto fail_locked;
		}
		ret = j1939_ifindex_start(bound_dev_if);
		if (ret < 0)
			goto fail_locked;
		jsk->ifindex_started = bound_dev_if;
		priv = j1939_priv_find(jsk->ifindex_started);
		j1939_name_local_get(priv, jsk->addr.src);
		j1939_addr_local_get(priv, jsk->addr.sa);
		put_j1939_priv(priv);
	}

	jsk->sk.sk_bound_dev_if = bound_dev_if;

	/* set addr + name */
	if (jsk->ifindex_started) {
		priv = j1939_priv_find(jsk->ifindex_started);
		/* priv should be set when ifindex_started is nonzero */
		j1939_name_local_put(priv, jsk->addr.src);
		j1939_name_local_get(priv, addr->can_addr.j1939.name);
		j1939_addr_local_put(priv, jsk->addr.sa);
		j1939_addr_local_get(priv, addr->can_addr.j1939.addr);
		put_j1939_priv(priv);
	}
	jsk->addr.src = addr->can_addr.j1939.name;
	jsk->addr.sa = addr->can_addr.j1939.addr;

	/* set default transmit pgn */
	if (pgn_is_valid(addr->can_addr.j1939.pgn))
		jsk->addr.pgn = addr->can_addr.j1939.pgn;

	if (!(jsk->state & (JSK_BOUND | JSK_CONNECTED))) {
		spin_lock_bh(&j1939_socks_lock);
		list_add_tail(&jsk->list, &j1939_socks);
		spin_unlock_bh(&j1939_socks_lock);
	}
	jsk->state |= JSK_BOUND;

	ret = 0;

 fail_locked:
	if (!jsk->sk.sk_bound_dev_if && jsk->ifindex_started) {
		/* started j1939 on this netdev during this call,
		 * so we revert that
		 */
		j1939_ifindex_stop(jsk->ifindex_started);
		jsk->ifindex_started = 0;
	}
	release_sock(sock->sk);
	return ret;
}

static int j1939sk_connect(struct socket *sock, struct sockaddr *uaddr,
			   int len, int flags)
{
	int ret;
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct j1939_sock *jsk = j1939_sk(sock->sk);
	struct j1939_priv *priv;
	int bound_dev_if;

	if (!uaddr)
		return -EDESTADDRREQ;

	if (len < J1939_MIN_NAMELEN)
		return -EINVAL;
	if (addr->can_family != AF_CAN)
		return -EINVAL;

	lock_sock(sock->sk);

	/* bind to device ... */
	bound_dev_if = jsk->sk.sk_bound_dev_if;

	/* copy netdev info */
	if (!bound_dev_if && addr->can_ifindex) {
		bound_dev_if = addr->can_ifindex;
	} else if (bound_dev_if && addr->can_ifindex) {
		/* do netdev */
		if (bound_dev_if != addr->can_ifindex) {
			ret = -EBUSY;
			goto fail_locked;
		}
	}

	/* start j1939 */
	if (bound_dev_if && bound_dev_if != jsk->ifindex_started) {
		if (jsk->ifindex_started) {
			ret = -EBUSY;
			goto fail_locked;
		}
		ret = j1939_ifindex_start(bound_dev_if);
		if (ret < 0)
			goto fail_locked;
		jsk->ifindex_started = bound_dev_if;
		/* make sure that this is in sync */
		priv = j1939_priv_find(jsk->ifindex_started);
		j1939_name_local_get(priv, jsk->addr.src);
		j1939_addr_local_get(priv, jsk->addr.sa);
		put_j1939_priv(priv);
	}

	/* lookup destination */
	jsk->addr.dst = addr->can_addr.j1939.name;
	jsk->addr.da = addr->can_addr.j1939.addr;

	/* start assigning, no problem can occur at this point anymore */
	jsk->sk.sk_bound_dev_if = bound_dev_if;

	if (pgn_is_valid(addr->can_addr.j1939.pgn))
		jsk->addr.pgn = addr->can_addr.j1939.pgn;

	if (!(jsk->state & (JSK_BOUND | JSK_CONNECTED))) {
		spin_lock_bh(&j1939_socks_lock);
		list_add_tail(&jsk->list, &j1939_socks);
		spin_unlock_bh(&j1939_socks_lock);
	}
	jsk->state |= JSK_CONNECTED;
	ret = 0;

 fail_locked:
	release_sock(sock->sk);
	return ret;
}

static void j1939sk_sock2sockaddr_can(struct sockaddr_can *addr,
				      const struct j1939_sock *jsk, int peer)
{
	addr->can_family = AF_CAN;
	addr->can_ifindex = jsk->sk.sk_bound_dev_if;
	addr->can_addr.j1939.name = peer ? jsk->addr.dst : jsk->addr.src;
	addr->can_addr.j1939.pgn = jsk->addr.pgn;
	addr->can_addr.j1939.addr = peer ? jsk->addr.da : jsk->addr.sa;
}

static int j1939sk_getname(struct socket *sock, struct sockaddr *uaddr,
			   int *len, int peer)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	int ret = 0;

	lock_sock(sk);

	if (peer && !(jsk->state & JSK_CONNECTED)) {
		ret = -EADDRNOTAVAIL;
		goto failure;
	}

	j1939sk_sock2sockaddr_can(addr, jsk, peer);
	*len = J1939_MIN_NAMELEN;

 failure:
	release_sock(sk);

	return ret;
}

static int j1939sk_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk;
	struct j1939_priv *priv;

	if (!sk)
		return 0;
	lock_sock(sk);
	jsk = j1939_sk(sk);
	spin_lock_bh(&j1939_socks_lock);
	list_del_init(&jsk->list);
	spin_unlock_bh(&j1939_socks_lock);

	if (jsk->ifindex_started) {
		priv = j1939_priv_find(jsk->ifindex_started);
		j1939_addr_local_put(priv, jsk->addr.sa);
		j1939_name_local_put(priv, jsk->addr.src);
		put_j1939_priv(priv);

		j1939_ifindex_stop(jsk->ifindex_started);
	}
	jsk->ifindex_started = 0;

	sock_orphan(sk);
	sock->sk = NULL;

	release_sock(sk);
	sock_put(sk);

	return 0;
}

static int j1939sk_setsockopt_flag(struct j1939_sock *jsk, char __user *optval,
				   unsigned int optlen, int flag)
{
	int tmp;

	if (optlen != sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(&tmp, optval, optlen))
		return -EFAULT;
	lock_sock(&jsk->sk);
	if (tmp)
		jsk->state |= flag;
	else
		jsk->state &= ~flag;
	release_sock(&jsk->sk);
	return tmp;
}

static int j1939sk_setsockopt(struct socket *sock, int level, int optname,
			      char __user *optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	int tmp, count;
	struct j1939_filter *filters, *ofilters;

	if (level != SOL_CAN_J1939)
		return -EINVAL;

	switch (optname) {
	case SO_J1939_FILTER:
		if (optval) {
			if (optlen % sizeof(*filters) != 0)
				return -EINVAL;
			count = optlen / sizeof(*filters);
			filters = kmalloc(optlen, GFP_KERNEL);
			if (!filters)
				return -ENOMEM;
			if (copy_from_user(filters, optval, optlen)) {
				kfree(filters);
				return -EFAULT;
			}
		} else {
			filters = NULL;
			count = 0;
		}

		spin_lock_bh(&j1939_socks_lock);
		ofilters = jsk->filters;
		jsk->filters = filters;
		jsk->nfilters = count;
		spin_unlock_bh(&j1939_socks_lock);
		kfree(ofilters);
		return 0;
	case SO_J1939_PROMISC:
		return j1939sk_setsockopt_flag(jsk, optval, optlen, PROMISC);
	case SO_J1939_RECV_OWN:
		return j1939sk_setsockopt_flag(jsk, optval, optlen, RECV_OWN);
	case SO_J1939_SEND_PRIO:
		if (optlen != sizeof(tmp))
			return -EINVAL;
		if (copy_from_user(&tmp, optval, optlen))
			return -EFAULT;
		if ((tmp < 0) || (tmp > 7))
			return -EDOM;
		if ((tmp < 2) && !capable(CAP_NET_ADMIN))
			return -EPERM;
		lock_sock(&jsk->sk);
		jsk->sk.sk_priority = j1939_to_sk_priority(tmp);
		release_sock(&jsk->sk);
		return 0;
	default:
		return -ENOPROTOOPT;
	}
}

static int j1939sk_getsockopt(struct socket *sock, int level, int optname,
			      char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	int ret, ulen;
	/* set defaults for using 'int' properties */
	int tmp = 0;
	int len = sizeof(tmp);
	void *val = &tmp;

	if (level != SOL_CAN_J1939)
		return -EINVAL;
	if (get_user(ulen, optlen))
		return -EFAULT;
	if (ulen < 0)
		return -EINVAL;

	lock_sock(&jsk->sk);
	switch (optname) {
	case SO_J1939_PROMISC:
		tmp = (jsk->state & PROMISC) ? 1 : 0;
		break;
	case SO_J1939_RECV_OWN:
		tmp = (jsk->state & RECV_OWN) ? 1 : 0;
		break;
	case SO_J1939_SEND_PRIO:
		tmp = j1939_prio(jsk->sk.sk_priority);
		break;
	default:
		ret = -ENOPROTOOPT;
		goto no_copy;
	}

	/* copy to user, based on 'len' & 'val'
	 * but most sockopt's are 'int' properties, and have 'len' & 'val'
	 * left unchanged, but instead modified 'tmp'
	 */
	if (len > ulen)
		ret = -EFAULT;
	else if (put_user(len, optlen))
		ret = -EFAULT;
	else if (copy_to_user(optval, val, len))
		ret = -EFAULT;
	else
		ret = 0;
 no_copy:
	release_sock(&jsk->sk);
	return ret;
}

static int j1939sk_recvmsg(struct socket *sock, struct msghdr *msg,
			   size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *skcb;
	int ret = 0;

	skb = skb_recv_datagram(sk, flags, 0, &ret);
	if (!skb)
		return ret;

	if (size < skb->len)
		msg->msg_flags |= MSG_TRUNC;
	else
		size = skb->len;

	ret = memcpy_to_msg(msg, skb->data, size);
	if (ret < 0) {
		skb_free_datagram(sk, skb);
		return ret;
	}

	skcb = (void *)skb->cb;
	if (j1939_address_is_valid(skcb->dstaddr))
		put_cmsg(msg, SOL_CAN_J1939, SCM_J1939_DEST_ADDR,
			 sizeof(skcb->dstaddr), &skcb->dstaddr);

	if (skcb->dstname)
		put_cmsg(msg, SOL_CAN_J1939, SCM_J1939_DEST_NAME,
			 sizeof(skcb->dstname), &skcb->dstname);

	put_cmsg(msg, SOL_CAN_J1939, SCM_J1939_PRIO,
		 sizeof(skcb->priority), &skcb->priority);

	if (msg->msg_name) {
		struct sockaddr_can *paddr = msg->msg_name;

		msg->msg_namelen = J1939_MIN_NAMELEN;
		memset(msg->msg_name, 0, msg->msg_namelen);
		paddr->can_family = AF_CAN;
		paddr->can_ifindex = skb->skb_iif;
		paddr->can_addr.j1939.name = skcb->srcname;
		paddr->can_addr.j1939.addr = skcb->srcaddr;
		paddr->can_addr.j1939.pgn = skcb->pgn;
	}

	sock_recv_ts_and_drops(msg, sk, skb);
	msg->msg_flags |= skcb->msg_flags;
	skb_free_datagram(sk, skb);

	return size;
}

static int j1939sk_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	struct sockaddr_can *addr = msg->msg_name;
	struct j1939_sk_buff_cb *skcb;
	struct sk_buff *skb;
	struct net_device *dev;
	int ifindex;
	int ret;

	/* various socket state tests */
	if (!(jsk->state & JSK_BOUND))
		return -EBADFD;

	ifindex = jsk->ifindex_started;
	if (!ifindex)
		return -EBADFD;

	if (jsk->addr.sa == J1939_NO_ADDR && !jsk->addr.src)
		/* no address assigned yet */
		return -EBADFD;

	/* deal with provided address info */
	if (msg->msg_name) {
		if (msg->msg_namelen < J1939_MIN_NAMELEN)
			return -EINVAL;
		if (addr->can_family != AF_CAN)
			return -EINVAL;
		if (addr->can_ifindex && (ifindex != addr->can_ifindex))
			return -EBADFD;
	}

	dev = dev_get_by_index(&init_net, ifindex);
	if (!dev)
		return -ENXIO;

	skb = sock_alloc_send_skb(sk,
				  size +
				  sizeof(struct can_frame) -
				  sizeof(((struct can_frame *)NULL)->data) +
				  sizeof(struct can_skb_priv),
				  msg->msg_flags & MSG_DONTWAIT, &ret);
	if (!skb)
		goto put_dev;

	can_skb_reserve(skb);
	can_skb_prv(skb)->ifindex = dev->ifindex;
	skb_reserve(skb, offsetof(struct can_frame, data));

	ret = memcpy_from_msg(skb_put(skb, size), msg, size);
	if (ret < 0)
		goto free_skb;
	sock_tx_timestamp(sk, skb->sk->sk_tsflags, &skb_shinfo(skb)->tx_flags);

	skb->dev = dev;

	skcb = (void *)skb->cb;
	memset(skcb, 0, sizeof(*skcb));
	skcb->msg_flags = msg->msg_flags;
	skcb->srcname = jsk->addr.src;
	skcb->dstname = jsk->addr.dst;
	skcb->pgn = jsk->addr.pgn;
	skcb->priority = j1939_prio(jsk->sk.sk_priority);
	skcb->srcaddr = jsk->addr.sa;
	skcb->dstaddr = jsk->addr.da;

	if (msg->msg_name) {
		struct sockaddr_can *addr = msg->msg_name;

		if (addr->can_addr.j1939.name ||
		    (addr->can_addr.j1939.addr != J1939_NO_ADDR)) {
			skcb->dstname = addr->can_addr.j1939.name;
			skcb->dstaddr = addr->can_addr.j1939.addr;
		}
		if (pgn_is_valid(addr->can_addr.j1939.pgn))
			skcb->pgn = addr->can_addr.j1939.pgn;
	}
	if (!pgn_is_valid(skcb->pgn)) {
		ret = -EINVAL;
		goto free_skb;
	}

	if (skcb->msg_flags & J1939_MSG_SYNC) {
		if (skcb->msg_flags & MSG_DONTWAIT) {
			ret = j1939_sock_pending_add_first(&jsk->sk);
			if (ret > 0)
				ret = -EAGAIN;
		} else {
			ret = wait_event_interruptible(jsk->waitq,
						       j1939_sock_pending_add_first(&jsk->sk));
		}
		if (ret < 0)
			goto free_skb;
	} else {
		j1939_sock_pending_add(&jsk->sk);
	}

	ret = j1939_send(skb);
	if (ret < 0)
		j1939_sock_pending_del(&jsk->sk);

	dev_put(dev);
	return (ret < 0) ? ret : size;

 free_skb:
	kfree_skb(skb);
 put_dev:
	dev_put(dev);
	return ret;
}

void j1939sk_netdev_event(int ifindex, int error_code)
{
	struct j1939_sock *jsk;

	spin_lock_bh(&j1939_socks_lock);
	list_for_each_entry(jsk, &j1939_socks, list) {
		if (jsk->sk.sk_bound_dev_if != ifindex)
			continue;
		jsk->sk.sk_err = error_code;
		if (!sock_flag(&jsk->sk, SOCK_DEAD))
			jsk->sk.sk_error_report(&jsk->sk);
		/* do not remove filters here */
	}
	spin_unlock_bh(&j1939_socks_lock);
}

static const struct proto_ops j1939_ops = {
	.family = PF_CAN,
	.release = j1939sk_release,
	.bind = j1939sk_bind,
	.connect = j1939sk_connect,
	.socketpair = sock_no_socketpair,
	.accept = sock_no_accept,
	.getname = j1939sk_getname,
	.poll = datagram_poll,
	.ioctl = can_ioctl,
	.listen = sock_no_listen,
	.shutdown = sock_no_shutdown,
	.setsockopt = j1939sk_setsockopt,
	.getsockopt = j1939sk_getsockopt,
	.sendmsg = j1939sk_sendmsg,
	.recvmsg = j1939sk_recvmsg,
	.mmap = sock_no_mmap,
	.sendpage = sock_no_sendpage,
};

static struct proto j1939_proto __read_mostly = {
	.name = "CAN_J1939",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct j1939_sock),
	.init = j1939sk_init,
};

const struct can_proto j1939_can_proto = {
	.type = SOCK_DGRAM,
	.protocol = CAN_J1939,
	.ops = &j1939_ops,
	.prot = &j1939_proto,
};
