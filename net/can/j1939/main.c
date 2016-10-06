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

/*
 * Core of can-j1939 that links j1939 to CAN.
 */

#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/socket.h>
#include <linux/list.h>
#include <linux/if_arp.h>
#include <net/tcp_states.h>

#include <linux/can.h>
#include <linux/can/core.h>
#include "j1939-priv.h"

MODULE_DESCRIPTION("PF_CAN SAE J1939");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("EIA Electronics (Kurt Van Dijck & Pieter Beyens)");
MODULE_ALIAS("can-proto-" __stringify(CAN_J1939));

const char j1939_procname[] = "can-j1939";
struct proc_dir_entry *j1939_procdir;

/* LOWLEVEL CAN interface */

/* CAN_HDR: #bytes before can_frame data part */
#define CAN_HDR	(offsetof(struct can_frame, data))
/* CAN_FTR: #bytes beyond data part */
#define CAN_FTR	(sizeof(struct can_frame)-CAN_HDR-\
		sizeof(((struct can_frame *)0)->data))

static unsigned int padding = 0;

module_param_named(padding, padding, uint, 0644);

MODULE_PARM_DESC(padding, "Pad all packets to 8 bytes, and stuff with 0xff");

/* lowest layer */
static void j1939_can_recv(struct sk_buff *iskb, void *data)
{
	struct j1939_priv *priv = data;
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *skcb;
	struct can_frame *cf;
	struct addr_ent *paddr;

	BUILD_BUG_ON(sizeof(*skcb) > sizeof(skb->cb));

	if (!priv)
		return;

	/* create a copy of the skb
	 * j1939 only delivers the real data bytes,
	 * the header goes into sockaddr.
	 * j1939 may not touch the incoming skb in such way
	 */
	skb = skb_clone(iskb, GFP_ATOMIC);
	/*
	 * get a pointer to the header of the skb
	 * the skb payload (pointer) is moved, so that the next skb_data
	 * returns the actual payload
	 */
	cf = (void *)skb->data;
	skb_pull(skb, CAN_HDR);
	/* fix length, set to dlc, with 8 maximum */
	skb_trim(skb, min_t(uint8_t, cf->can_dlc, 8));

	/* set addr */
	skcb = (struct j1939_sk_buff_cb *)skb->cb;
	memset(skcb, 0, sizeof(*skcb));
	/* save incoming socket, without assigning the skb to it */
	skcb->insock = iskb->sk;
	skcb->priority = (cf->can_id & 0x1c000000) >> 26;
	skcb->srcaddr = cf->can_id & 0xff;
	skcb->pgn = (cf->can_id & 0x3ffff00) >> 8;
	if (pgn_is_pdu1(skcb->pgn)) {
		/* Type 1: with destination address */
		skcb->dstaddr = skcb->pgn & 0xff;
		/* normalize pgn: strip dst address */
		skcb->pgn &= 0x3ff00;
	} else {
		/* set broadcast address */
		skcb->dstaddr = J1939_NO_ADDR;
	}

	/* update local rxtime cache */
	write_lock_bh(&priv->lock);
	if (j1939_address_is_unicast(skcb->srcaddr)) {
		paddr = &priv->ents[skcb->srcaddr];
		paddr->rxtime = ktime_get();
		if (paddr->ecu && skcb->pgn != 0x0ee00)
			paddr->ecu->rxtime = paddr->rxtime;
	}
	write_unlock_bh(&priv->lock);

	/* update localflags */
	read_lock_bh(&priv->lock);
	if (j1939_address_is_unicast(skcb->srcaddr) &&
			priv->ents[skcb->srcaddr].nusers)
		skcb->srcflags |= ECU_LOCAL;
	if (j1939_address_is_valid(skcb->dstaddr) ||
			(j1939_address_is_unicast(skcb->dstaddr) &&
				priv->ents[skcb->dstaddr].nusers))
		skcb->dstflags |= ECU_LOCAL;
	read_unlock_bh(&priv->lock);

	/* deliver into the j1939 stack ... */
	j1939_recv_address_claim(skb, priv);

	if (j1939_recv_transport(skb))
		/* this means the transport layer processed the message */
		goto done;
	j1939_recv(skb);
done:
	kfree_skb(skb);
}

