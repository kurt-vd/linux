/*
 * j1939-priv.h
 *
 * Copyright (c) 2010-2011 EIA Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _J1939_PRIV_H_
#define _J1939_PRIV_H_

#include <linux/kref.h>
#include <linux/list.h>
#include <net/sock.h>

#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/can/j1939.h>
#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/if_arp.h>

#include "../af_can.h"

/* TODO: return ENETRESET on busoff. */

#define PGN_REQUEST		0x0ea00
#define PGN_ADDRESS_CLAIMED	0x0ee00
#define PGN_MAX			0x3ffff

#define SA_MAX_UNICAST	0xfd
/*
 * j1939 devices
 */
struct j1939_ecu {
	struct list_head list;
	ktime_t rxtime;
	name_t name;
	uint8_t sa;
	/*
	 * atomic flag, set by ac_timer
	 * cleared/processed by segment's tasklet
	 * indicates that this ecu successfully claimed @sa as its address
	 * By communicating this from the ac_timer event to segments tasklet,
	 * a context locking problem is solved. All other 'ecu readers'
	 * must only lock with _bh, not with _irq.
	 */
	atomic_t ac_delay_expired;
	struct hrtimer ac_timer;
	struct kref kref;
	struct j1939_priv *priv;
	/* count users, to help transport protocol decide for interaction */
	int nusers;
};
#define to_j1939_ecu(x) container_of((x), struct j1939_ecu, dev)

struct j1939_priv {
	struct list_head ecus; /*
	 * local list entry in priv
	 * These allow irq (& softirq) context lookups on j1939 devices
	 * This approach (seperate lists) is done as the other 2 alternatives
	 * are not easier or even wrong
	 * 1) using the pure kobject methods involves mutexes, which are not
	 *    allowed in irq context.
	 * 2) duplicating data structures would require a lot of synchronization
	 *    code
	 * usage:
	 */
	rwlock_t lock; /*
	 * segments need a lock to protect the above list
	 */
	int ifindex;
	struct net_device *netdev;
	struct addr_ent {
		ktime_t rxtime;
		struct j1939_ecu *ecu;
		/* count users, to help transport protocol */
		int nusers;
	} ents[256];

	/*
	 * tasklet to process ecu address claimed events.
	 * These events raise in hardirq context. Signalling the event
	 * and scheduling this tasklet successfully moves the
	 * event to softirq context
	 */
	struct tasklet_struct ac_task;
	/*
	 * list of 256 ecu ptrs, that cache the claimed addresses.
	 * also protected by the above lock
	 * don't use directly, use j1939_ecu_set_address() instead
	 */
	struct kref kref;
	/*
	 * ref counter that hold the number of active listeners.
	 * This number itself is protected with a mutex
	 */
	int nusers;
};
#define to_j1939_priv(x) container_of((x), struct j1939_priv, dev)

extern void put_j1939_ecu(struct j1939_ecu *ecu);
extern void put_j1939_priv(struct j1939_priv *segment);
static inline void get_j1939_ecu(struct j1939_ecu *dut)
{
	kref_get(&dut->kref);
}
static inline void get_j1939_priv(struct j1939_priv *dut)
{
	kref_get(&dut->kref);
}

/* keep the cache of what is local */
extern void j1939_addr_local_get(struct j1939_priv *priv, int sa);
extern void j1939_addr_local_put(struct j1939_priv *priv, int sa);
extern void j1939_name_local_get(struct j1939_priv *priv, uint64_t name);
extern void j1939_name_local_put(struct j1939_priv *priv, uint64_t name);

/*
 * conversion function between (struct sock | struct sk_buff)->sk_priority
 * from linux and j1939 priority field
 */
static inline int j1939_prio(int sk_priority)
{
	if (sk_priority < 0)
		return 6; /* default */
	else if (sk_priority > 7)
		return 0;
	else
		return 7 - sk_priority;
}
static inline int j1939_to_sk_priority(int j1939_prio)
{
	return 7 - j1939_prio;
}

static inline int j1939_address_is_valid(uint8_t sa)
{
	return sa != J1939_NO_ADDR;
}

static inline int j1939_address_is_unicast(uint8_t sa)
{
	return sa <= SA_MAX_UNICAST;
}

static inline int pgn_is_pdu1(pgn_t pgn)
{
	/* ignore dp & res bits for this */
	return (pgn & 0xff00) < 0xf000;
}

static inline int pgn_is_valid(pgn_t pgn)
{
	return pgn <= PGN_MAX;
}

/* utility to correctly unregister a SA */
static inline void _j1939_ecu_remove_sa(struct j1939_ecu *ecu)
{
	if (!j1939_address_is_unicast(ecu->sa))
		return;
	if (ecu->priv && ecu->priv->ents[ecu->sa].ecu == ecu) {
		ecu->priv->ents[ecu->sa].ecu = NULL;
		ecu->priv->ents[ecu->sa].nusers -= ecu->nusers;
	}
}

