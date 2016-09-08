/*
 * Copyright (c) 2010-2011 EIA Electronics
 *
 * Authors:
 * Pieter Beyens <pieter.beyens@eia.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 */

/* J1939 Address Claiming.
 * Address Claiming in the kernel
 * - keeps track of the AC states of ECU's,
 * - resolves NAME<=>SA taking into account the AC states of ECU's.
 *
 * All Address Claim msgs (including host-originated msg) are processed
 * at the receive path (a sent msg is always received again via CAN echo).
 * As such, the processing of AC msgs is done in the order on which msgs
 * are sent on the bus.
 *
 * This module doesn't send msgs itself (e.g. replies on Address Claims),
 * this is the responsibility of a user space application or daemon.
 */

#include <linux/skbuff.h>
#include <linux/byteorder/generic.h>

#include "j1939-priv.h"

#define CANDATA2NAME(data) le64_to_cpup((uint64_t *)data)

static inline int ac_msg_is_request_for_ac(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *skcb = (void *)skb->cb;
	int req_pgn;

	if ((skb->len < 3) || (skcb->pgn != PGN_REQUEST))
		return 0;

	req_pgn = skb->data[0] | (skb->data[1] << 8) | (skb->data[2] << 16);

	return req_pgn == PGN_ADDRESS_CLAIMED;
}

static int j1939_verify_outgoing_address_claim(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *skcb = (void *)skb->cb;

	if (skb->len != 8) {
		j1939_notice("tx address claim with dlc %i\n", skb->len);
		return -EPROTO;
	}

	if (skcb->srcname != CANDATA2NAME(skb->data)) {
		j1939_notice("tx address claim with different name\n");
		return -EPROTO;
	}

	if (skcb->srcaddr == J1939_NO_ADDR) {
		j1939_notice("tx address claim with broadcast sa\n");
		return -EPROTO;
	}

	/* ac must always be a broadcast */
	if (skcb->dstname || (skcb->dstaddr != J1939_NO_ADDR)) {
		j1939_notice("tx address claim with dest, not broadcast\n");
		return -EPROTO;
	}
	return 0;
}

int j1939_fixup_address_claim(struct sk_buff *skb)
{
	int ret, sa;
	struct j1939_sk_buff_cb *skcb = (void *)skb->cb;

	/* network mgmt: address claiming msgs */
	if (skcb->pgn == PGN_ADDRESS_CLAIMED) {
		struct j1939_ecu *ecu;

		ret = j1939_verify_outgoing_address_claim(skb);
		/* return both when failure & when successful */
		if (ret < 0)
			return ret;
		ecu = j1939_ecu_find_by_name(skcb->srcname,
					     skb->dev->ifindex);
		if (!ecu)
			return -ENODEV;

		if (ecu->sa != skcb->srcaddr)
			/* hold further traffic for ecu, remove from parent */
			j1939_ecu_remove_sa(ecu);
		put_j1939_ecu(ecu);
	} else if (skcb->srcname) {
		/* assign source address */
		sa = j1939_name_to_sa(skcb->srcname, skb->dev->ifindex);
		if (!j1939_address_is_unicast(sa) &&
		    !ac_msg_is_request_for_ac(skb)) {
			j1939_notice("tx drop: invalid sa for name 0x%016llx\n",
				     skcb->srcname);
			return -EADDRNOTAVAIL;
		}
		skcb->srcaddr = sa;
	}

	/* assign destination address */
	if (skcb->dstname) {
		sa = j1939_name_to_sa(skcb->dstname, skb->dev->ifindex);
		if (!j1939_address_is_unicast(sa)) {
			j1939_notice("tx drop: invalid da for name 0x%016llx\n",
				     skcb->dstname);
			return -EADDRNOTAVAIL;
		}
		skcb->dstaddr = sa;
	}
	return 0;
}

static void j1939_process_address_claim(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *skcb = (void *)skb->cb;
	struct j1939_ecu *ecu, *prev;
	struct j1939_priv *priv;
	name_t name;

	if (skb->len != 8) {
		j1939_notice("rx address claim with wrong dlc %i\n", skb->len);
		return;
	}

	name = CANDATA2NAME(skb->data);
	skcb->srcname = name;
	if (!name) {
		j1939_notice("rx address claim without name\n");
		return;
	}

	if (!j1939_address_is_valid(skcb->srcaddr)) {
		j1939_notice("rx address claim with broadcast sa\n");
		return;
	}

	priv = j1939_priv_find(skb->skb_iif);
	if (!priv)
		return;

	write_lock_bh(&priv->lock);

	ecu = _j1939_ecu_get_register(priv, name,
				      j1939_address_is_unicast(skcb->srcaddr));
	if (IS_ERR(ecu))
		goto done;

	if (skcb->srcaddr >= J1939_IDLE_ADDR) {
		_j1939_ecu_unregister(ecu);
		goto done;
	}

	/* save new SA */
	if (skcb->srcaddr != ecu->sa)
		_j1939_ecu_remove_sa(ecu);
	/* cancel pending (previous) address claim */
	hrtimer_try_to_cancel(&ecu->ac_timer);
	ecu->sa = skcb->srcaddr;

	prev = priv->ents[skcb->srcaddr].ecu;
	if (prev && prev != ecu) {
		if (ecu->name > prev->name) {
			_j1939_ecu_unregister(ecu);
			goto done;
		} else {
			/* kick prev */
			_j1939_ecu_unregister(prev);
		}
	}

	/* schedule timer in 250 msec to commit address change */
	hrtimer_start(&ecu->ac_timer, ktime_set(0, 250000000),
		      HRTIMER_MODE_REL);
	/* rxtime administration */
	ecu->rxtime = ktime_get();
 done:
	write_unlock_bh(&priv->lock);
	put_j1939_priv(priv);
}

void j1939_recv_address_claim(struct sk_buff *skb, struct j1939_priv *priv)
{
	struct j1939_sk_buff_cb *skcb = (void *)skb->cb;
	struct j1939_ecu *ecu;

	/* network mgmt */
	if (skcb->pgn == PGN_ADDRESS_CLAIMED) {
		j1939_process_address_claim(skb);
	} else if (j1939_address_is_unicast(skcb->srcaddr)) {
		ecu = j1939_ecu_find_by_addr(skcb->srcaddr, skb->skb_iif);
		if (ecu) {
			/* source administration */
			ecu->rxtime = ktime_get();
			skcb->srcname = ecu->name;
			put_j1939_ecu(ecu);
		}
	}

	/* assign destination stuff */
	ecu = j1939_ecu_find_by_addr(skcb->dstaddr, skb->skb_iif);
	if (ecu) {
		skcb->dstname = ecu->name;
		put_j1939_ecu(ecu);
	}
}