int j1939_send(struct sk_buff *skb)
{
	int ret, dlc;
	canid_t canid;
	struct j1939_sk_buff_cb *skcb = (struct j1939_sk_buff_cb *)skb->cb;
	struct can_frame *cf;

	if (skb->len > 8)
		/* re-route via transport protocol */
		return j1939_send_transport(skb);

	/* apply sanity checks */
	skcb->pgn &= (pgn_is_pdu1(skcb->pgn)) ? 0x3ff00 : 0x3ffff;
	if (skcb->priority > 7)
		skcb->priority = 6;

	ret = j1939_fixup_address_claim(skb);
	if (unlikely(ret))
		goto failed;
	dlc = skb->len;
	if (dlc > 8) {
		ret = -EMSGSIZE;
		goto failed;
	}

	/* re-claim the CAN_HDR from the SKB */
	cf = (void *)skb_push(skb, CAN_HDR);
	BUG_ON(!cf);
	/* make it a full can frame again */
	skb_put(skb, CAN_FTR + (8 - dlc));

	canid = CAN_EFF_FLAG |
		(skcb->srcaddr & 0xff) |
		((skcb->priority & 0x7) << 26);
	if (pgn_is_pdu1(skcb->pgn))
		canid |= ((skcb->pgn & 0x3ff00) << 8) |
			((skcb->dstaddr & 0xff) << 8);
	else
		canid |= ((skcb->pgn & 0x3ffff) << 8);

	cf->can_id = canid;
	if (padding) {
		memset(cf->data + dlc, 0xff, 8 - dlc);
		cf->can_dlc = 8;
	} else
		cf->can_dlc = dlc;

	return can_send(skb, 1);
failed:
	consume_skb(skb);
	return ret;
}
EXPORT_SYMBOL_GPL(j1939_send);

/*
 * iterate over ECU's,
 * and register flagged ecu's on their claimed SA
 */
static void j1939_priv_ac_task(unsigned long val)
{
	struct j1939_priv *priv = (void *)val;
	struct j1939_ecu *ecu;

	write_lock_bh(&priv->lock);
	list_for_each_entry(ecu, &priv->ecus, list) {
		/* next 2 (read & set) could be merged into xxx? */
		if (!atomic_read(&ecu->ac_delay_expired))
			continue;
		atomic_set(&ecu->ac_delay_expired, 0);
		if (j1939_address_is_unicast(ecu->sa)) {
			ecu->priv->ents[ecu->sa].ecu = ecu;
			ecu->priv->ents[ecu->sa].nusers += ecu->nusers;
		}
	}
	write_unlock_bh(&priv->lock);
}

/* NETDEV MANAGEMENT */

/* values for can_rx_(un)register */
#define J1939_CAN_ID	CAN_EFF_FLAG
#define J1939_CAN_MASK	(CAN_EFF_FLAG | CAN_RTR_FLAG)

static DEFINE_MUTEX(j1939_netdev_lock);

int j1939_netdev_start(struct net_device *netdev)
{
	int ret;
	struct j1939_priv *priv;
	struct dev_rcv_lists *can_ml_priv;

	BUG_ON(!netdev);
	if (netdev->type != ARPHRD_CAN)
		return -EAFNOSUPPORT;

	mutex_lock(&j1939_netdev_lock);
	can_ml_priv = netdev->ml_priv;
	priv = can_ml_priv->j1939_priv;
	if (priv) {
		++priv->nusers;
		goto done;
	}

	/* create/stuff j1939_priv */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto fail_mem;
	}
	tasklet_init(&priv->ac_task, j1939_priv_ac_task, (unsigned long)priv);
	rwlock_init(&priv->lock);
	INIT_LIST_HEAD(&priv->ecus);
	priv->netdev = netdev;
	priv->ifindex = netdev->ifindex;
	kref_init(&priv->kref);
	priv->nusers = 1;

	/* add CAN handler */
	ret = can_rx_register(netdev, J1939_CAN_ID, J1939_CAN_MASK,
			j1939_can_recv, priv, "j1939");
	if (ret < 0)
		goto fail_can;

	can_ml_priv->j1939_priv = priv;
	dev_hold(netdev);
done:
	mutex_unlock(&j1939_netdev_lock);
	return 0;

fail_can:
	kfree(priv);
fail_mem:
	mutex_unlock(&j1939_netdev_lock);
	return ret;
}

void j1939_netdev_stop(struct net_device *netdev)
{
	struct dev_rcv_lists *can_ml_priv;
	struct j1939_priv *priv;

	BUG_ON(!netdev);
	if (netdev->type != ARPHRD_CAN)
		return;
	can_ml_priv = netdev->ml_priv;

	mutex_lock(&j1939_netdev_lock);
	priv = can_ml_priv->j1939_priv;
	--priv->nusers;
	if (priv->nusers) {
		mutex_unlock(&j1939_netdev_lock);
		return;
	}
	/* no users left, start breakdown */

	/* unlink from netdev */
	can_ml_priv->j1939_priv = NULL;
	mutex_unlock(&j1939_netdev_lock);

	can_rx_unregister(netdev, J1939_CAN_ID, J1939_CAN_MASK,
			j1939_can_recv, priv);

	/* remove pending transport protocol sessions */
	j1939tp_rmdev_notifier(netdev);

	/* final put */
	put_j1939_priv(priv);
	dev_put(netdev);
}

/*
 * device interface
 */
static void on_put_j1939_priv(struct kref *kref)
{
	struct j1939_priv *priv = container_of(kref, struct j1939_priv, kref);
	struct j1939_ecu *ecu;

	tasklet_disable_nosync(&priv->ac_task);

	/* cleanup priv */
	write_lock_bh(&priv->lock);
	while (!list_empty(&priv->ecus)) {
		ecu = list_first_entry(&priv->ecus, struct j1939_ecu, list);
		_j1939_ecu_unregister(ecu);
	}
	write_unlock_bh(&priv->lock);
	kfree(priv);
}

