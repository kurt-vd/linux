/*
 * Copyright (c) 2010-2011 EIA Electronics
 *
 * Authors:
 * Kurt Van Dijck <kurt.van.dijck@eia.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 */

/* bus for j1939 remote devices
 * Since rtnetlink, no real bus is used.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/workqueue.h>

#include "j1939-priv.h"

#define ecu_dbg(ecu, fmt, ...) \
	pr_debug("j1939-%i,%016llx,%02x: " fmt, (ecu)->priv->ifindex, \
		(ecu)->name, (ecu)->sa, ##__VA_ARGS__)
#define ecu_alert(ecu, fmt, ...) \
	pr_alert("j1939-%i,%016llx,%02x: " fmt, (ecu)->priv->ifindex, \
		(ecu)->name, (ecu)->sa, ##__VA_ARGS__)

/* ECU device interface */
static enum hrtimer_restart j1939_ecu_timer_handler(struct hrtimer *hrtimer)
{
	struct j1939_ecu *ecu =
		container_of(hrtimer, struct j1939_ecu, ac_timer);

	atomic_set(&ecu->ac_delay_expired, 1);
	tasklet_schedule(&ecu->priv->ac_task);
	return HRTIMER_NORESTART;
}

static void cb_put_j1939_ecu(struct kref *kref)
{
	struct j1939_ecu *ecu =container_of(kref, struct j1939_ecu, kref);

	kfree(ecu);
}
void put_j1939_ecu(struct j1939_ecu *ecu)
{
	kref_put(&ecu->kref, cb_put_j1939_ecu);
}

struct j1939_ecu *_j1939_ecu_get_register(struct j1939_priv *priv, name_t name,
		int create_if_necessary)
{
	struct j1939_ecu *ecu, *dut;

	/* find existing */
	/* test for existing name */
	list_for_each_entry(dut, &priv->ecus, list) {
		if (dut->name == name)
			return dut;
	}
	/* alloc */
	ecu = kzalloc(sizeof(*ecu), gfp_any());
	if (!ecu)
		/* should we look for an existing ecu */
		return ERR_PTR(-ENOMEM);
	kref_init(&ecu->kref);
	ecu->sa = J1939_IDLE_ADDR;
	ecu->name = name;

	hrtimer_init(&ecu->ac_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ecu->ac_timer.function = j1939_ecu_timer_handler;
	INIT_LIST_HEAD(&ecu->list);

	/* first add to internal list */
	/* a ref to priv is held */
	ecu->priv = priv;
	list_add_tail(&ecu->list, &priv->ecus);

	ecu_dbg(ecu, "register\n");
	/* do not put_j1939_priv, a new ECU keeps a refcnt open */
	return ecu;
}

void _j1939_ecu_unregister(struct j1939_ecu *ecu)
{
	BUG_ON(!ecu);
	ecu_dbg(ecu, "unregister\n");
	hrtimer_try_to_cancel(&ecu->ac_timer);

	_j1939_ecu_remove_sa(ecu);
	list_del_init(&ecu->list);
	put_j1939_ecu(ecu);
}

struct j1939_ecu *j1939_ecu_find_by_addr(int sa, int ifindex)
{
	struct j1939_ecu *ecu;
	struct j1939_priv *priv;

	if (!j1939_address_is_unicast(sa))
		return NULL;
	priv = j1939_priv_find(ifindex);
	if (!priv)
		return NULL;
	read_lock_bh(&priv->lock);
	ecu = priv->ents[sa].ecu;
	if (ecu)
		get_j1939_ecu(ecu);
	read_unlock_bh(&priv->lock);
	put_j1939_priv(priv);
	return ecu;
}

int j1939_name_to_sa(u64 name, int ifindex)
{
	struct j1939_ecu *ecu;
	struct j1939_priv *priv;
	int sa;

	if (!name)
		return J1939_NO_ADDR;
	priv = j1939_priv_find(ifindex);
	if (!priv)
		return J1939_NO_ADDR;

	sa = J1939_IDLE_ADDR;
	read_lock_bh(&priv->lock);
	list_for_each_entry(ecu, &priv->ecus, list) {
		if (ecu->name == name) {
			if (priv->ents[ecu->sa].ecu == ecu)
				/* ecu's SA is registered */
				sa = ecu->sa;
			break;
		}
	}
	read_unlock_bh(&priv->lock);
	put_j1939_priv(priv);
	return sa;
}

/* ecu lookup helper */
static struct j1939_ecu *_j1939_ecu_find_by_name(name_t name,
		struct j1939_priv *priv)
{
	struct j1939_ecu *ecu;

	read_lock_bh(&priv->lock);
	list_for_each_entry(ecu, &priv->ecus, list) {
		if (ecu->name == name) {
			get_j1939_ecu(ecu);
			goto found_on_intf;
		}
	}
	ecu = NULL;
found_on_intf:
	read_unlock_bh(&priv->lock);
	return ecu;
}

/* ecu lookup by name */
struct j1939_ecu *j1939_ecu_find_by_name(name_t name, int ifindex)
{
	struct j1939_ecu *ecu;
	struct j1939_priv *priv;

	if (!name)
		return NULL;
	if (!ifindex)
		return NULL;
	priv = j1939_priv_find(ifindex);
	if (!priv)
		return NULL;
	ecu = _j1939_ecu_find_by_name(name, priv);
	put_j1939_priv(priv);
	return ecu;
}

/* TX addr/name accounting
 * Transport protocol needs to know if a SA is local or not
 * These functions originate from userspace manipulating sockets,
 * so locking is straigforward
 */
void j1939_addr_local_get(struct j1939_priv *priv, int sa)
{
	if (!j1939_address_is_unicast(sa))
		return;
	write_lock_bh(&priv->lock);
	++priv->ents[sa].nusers;
	write_unlock_bh(&priv->lock);
}

void j1939_addr_local_put(struct j1939_priv *priv, int sa)
{
	if (!j1939_address_is_unicast(sa))
		return;
	write_lock_bh(&priv->lock);
	--priv->ents[sa].nusers;
	write_unlock_bh(&priv->lock);
}

void j1939_name_local_get(struct j1939_priv *priv, u64 name)
{
	struct j1939_ecu *ecu;

	if (!name)
		return;
	write_lock_bh(&priv->lock);
	ecu = _j1939_ecu_get_register(priv, name, 1);
	if (!IS_ERR(ecu)) {
		get_j1939_ecu(ecu);
		++ecu->nusers;
		if (priv->ents[ecu->sa].ecu == ecu)
			/* ecu's sa is active already */
			++priv->ents[ecu->sa].nusers;
	}
	write_unlock_bh(&priv->lock);
}

void j1939_name_local_put(struct j1939_priv *priv, u64 name)
{
	struct j1939_ecu *ecu;

	if (!name)
		return;
	write_lock_bh(&priv->lock);
	ecu = _j1939_ecu_get_register(priv, name, 0);
	if (!IS_ERR(ecu)) {
		--ecu->nusers;
		if (priv->ents[ecu->sa].ecu == ecu)
			/* ecu's sa is active already */
			--priv->ents[ecu->sa].nusers;
		put_j1939_ecu(ecu);
	}
	write_unlock_bh(&priv->lock);
}