static inline void j1939_ecu_remove_sa(struct j1939_ecu *ecu)
{
	if (!j1939_address_is_unicast(ecu->sa))
		return;
	write_lock_bh(&ecu->priv->lock);
	_j1939_ecu_remove_sa(ecu);
	write_unlock_bh(&ecu->priv->lock);
}

extern int j1939_name_to_sa(uint64_t name, int ifindex);
extern struct j1939_ecu *j1939_ecu_find_by_addr(int sa, int ifindex);
extern struct j1939_ecu *j1939_ecu_find_by_name(name_t name, int ifindex);
/* find_by_name, with kref & read_lock taken */
extern struct j1939_ecu *j1939_ecu_find_priv_default_tx(
		int ifindex, name_t *pname, uint8_t *paddr);

extern struct proc_dir_entry *j1939_procdir;
extern const char j1939_procname[];

/* j1939 printk */
#define j1939_printk(level, ...) printk(level "J1939 " __VA_ARGS__)

#define j1939_err(...)		j1939_printk(KERN_ERR , __VA_ARGS__)
#define j1939_warning(...)	j1939_printk(KERN_WARNING , __VA_ARGS__)
#define j1939_notice(...)	j1939_printk(KERN_NOTICE , __VA_ARGS__)
#define j1939_info(...)		j1939_printk(KERN_INFO , __VA_ARGS__)
#ifdef DEBUG
#define j1939_debug(...)	j1939_printk(KERN_DEBUG , __VA_ARGS__)
#else
#define j1939_debug(...)
#endif

struct sk_buff;

/* control buffer of the sk_buff */
struct j1939_sk_buff_cb {
	pgn_t pgn;
	priority_t priority;
	uint8_t srcaddr;
	uint8_t dstaddr;
	name_t srcname;
	name_t dstname;

	/*
	 * Flags for quick lookups during skb processing
	 * These are set in the receive path only
	 */
	int srcflags, dstflags;
	#define ECU_LOCAL	1
	/* for tx, MSG_SYN will be used to sync on sockets */
	int msg_flags;
	/* j1939 clones incoming skb's.
	 * insock saves the incoming skb->sk
	 * to determine local generated packets
	 */
	struct sock *insock;
};

#define J1939_MSG_RESERVED	MSG_SYN
#define J1939_MSG_SYNC		MSG_SYN

static inline int j1939cb_is_broadcast(const struct j1939_sk_buff_cb *skcb)
{
	return (!skcb->dstname && (skcb->dstaddr >= 0xff));
}

extern int j1939_send(struct sk_buff *);
extern void j1939_recv(struct sk_buff *);

/* stack entries */
extern int j1939_send_transport(struct sk_buff *);
extern int j1939_recv_transport(struct sk_buff *);
extern int j1939_fixup_address_claim(struct sk_buff *);
extern void j1939_recv_address_claim(struct sk_buff *, struct j1939_priv *priv);

/* network management */
/*
 * j1939_ecu_get_register
 * 'create' & 'register' & 'get' new ecu
 * when a matching ecu already exists, then that is returned
 */
extern struct j1939_ecu *_j1939_ecu_get_register(struct j1939_priv *priv,
		name_t name, int create_if_necessary);
/* unregister must be called with lock held */
extern void _j1939_ecu_unregister(struct j1939_ecu *);

extern int j1939_netdev_start(struct net_device *);
extern void j1939_netdev_stop(struct net_device *);

static inline struct j1939_priv *dev_j1939_priv(struct net_device *dev)
{
	struct dev_rcv_lists *can_ml_priv;
	struct j1939_priv *priv;

	BUG_ON(!dev);

	if (dev->type != ARPHRD_CAN)
		return NULL;

	can_ml_priv = dev->ml_priv;
	priv = can_ml_priv ? can_ml_priv->j1939_priv : NULL;
	if (priv)
		get_j1939_priv(priv);
	return priv;
}

static inline struct j1939_priv *j1939_priv_find(int ifindex)
{
	struct j1939_priv *priv;
	struct net_device *netdev;

	netdev = dev_get_by_index(&init_net, ifindex);
	priv = dev_j1939_priv(netdev);
	if (netdev)
		dev_put(netdev);
	return priv;
}

/* notify/alert all j1939 sockets bound to ifindex */
extern void j1939sk_netdev_event(int ifindex, int error_code);
extern int j1939tp_rmdev_notifier(struct net_device *netdev);

/*
 * decrement pending skb for a j1939 socket
 */
extern void j1939_sock_pending_del(struct sock *sk);

/* seperate module-init/modules-exit's */
extern __init int j1939tp_module_init(void);

extern void j1939tp_module_exit(void);

/* CAN protocol */
extern const struct can_proto j1939_can_proto;

#endif /* _J1939_PRIV_H_ */