void put_j1939_priv(struct j1939_priv *segment)
{
	kref_put(&segment->kref, on_put_j1939_priv);
}

static int j1939_netdev_notify(struct notifier_block *nb,
			unsigned long msg, void *data)
{
	struct net_device *netdev = (struct net_device *)data;

	if (!net_eq(dev_net(netdev), &init_net))
		return NOTIFY_DONE;

	if (netdev->type != ARPHRD_CAN)
		return NOTIFY_DONE;

	switch (msg) {
	case NETDEV_UNREGISTER:
		j1939tp_rmdev_notifier(netdev);
		j1939sk_netdev_event(netdev->ifindex, ENODEV);
		break;

	case NETDEV_DOWN:
		j1939sk_netdev_event(netdev->ifindex, ENETDOWN);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block j1939_netdev_notifier = {
	.notifier_call = j1939_netdev_notify,
};

/* proc access to the addr+name database */
static int j1939_proc_show_addr(struct seq_file *sqf, void *v)
{
	struct net_device *netdev;
	struct j1939_priv *priv;
	int j;

	seq_printf(sqf, "iface\tsa\t#users\n");
	rcu_read_lock();
	for_each_netdev_rcu(&init_net, netdev) {
		priv = dev_j1939_priv(netdev);
		if (!priv)
			continue;
		read_lock_bh(&priv->lock);
		for (j = 0; j < 0xfe; ++j) {
			if (!priv->ents[j].nusers)
				continue;
			seq_printf(sqf, "%s\t%02x\t%i\n",
					netdev->name, j, priv->ents[j].nusers);
		}
		read_unlock_bh(&priv->lock);
	}
	rcu_read_unlock();
	return 0;
}

static int j1939_proc_show_name(struct seq_file *sqf, void *v)
{
	struct net_device *netdev;
	struct j1939_priv *priv;
	struct j1939_ecu *ecu;

	seq_printf(sqf, "iface\tname\tsa\t#users\n");
	rcu_read_lock();
	for_each_netdev_rcu(&init_net, netdev) {
		priv = dev_j1939_priv(netdev);
		if (!priv)
			continue;
		read_lock_bh(&priv->lock);
		list_for_each_entry(ecu, &priv->ecus, list)
			seq_printf(sqf, "%s\t%016llx\t%02x%s\t%i\n",
					netdev->name, ecu->name, ecu->sa,
				(priv->ents[ecu->sa].ecu == ecu) ? "" : "?",
					ecu->nusers);
		read_unlock_bh(&priv->lock);
	}
	rcu_read_unlock();
	return 0;
}

static int j1939_proc_open_addr(struct inode *inode, struct file *file)
{
	return single_open(file, j1939_proc_show_addr, NULL);
}

static int j1939_proc_open_name(struct inode *inode, struct file *file)
{
	return single_open(file, j1939_proc_show_name, NULL);
}

static const struct file_operations j1939_proc_ops_addr = {
	.owner		= THIS_MODULE,
	.open		= j1939_proc_open_addr,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations j1939_proc_ops_name = {
	.owner		= THIS_MODULE,
	.open		= j1939_proc_open_name,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* MODULE interface */
static __init int j1939_module_init(void)
{
	int ret;

	pr_info("can: SAE J1939\n");

	/* create /proc/net/can directory */
	j1939_procdir = proc_mkdir(j1939_procname, init_net.proc_net);
	if (!j1939_procdir)
		return -EINVAL;

	register_netdevice_notifier(&j1939_netdev_notifier);

	ret = can_proto_register(&j1939_can_proto);
	if (ret < 0) {
		pr_err("can: registration of j1939 protocol failed\n");
		goto fail_sk;
	}
	ret = j1939tp_module_init();
	if (ret < 0)
		goto fail_tp;

	if (!proc_create("addr", 0444, j1939_procdir, &j1939_proc_ops_addr))
		goto fail_addr;
	if (!proc_create("name", 0444, j1939_procdir, &j1939_proc_ops_name))
		goto fail_name;
	return 0;

	remove_proc_entry("name", j1939_procdir);
fail_name:
	remove_proc_entry("addr", j1939_procdir);
fail_addr:
	j1939tp_module_exit();
fail_tp:
	can_proto_unregister(&j1939_can_proto);
fail_sk:
	unregister_netdevice_notifier(&j1939_netdev_notifier);
	proc_remove(j1939_procdir);
	j1939_procdir = NULL;
	return ret;
}

static __exit void j1939_module_exit(void)
{
	remove_proc_entry("name", j1939_procdir);
	remove_proc_entry("addr", j1939_procdir);
	j1939tp_module_exit();

	can_proto_unregister(&j1939_can_proto);

	unregister_netdevice_notifier(&j1939_netdev_notifier);

	proc_remove(j1939_procdir);
	j1939_procdir = NULL;
}

module_init(j1939_module_init);
module_exit(j1939_module_exit);
