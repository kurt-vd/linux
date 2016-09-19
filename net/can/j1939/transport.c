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

#include <linux/skbuff.h>
#include <linux/hrtimer.h>
#include <linux/version.h>
#include <linux/if_arp.h>
#include <linux/wait.h>
#include <linux/seq_file.h>
#include <linux/can/skb.h>
#include "j1939-priv.h"

#define REGULAR		0
#define EXTENDED	1

#define etp_pgn_ctl	0xc800
#define etp_pgn_dat	0xc700
#define tp_pgn_ctl	0xec00
#define tp_pgn_dat	0xeb00

#define  tp_cmd_bam	0x20
#define  tp_cmd_rts	0x10
#define  tp_cmd_cts	0x11
#define  tp_cmd_eof	0x13
#define  tp_cmd_abort	0xff

#define etp_cmd_rts	0x14
#define etp_cmd_cts	0x15
#define etp_cmd_dpo	0x16
#define etp_cmd_eof	0x17
#define etp_cmd_abort	0xff

#define ABORT_BUSY	1
#define ABORT_RESOURCE	2
#define ABORT_TIMEOUT	3
#define ABORT_GENERIC	4
#define ABORT_FAULT	5

#define MAX_TP_PACKET_SIZE	(7*255)
#define MAX_ETP_PACKET_SIZE	(7*0xffffff)

static unsigned int block = 255;
static unsigned int max_packet_size = 1024*100;
static unsigned int retry_ms = 20;
static unsigned int packet_delay;

module_param_named(transport_burst_count, block, uint, 0644);
module_param_named(transport_max_size, max_packet_size, uint, 0644);
module_param_named(transport_retry_time, retry_ms, uint, 0644);
module_param_named(transport_packet_delay, packet_delay, uint, 0644);

MODULE_PARM_DESC(transport_burst_count, "Number of packets to send in burst between flow control (1..255, default 255)");
MODULE_PARM_DESC(transport_max_size, "Maximum packet size (default 100k)");
MODULE_PARM_DESC(transport_retry_time, "Packet retransmission timeout in msecs, used in case of buffer full. (default 20)");
MODULE_PARM_DESC(transport_packet_delay, "Delay between packets to avoid buffer overruns (default 0)");

struct session {
	struct list_head list;
	atomic_t refs;
	spinlock_t lock;

	/* ifindex, src, dst, pgn define the session block
	 * the are _never_ modified after insertion in the list
	 * this decreases locking problems a _lot_
	 */
	struct j1939_sk_buff_cb *cb;
	struct sk_buff *skb;
	int skb_iif;

	/* all tx related stuff (last_txcmd, pkt.tx)
	 * is protected (modified only) with the txtask tasklet
	 * 'total' & 'block' are never changed,
	 * last_cmd, last & block are protected by ->lock
	 * this means that the tx may run after cts is received that should
	 * have stopped tx, but this time discrepancy is never avoided anyhow
	 */
	u8 last_cmd, last_txcmd;
	u8 transmission;
	u8 extd;
	struct {
		/* these do not require 16 bit, they should fit in u8
		 * but putting in int makes it easier to deal with
		 */
		unsigned int total, done, last, tx;
		unsigned int block; /* for TP */
		unsigned int dpo; /* for ETP */
	} pkt;
	struct hrtimer txtimer, rxtimer;

	/* tasklets for execution of tx/rx timer handler in softirq */
	struct tasklet_struct txtask, rxtask;
};

/* forward declarations */
static struct session *j1939session_new(struct sk_buff *skb);
static struct session *j1939session_fresh_new(int size,
		struct sk_buff *rel_skb, pgn_t pgn);
static void j1939tp_del_work(struct work_struct *work);

/* local variables */
static DEFINE_SPINLOCK(tp_lock);
static struct list_head tp_sessionq = LIST_HEAD_INIT(tp_sessionq);
static struct list_head tp_extsessionq = LIST_HEAD_INIT(tp_extsessionq);
static DEFINE_SPINLOCK(tp_dellock);
static struct list_head tp_delsessionq = LIST_HEAD_INIT(tp_delsessionq);
static DECLARE_WORK(tp_delwork, j1939tp_del_work);
static DECLARE_WAIT_QUEUE_HEAD(tp_wait);

/* helpers */
static inline void fix_cb(struct j1939_sk_buff_cb *cb)
{
	cb->msg_flags &= ~J1939_MSG_RESERVED;
}

static inline struct list_head *sessionq(int extd)
{
	return extd ? &tp_extsessionq : &tp_sessionq;
}

static inline void j1939session_destroy(struct session *session)
{
	if (session->skb)
		kfree_skb(session->skb);
	hrtimer_cancel(&session->rxtimer);
	hrtimer_cancel(&session->txtimer);
	tasklet_disable(&session->rxtask);
	tasklet_disable(&session->txtask);
	kfree(session);
}

/* clean up work queue */
static void j1939tp_del_work(struct work_struct *work)
{
	struct session *session;
	int cnt = 0;

	do {
		session = NULL;
		spin_lock_bh(&tp_dellock);
		if (list_empty(&tp_delsessionq)) {
			spin_unlock_bh(&tp_dellock);
			break;
		}
		session = list_first_entry(&tp_delsessionq,
				struct session, list);
		list_del_init(&session->list);
		spin_unlock_bh(&tp_dellock);
		j1939session_destroy(session);
		++cnt;
	} while (1);
}
/* reference counter */
static inline void get_session(struct session *session)
{
	atomic_inc(&session->refs);
}

static void put_session(struct session *session)
{
	BUG_ON(!session);
	if (atomic_add_return(-1, &session->refs) >= 0)
		/* not the last one */
		return;
	/* it should have been removed from any list long time ago */
	BUG_ON(!list_empty(&session->list));

	hrtimer_try_to_cancel(&session->rxtimer);
	hrtimer_try_to_cancel(&session->txtimer);
	tasklet_disable_nosync(&session->rxtask);
	tasklet_disable_nosync(&session->txtask);

	if (in_interrupt()) {
		spin_lock_bh(&tp_dellock);
		list_add_tail(&session->list, &tp_delsessionq);
		spin_unlock_bh(&tp_dellock);
		schedule_work(&tp_delwork);
	} else {
		/* destroy session right here */
		j1939session_destroy(session);
	}
}

/* transport status locking */
static inline void session_lock(struct session *session)
{
	get_session(session); /* safety measure */
	spin_lock_bh(&session->lock);
}

static inline void session_unlock(struct session *session)
{
	spin_unlock_bh(&session->lock);
	put_session(session);
}

static inline void sessionlist_lock(void)
{
	spin_lock_bh(&tp_lock);
}

static inline void sessionlist_unlock(void)
{
	spin_unlock_bh(&tp_lock);
}

/* see if we are receiver
 * returns 0 for broadcasts, although we will receive them
 */
static inline int j1939tp_im_receiver(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;

	return cb->dstflags & ECU_LOCAL;
}

/* see if we are sender */
static inline int j1939tp_im_transmitter(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;

	return cb->srcflags & ECU_LOCAL;
}

/* see if we are involved as either receiver or transmitter */
static int j1939tp_im_involved(struct sk_buff *skb, int swap)
{
	return swap ? j1939tp_im_receiver(skb) : j1939tp_im_transmitter(skb);
}

static int j1939tp_im_involved_anydir(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;

	return (cb->srcflags | cb->dstflags) & ECU_LOCAL;
}

/* extract pgn from flow-ctl message */
static inline pgn_t j1939xtp_ctl_to_pgn(const u8 *dat)
{
	pgn_t pgn;

	pgn = (dat[7] << 16) | (dat[6] << 8) | (dat[5] << 0);
	if (pgn_is_pdu1(pgn))
		pgn &= 0xffff00;
	return pgn;
}

static inline unsigned int j1939tp_ctl_to_size(const u8 *dat)
{
	return (dat[2] << 8) + (dat[1] << 0);
}

static inline unsigned int j1939etp_ctl_to_packet(const u8 *dat)
{
	return (dat[4] << 16) | (dat[3] << 8) | (dat[2] << 0);
}

static inline unsigned int j1939etp_ctl_to_size(const u8 *dat)
{
	return (dat[4] << 24) | (dat[3] << 16) |
		(dat[2] << 8) | (dat[1] << 0);
}

/* find existing session:
 * reverse: swap cb's src & dst
 * there is no problem with matching broadcasts, since
 * broadcasts (no dst, no da) would never call this
 * with reverse==1
 */
static int j1939tp_match(struct session *session, struct sk_buff *skb,
		int reverse)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;

	if (session->skb_iif != skb->skb_iif)
		return 0;
	if (!reverse) {
		if (session->cb->srcname) {
			if (session->cb->srcname != cb->srcname)
				return 0;
		} else if (session->cb->srcaddr != cb->srcaddr)
			return 0;
		if (session->cb->dstname) {
			if (session->cb->dstname != cb->dstname)
				return 0;
		} else if (session->cb->dstaddr != cb->dstaddr)
			return 0;
	} else {
		if (session->cb->srcname) {
			if (session->cb->srcname != cb->dstname)
				return 0;
		} else if (session->cb->srcaddr != cb->dstaddr)
			return 0;
		if (session->cb->dstname) {
			if (session->cb->dstname != cb->srcname)
				return 0;
		} else if (session->cb->dstaddr != cb->srcaddr)
			return 0;
	}
	return 1;
}

static struct session *_j1939tp_find(struct list_head *root,
		struct sk_buff *skb, int reverse)
{
	struct session *session;

	list_for_each_entry(session, root, list) {
		get_session(session);
		if (j1939tp_match(session, skb, reverse))
			return session;
		put_session(session);
	}
	return NULL;
}

static struct session *j1939tp_find(struct list_head *root,
		struct sk_buff *skb, int reverse)
{
	struct session *session;
	sessionlist_lock();
	session = _j1939tp_find(root, skb, reverse);
	sessionlist_unlock();
	return session;
}

static void j1939_skbcb_swap(struct j1939_sk_buff_cb *cb)
{
	name_t name;
	u8 addr;
	int flags;

	name = cb->dstname;
	cb->dstname = cb->srcname;
	cb->srcname = name;

	addr = cb->dstaddr;
	cb->dstaddr = cb->srcaddr;
	cb->srcaddr = addr;

	flags = cb->dstflags;
	cb->dstflags = cb->srcflags;
	cb->srcflags = flags;
}
/* TP transmit packet functions */
static int j1939tp_tx_dat(struct sk_buff *related, int extd,
			  const u8 *dat, int len)
{
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *skb_cb;
	u8 *skdat;

	skb = alloc_skb(sizeof(struct can_frame) + sizeof(struct can_skb_priv),
			GFP_ATOMIC);
	if (unlikely(!skb)) {
		pr_alert("%s: out of memory?\n", __func__);
		return -ENOMEM;
	}
	can_skb_reserve(skb);
	can_skb_prv(skb)->ifindex = can_skb_prv(related)->ifindex;
	/* reserve CAN header */
	skb_reserve(skb, offsetof(struct can_frame, data));

	skb->dev = related->dev;
	skb->protocol = related->protocol;
	skb->pkt_type = related->pkt_type;
	skb->ip_summed = related->ip_summed;

	memcpy(skb->cb, related->cb, sizeof(skb->cb));
	skb_cb = (void *)skb->cb;
	fix_cb(skb_cb);
	/* fix pgn */
	skb_cb->pgn = extd ? etp_pgn_dat : tp_pgn_dat;

	skdat = skb_put(skb, len);
	memcpy(skdat, dat, len);
	return j1939_send(skb);
}

static int j1939xtp_do_tx_ctl(struct sk_buff *related, int extd,
			      int swap_src_dst, pgn_t pgn, const u8 dat[5])
{
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *skb_cb;
	u8 *skdat;

	if (!j1939tp_im_involved(related, swap_src_dst))
		return 0;

	skb = alloc_skb(sizeof(struct can_frame) + sizeof(struct can_skb_priv),
			GFP_ATOMIC);
	if (unlikely(!skb)) {
		pr_alert("%s: out of memory?\n", __func__);
		return -ENOMEM;
	}
	skb->dev = related->dev;
	can_skb_reserve(skb);
	can_skb_prv(skb)->ifindex = can_skb_prv(related)->ifindex;
	/* reserve CAN header */
	skb_reserve(skb, offsetof(struct can_frame, data));
	skb->protocol = related->protocol;
	skb->pkt_type = related->pkt_type;
	skb->ip_summed = related->ip_summed;

	memcpy(skb->cb, related->cb, sizeof(skb->cb));
	skb_cb = (void *)skb->cb;
	fix_cb(skb_cb);
	if (swap_src_dst)
		j1939_skbcb_swap(skb_cb);
	skb_cb->pgn = extd ? etp_pgn_ctl : tp_pgn_ctl;

	skdat = skb_put(skb, 8);
	memcpy(skdat, dat, 5);
	skdat[7] = (pgn >> 16) & 0xff;
	skdat[6] = (pgn >>  8) & 0xff;
	skdat[5] = (pgn >>  0) & 0xff;

	return j1939_send(skb);
}

static inline int j1939tp_tx_ctl(struct session *session,
				 int swap_src_dst, const u8 dat[8])
{
	return j1939xtp_do_tx_ctl(session->skb, session->extd, swap_src_dst,
			session->cb->pgn, dat);
}

static int j1939xtp_tx_abort(struct sk_buff *related, int extd,
		int swap_src_dst, int err, pgn_t pgn)
{
	u8 dat[5];

	if (!j1939tp_im_involved(related, swap_src_dst))
		return 0;

	memset(dat, 0xff, sizeof(dat));
	dat[0] = tp_cmd_abort;
	if (!extd)
		dat[1] = err ?: ABORT_GENERIC;
	return j1939xtp_do_tx_ctl(related, extd, swap_src_dst, pgn, dat);
}

/* timer & scheduler functions */
static inline void j1939session_schedule_txnow(struct session *session)
{
	tasklet_schedule(&session->txtask);
}
static enum hrtimer_restart j1939tp_txtimer(struct hrtimer *hrtimer)
{
	struct session *session =
		container_of(hrtimer, struct session, txtimer);
	j1939session_schedule_txnow(session);
	return HRTIMER_NORESTART;
}
static inline void j1939tp_schedule_txtimer(struct session *session, int msec)
{
	hrtimer_start(&session->txtimer,
			ktime_set(msec / 1000, (msec % 1000)*1000000UL),
			HRTIMER_MODE_REL);
}
static inline void j1939tp_set_rxtimeout(struct session *session, int msec)
{
	hrtimer_start(&session->rxtimer,
			ktime_set(msec / 1000, (msec % 1000)*1000000UL),
			HRTIMER_MODE_REL);
}

/* session completion functions */

/* j1939session_drop
 * removes a session from open session list
 */
static inline void j1939session_drop(struct session *session)
{
	sessionlist_lock();
	list_del_init(&session->list);
	sessionlist_unlock();

	if (session->transmission) {
		if (session->skb && session->skb->sk)
			j1939_sock_pending_del(session->skb->sk);
		wake_up_all(&tp_wait);
	}
	put_session(session);
}

static inline void j1939session_completed(struct session *session)
{
	/* distribute among j1939 receivers */
	j1939_recv(session->skb);
	j1939session_drop(session);
}

static void j1939session_cancel(struct session *session, int err)
{
	if ((err >= 0) && j1939tp_im_involved_anydir(session->skb)) {
		if (!j1939cb_is_broadcast(session->cb)) {
			/* do not send aborts on incoming broadcasts */
			j1939xtp_tx_abort(session->skb, session->extd,
				!(session->cb->srcflags & ECU_LOCAL),
				err, session->cb->pgn);
		}
	}
	j1939session_drop(session);
}

static enum hrtimer_restart j1939tp_rxtimer(struct hrtimer *hrtimer)
{
	struct session *session =
		container_of(hrtimer, struct session, rxtimer);
	tasklet_schedule(&session->rxtask);
	return HRTIMER_NORESTART;
}

static void j1939tp_rxtask(unsigned long val)
{
	struct session *session = (void *)val;

	get_session(session);
	pr_alert("%s: timeout on %i\n", __func__, session->skb_iif);
	j1939session_cancel(session, ABORT_TIMEOUT);
	put_session(session);
}

/* receive packet functions */
static void _j1939xtp_rx_bad_message(struct sk_buff *skb, int extd)
{
	struct session *session;
	pgn_t pgn;

	pgn = j1939xtp_ctl_to_pgn(skb->data);
	session = j1939tp_find(sessionq(extd), skb, 0);
	if (session /*&& (session->cb->pgn == pgn)*/) {
		/* do not allow TP control messages on 2 pgn's */
		j1939session_cancel(session, ABORT_FAULT);
		put_session(session); /* ~j1939tp_find */
		return;
	}
	j1939xtp_tx_abort(skb, extd, 0, ABORT_FAULT, pgn);
	if (!session)
		return;
	put_session(session); /* ~j1939tp_find */
}

/* abort packets may come in 2 directions */
static void j1939xtp_rx_bad_message(struct sk_buff *skb, int extd)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;

	pr_info("%s, pgn %05x\n", __func__, j1939xtp_ctl_to_pgn(skb->data));
	_j1939xtp_rx_bad_message(skb, extd);
	j1939_skbcb_swap(cb);
	_j1939xtp_rx_bad_message(skb, extd);
	/* restore skb */
	j1939_skbcb_swap(cb);
}

static void _j1939xtp_rx_abort(struct sk_buff *skb, int extd)
{
	struct session *session;
	pgn_t pgn;

	pgn = j1939xtp_ctl_to_pgn(skb->data);
	session = j1939tp_find(sessionq(extd), skb, 0);
	if (!session)
		return;
	if (session->transmission && !session->last_txcmd) {
		/* empty block:
		 * do not drop session when a transmit session did not
		 * start yet
		 */
	} else if (session->cb->pgn == pgn)
		j1939session_drop(session);
	/* TODO: maybe cancel current connection
	 * as another pgn was communicated
	 */
	put_session(session); /* ~j1939tp_find */
}
/* abort packets may come in 2 directions */
static inline void j1939xtp_rx_abort(struct sk_buff *skb, int extd)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;

	pr_info("%s %i, %05x\n", __func__, skb->skb_iif,
			j1939xtp_ctl_to_pgn(skb->data));
	_j1939xtp_rx_abort(skb, extd);
	j1939_skbcb_swap(cb);
	_j1939xtp_rx_abort(skb, extd);
	/* restore skb */
	j1939_skbcb_swap(cb);
}

static void j1939xtp_rx_eof(struct sk_buff *skb, int extd)
{
	struct session *session;
	pgn_t pgn;

	/* end of tx cycle */
	pgn = j1939xtp_ctl_to_pgn(skb->data);
	session = j1939tp_find(sessionq(extd), skb, 1);
	if (!session)
		/* strange, we had EOF on closed connection
		 * do nothing, as EOF closes the connection anyway
		 */
		return;

	if (session->cb->pgn != pgn) {
		j1939xtp_tx_abort(skb, extd, 1, ABORT_BUSY, pgn);
		j1939session_cancel(session, ABORT_BUSY);
	} else {
		/* transmitted without problems */
		j1939session_completed(session);
	}
	put_session(session); /* ~j1939tp_find */
}

static void j1939xtp_rx_cts(struct sk_buff *skb, int extd)
{
	struct session *session;
	pgn_t pgn;
	unsigned int pkt;
	const u8 *dat;

	dat = skb->data;
	pgn = j1939xtp_ctl_to_pgn(skb->data);
	session = j1939tp_find(sessionq(extd), skb, 1);
	if (!session) {
		/* 'CTS shall be ignored' */
		return;
	}
	if (session->cb->pgn != pgn) {
		/* what to do? */
		j1939xtp_tx_abort(skb, extd, 1, ABORT_BUSY, pgn);
		j1939session_cancel(session, ABORT_BUSY);
		put_session(session); /* ~j1939tp_find */
		return;
	}
	session_lock(session);
	pkt = extd ? j1939etp_ctl_to_packet(dat) : dat[2];
	if (!dat[0])
		hrtimer_cancel(&session->txtimer);
	else if (!pkt)
		goto bad_fmt;
	else if (dat[1] > session->pkt.block /* 0xff for etp */)
		goto bad_fmt;
	else {
		/* set packet counters only when not CTS(0) */
		session->pkt.done = pkt - 1;
		session->pkt.last = session->pkt.done + dat[1];
		if (session->pkt.last > session->pkt.total)
			/* safety measure */
			session->pkt.last = session->pkt.total;
		/* TODO: do not set tx here, do it in txtask */
		session->pkt.tx = session->pkt.done;
	}
	session->last_cmd = dat[0];
	session_unlock(session);
	if (dat[1]) {
		j1939tp_set_rxtimeout(session, 1250);
		if (j1939tp_im_transmitter(session->skb))
			j1939session_schedule_txnow(session);
	} else {
		/* CTS(0) */
		j1939tp_set_rxtimeout(session, 550);
	}
	put_session(session); /* ~j1939tp_find */
	return;
bad_fmt:
	session_unlock(session);
	j1939session_cancel(session, ABORT_FAULT);
	put_session(session); /* ~j1939tp_find */
}

static void j1939xtp_rx_rts(struct sk_buff *skb, int extd)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;
	struct session *session;
	int len;
	const u8 *dat;
	pgn_t pgn;

	dat = skb->data;
	pgn = j1939xtp_ctl_to_pgn(dat);

	if ((tp_cmd_rts == dat[0]) && j1939cb_is_broadcast(cb)) {
		pr_alert("%s: rts without destination (%i %02x)\n", __func__,
			skb->skb_iif, cb->srcaddr);
		return;
	}

	/* TODO: abort RTS when a similar
	 * TP is pending in the other direction
	 */
	session = j1939tp_find(sessionq(extd), skb, 0);
	if (session && !j1939tp_im_transmitter(skb)) {
		/* RTS on pending connection */
		j1939session_cancel(session, ABORT_BUSY);
		if ((pgn != session->cb->pgn) && (tp_cmd_bam != dat[0]))
			j1939xtp_tx_abort(skb, extd, 1, ABORT_BUSY, pgn);
		put_session(session); /* ~j1939tp_find */
		return;
	} else if (!session && j1939tp_im_transmitter(skb)) {
		pr_alert("%s: I should tx (%i %02x %02x)\n", __func__,
			skb->skb_iif, cb->srcaddr, cb->dstaddr);
		return;
	}
	if (session && (session->last_cmd != 0)) {
		/* we received a second rts on the same connection */
		pr_alert("%s: connection exists (%i %02x %02x)\n", __func__,
				skb->skb_iif, cb->srcaddr, cb->dstaddr);
		j1939session_cancel(session, ABORT_BUSY);
		put_session(session); /* ~j1939tp_find */
		return;
	}
	if (session) {
		/* make sure 'sa' & 'da' are correct !
		 * They may be 'not filled in yet' for sending
		 * skb's, since they did not pass the Address Claim ever.
		 */
		session->cb->srcaddr = cb->srcaddr;
		session->cb->dstaddr = cb->dstaddr;
	} else {
		int abort = 0;
		if (extd) {
			len = j1939etp_ctl_to_size(dat);
			if (len > MAX_ETP_PACKET_SIZE)
				abort = ABORT_FAULT;
			else if (max_packet_size && (len > max_packet_size))
				abort = ABORT_RESOURCE;
			else if (len <= MAX_TP_PACKET_SIZE)
				abort = ABORT_FAULT;
		} else {
			len = j1939tp_ctl_to_size(dat);
			if (len > MAX_TP_PACKET_SIZE)
				abort = ABORT_FAULT;
			else if (max_packet_size && (len > max_packet_size))
				abort = ABORT_RESOURCE;
		}
		if (abort) {
			j1939xtp_tx_abort(skb, extd, 1, abort, pgn);
			return;
		}
		session = j1939session_fresh_new(len, skb, pgn);
		if (!session) {
			j1939xtp_tx_abort(skb, extd, 1, ABORT_RESOURCE, pgn);
			return;
		}
		session->extd = extd;
		/* initialize the control buffer: plain copy */
		session->pkt.total = (len+6)/7;
		session->pkt.block = 0xff;
		if (!extd) {
			if (dat[3] != session->pkt.total)
				pr_alert("%s: strange total,"
						" %u != %u\n", __func__,
						session->pkt.total, dat[3]);
			session->pkt.total = dat[3];
			session->pkt.block = dat[4];
		}
		session->pkt.done = 0;
		session->pkt.tx = 0;
		get_session(session); /* equivalent to j1939tp_find() */
		sessionlist_lock();
		list_add_tail(&session->list, sessionq(extd));
		sessionlist_unlock();
	}
	session->last_cmd = dat[0];

	j1939tp_set_rxtimeout(session, 1250);

	if (j1939tp_im_receiver(session->skb)) {
		if (extd || (tp_cmd_bam != dat[0]))
			j1939session_schedule_txnow(session);
	}

	/* as soon as it's inserted, things can go fast
	 * protect against a long delay
	 * between spin_unlock & next statement
	 * so, only release here, at the end
	 */
	put_session(session); /* ~j1939tp_find */
}

static void j1939xtp_rx_dpo(struct sk_buff *skb, int extd)
{
	struct session *session;
	pgn_t pgn;
	const u8 *dat = skb->data;

	pgn = j1939xtp_ctl_to_pgn(dat);
	session = j1939tp_find(sessionq(extd), skb, 0);
	if (!session) {
		pr_info("%s: %s\n", __func__, "no connection found");
		return;
	}

	if (session->cb->pgn != pgn) {
		pr_info("%s: different pgn\n", __func__);
		j1939xtp_tx_abort(skb, 1, 1, ABORT_BUSY, pgn);
		j1939session_cancel(session, ABORT_BUSY);
		put_session(session); /* ~j1939tp_find */
		return;
	}
	/* transmitted without problems */
	session->pkt.dpo = j1939etp_ctl_to_packet(skb->data);
	session->last_cmd = dat[0];
	j1939tp_set_rxtimeout(session, 750);
	put_session(session); /* ~j1939tp_find */
}

static void j1939xtp_rx_dat(struct sk_buff *skb, int extd)
{
	struct session *session;
	const u8 *dat;
	u8 *tpdat;
	int offset;
	int nbytes;
	int final;
	int do_cts_eof;
	int packet;

	session = j1939tp_find(sessionq(extd), skb, 0);
	if (!session) {
		pr_info("%s:%s\n", __func__, "no connection found");
		return;
	}
	dat = skb->data;
	if (skb->len <= 1)
		/* makes no sense */
		goto strange_packet_unlocked;

	session_lock(session);

	switch (session->last_cmd) {
	case 0xff:
		break;
	case etp_cmd_dpo:
		if (extd)
			break;
	case tp_cmd_bam:
	case tp_cmd_cts:
		if (!extd)
			break;
	default:
		pr_info("%s: last %02x\n", __func__,
				session->last_cmd);
		goto strange_packet;
	}

	packet = (dat[0]-1+session->pkt.dpo);
	offset = packet * 7;
	if ((packet > session->pkt.total) ||
			(session->pkt.done+1) > session->pkt.total) {
		pr_info("%s: should have been completed\n", __func__);
		goto strange_packet;
	}
	nbytes = session->skb->len - offset;
	if (nbytes > 7)
		nbytes = 7;
	if ((nbytes <= 0) || ((nbytes + 1) > skb->len)) {
		pr_info("%s: nbytes %i, len %i\n", __func__, nbytes,
				skb->len);
		goto strange_packet;
	}
	tpdat = session->skb->data;
	memcpy(&tpdat[offset], &dat[1], nbytes);
	if (packet == session->pkt.done)
		++session->pkt.done;

	if (!extd && j1939cb_is_broadcast(session->cb)) {
		final = session->pkt.done >= session->pkt.total;
		do_cts_eof = 0;
	} else {
		final = 0; /* never final, an EOF must follow */
		do_cts_eof = (session->pkt.done >= session->pkt.last);
	}
	session_unlock(session);
	if (final) {
		j1939session_completed(session);
	} else if (do_cts_eof) {
		j1939tp_set_rxtimeout(session, 1250);
		if (j1939tp_im_receiver(session->skb))
			j1939session_schedule_txnow(session);
	} else {
		j1939tp_set_rxtimeout(session, 250);
	}
	session->last_cmd = 0xff;
	put_session(session); /* ~j1939tp_find */
	return;

strange_packet:
	/* unlock session (spinlock) before trying to send */
	session_unlock(session);
strange_packet_unlocked:
	j1939session_cancel(session, ABORT_FAULT);
	put_session(session); /* ~j1939tp_find */
}

/* transmit function */
static int j1939tp_txnext(struct session *session)
{
	u8 dat[8];
	const u8 *tpdat;
	int ret, offset, pkt_done, pkt_end;
	unsigned int pkt, len, pdelay;

	memset(dat, 0xff, sizeof(dat));
	get_session(session); /* do not loose it */

	switch (session->last_cmd) {
	case 0:
		if (!j1939tp_im_transmitter(session->skb))
			break;
		dat[1] = (session->skb->len >> 0) & 0xff;
		dat[2] = (session->skb->len >> 8) & 0xff;
		dat[3] = session->pkt.total;
		if (session->extd) {
			dat[0] = etp_cmd_rts;
			dat[1] = (session->skb->len >>  0) & 0xff;
			dat[2] = (session->skb->len >>  8) & 0xff;
			dat[3] = (session->skb->len >> 16) & 0xff;
			dat[4] = (session->skb->len >> 24) & 0xff;
		} else if (j1939cb_is_broadcast(session->cb)) {
			dat[0] = tp_cmd_bam;
			/* fake cts for broadcast */
			session->pkt.tx = 0;
		} else {
			dat[0] = tp_cmd_rts;
			dat[4] = dat[3];
		}
		if (dat[0] == session->last_txcmd)
			/* done already */
			break;
		ret = j1939tp_tx_ctl(session, 0, dat);
		if (ret < 0)
			goto failed;
		session->last_txcmd = dat[0];
		/* must lock? */
		if (tp_cmd_bam == dat[0])
			j1939tp_schedule_txtimer(session, 50);
		j1939tp_set_rxtimeout(session, 1250);
		break;
	case tp_cmd_rts:
	case etp_cmd_rts:
		if (!j1939tp_im_receiver(session->skb))
			break;
tx_cts:
		ret = 0;
		len = session->pkt.total - session->pkt.done;
		len = min(max(len, session->pkt.block), block ?: 255);

		if (session->extd) {
			pkt = session->pkt.done+1;
			dat[0] = etp_cmd_cts;
			dat[1] = len;
			dat[2] = (pkt >>  0) & 0xff;
			dat[3] = (pkt >>  8) & 0xff;
			dat[4] = (pkt >> 16) & 0xff;
		} else {
			dat[0] = tp_cmd_cts;
			dat[1] = len;
			dat[2] = session->pkt.done+1;
		}
		if (dat[0] == session->last_txcmd)
			/* done already */
			break;
		ret = j1939tp_tx_ctl(session, 1, dat);
		if (ret < 0)
			goto failed;
		if (len)
			/* only mark cts done when len is set */
			session->last_txcmd = dat[0];
		j1939tp_set_rxtimeout(session, 1250);
		break;
	case etp_cmd_cts:
		if (j1939tp_im_transmitter(session->skb) && session->extd &&
		    (etp_cmd_dpo != session->last_txcmd)) {
			/* do dpo */
			dat[0] = etp_cmd_dpo;
			session->pkt.dpo = session->pkt.done;
			pkt = session->pkt.dpo;
			dat[1] = session->pkt.last - session->pkt.done;
			dat[2] = (pkt >>  0) & 0xff;
			dat[3] = (pkt >>  8) & 0xff;
			dat[4] = (pkt >> 16) & 0xff;
			ret = j1939tp_tx_ctl(session, 0, dat);
			if (ret < 0)
				goto failed;
			session->last_txcmd = dat[0];
			j1939tp_set_rxtimeout(session, 1250);
			session->pkt.tx = session->pkt.done;
		}
	case tp_cmd_cts:
	case 0xff: /* did some data */
	case etp_cmd_dpo:
		if ((session->extd || !j1939cb_is_broadcast(session->cb)) &&
		     j1939tp_im_receiver(session->skb)) {
			if (session->pkt.done >= session->pkt.total) {
				if (session->extd) {
					dat[0] = etp_cmd_eof;
					dat[1] = session->skb->len >> 0;
					dat[2] = session->skb->len >> 8;
					dat[3] = session->skb->len >> 16;
					dat[4] = session->skb->len >> 24;
				} else {
					dat[0] = tp_cmd_eof;
					dat[1] = session->skb->len;
					dat[2] = session->skb->len >> 8;
					dat[3] = session->pkt.total;
				}
				if (dat[0] == session->last_txcmd)
					/* done already */
					break;
				ret = j1939tp_tx_ctl(session, 1, dat);
				if (ret < 0)
					goto failed;
				session->last_txcmd = dat[0];
				j1939tp_set_rxtimeout(session, 1250);
				/* wait for the EOF packet to come in */
				break;
			} else if (session->pkt.done >= session->pkt.last) {
				session->last_txcmd = 0;
				goto tx_cts;
			}
		}
	case tp_cmd_bam:
		if (!j1939tp_im_transmitter(session->skb))
			break;
		tpdat = session->skb->data;
		ret = 0;
		pkt_done = 0;
		pkt_end = (!session->extd && j1939cb_is_broadcast(session->cb))
			? session->pkt.total : session->pkt.last;

		while (session->pkt.tx < pkt_end) {
			dat[0] = session->pkt.tx - session->pkt.dpo+1;
			offset = session->pkt.tx * 7;
			len = session->skb->len - offset;
			if (len > 7)
				len = 7;
			memcpy(&dat[1], &tpdat[offset], len);
			ret = j1939tp_tx_dat(session->skb, session->extd,
					dat, len+1);
			if (ret < 0)
				break;
			session->last_txcmd = 0xff;
			++pkt_done;
			++session->pkt.tx;
			pdelay = j1939cb_is_broadcast(session->cb) ?  50 :
				packet_delay;
			if ((session->pkt.tx < session->pkt.total) && pdelay) {
				j1939tp_schedule_txtimer(session, pdelay);
				break;
			}
		}
		if (pkt_done)
			j1939tp_set_rxtimeout(session, 250);
		if (ret)
			goto failed;
		break;
	}
	put_session(session);
	return 0;
failed:
	put_session(session);
	return ret;
}

static void j1939tp_txtask(unsigned long val)
{
	struct session *session = (void *)val;
	int ret;

	get_session(session);
	ret = j1939tp_txnext(session);
	if (ret < 0)
		j1939tp_schedule_txtimer(session, retry_ms ?: 20);
	put_session(session);
}

static inline int j1939tp_tx_initial(struct session *session)
{
	int ret;

	get_session(session);
	ret = j1939tp_txnext(session);
	/* set nonblocking for further packets */
	session->cb->msg_flags |= MSG_DONTWAIT;
	put_session(session);
	return ret;
}

/* this call is to be used as probe within wait_event_xxx() */
static int j1939session_insert(struct session *session)
{
	struct session *pending;

	sessionlist_lock();
	pending = _j1939tp_find(sessionq(session->extd), session->skb, 0);
	if (pending)
		/* revert the effect of find() */
		put_session(pending);
	else
		list_add_tail(&session->list, sessionq(session->extd));
	sessionlist_unlock();
	return pending ? 0 : 1;
}

/* j1939 main intf */
int j1939_send_transport(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;
	struct session *session;
	int ret;
	struct j1939_priv *priv;

	if ((tp_pgn_dat == cb->pgn) || (tp_pgn_ctl == cb->pgn) ||
	    (etp_pgn_dat == cb->pgn) || (etp_pgn_ctl == cb->pgn))
		/* avoid conflict */
		return -EDOM;
	else if ((skb->len > MAX_ETP_PACKET_SIZE) ||
			(max_packet_size && (skb->len > max_packet_size)))
		return -EMSGSIZE;

	if (skb->len > MAX_TP_PACKET_SIZE) {
		if (j1939cb_is_broadcast(cb))
			return -EDESTADDRREQ;
	}

	/* fill in addresses from names */
	ret = j1939_fixup_address_claim(skb);
	if (unlikely(ret))
		return ret;

	/* fix dstflags, it may be used there soon */
	priv = j1939_priv_find(can_skb_prv(skb)->ifindex);
	if (!priv)
		return -EINVAL;
	if (!j1939_address_is_valid(cb->dstaddr) ||
			(j1939_address_is_unicast(cb->dstaddr) &&
				priv->ents[cb->dstaddr].nusers))
		cb->dstflags |= ECU_LOCAL;
	put_j1939_priv(priv);
	/* src is always local, I'm sending ... */
	cb->srcflags |= ECU_LOCAL;

	/* prepare new session */
	session = j1939session_new(skb);
	if (!session)
		return -ENOMEM;

	session->skb_iif = can_skb_prv(skb)->ifindex;
	session->extd = (skb->len > MAX_TP_PACKET_SIZE) ? EXTENDED : REGULAR;
	session->transmission = 1;
	session->pkt.total = (skb->len + 6)/7;
	session->pkt.block = session->extd ? 255 :
		min(block ?: 255, session->pkt.total);
	if (j1939cb_is_broadcast(session->cb))
		/* set the end-packet for broadcast */
		session->pkt.last = session->pkt.total;

	/* insert into queue, but avoid collision with pending session */
	if (session->cb->msg_flags & MSG_DONTWAIT)
		ret = j1939session_insert(session) ? 0 : -EAGAIN;
	else
		ret = wait_event_interruptible(tp_wait,
				j1939session_insert(session));
	if (ret < 0)
		goto failed;

	ret = j1939tp_tx_initial(session);
	if (!ret)
		/* transmission started */
		return ret;
	sessionlist_lock();
	list_del_init(&session->list);
	sessionlist_unlock();
failed:
	/* hide the skb from j1939session_drop, as it would
	 * kfree_skb, but our caller will kfree_skb(skb) too.
	 */
	session->skb = NULL;
	j1939session_drop(session);
	return ret;
}

int j1939_recv_transport(struct sk_buff *skb)
{
	struct j1939_sk_buff_cb *cb = (void *)skb->cb;
	const u8 *dat;

	switch (cb->pgn) {
	case etp_pgn_dat:
		j1939xtp_rx_dat(skb, EXTENDED);
		break;
	case etp_pgn_ctl:
		if (skb->len < 8) {
			j1939xtp_rx_bad_message(skb, EXTENDED);
			break;
		}
		dat = skb->data;
		switch (*dat) {
		case etp_cmd_rts:
			j1939xtp_rx_rts(skb, EXTENDED);
			break;
		case etp_cmd_cts:
			j1939xtp_rx_cts(skb, EXTENDED);
			break;
		case etp_cmd_dpo:
			j1939xtp_rx_dpo(skb, EXTENDED);
			break;
		case etp_cmd_eof:
			j1939xtp_rx_eof(skb, EXTENDED);
			break;
		case etp_cmd_abort:
			j1939xtp_rx_abort(skb, EXTENDED);
			break;
		default:
			j1939xtp_rx_bad_message(skb, EXTENDED);
			break;
		}
		break;
	case tp_pgn_dat:
		j1939xtp_rx_dat(skb, REGULAR);
		break;
	case tp_pgn_ctl:
		if (skb->len < 8) {
			j1939xtp_rx_bad_message(skb, REGULAR);
			break;
		}
		dat = skb->data;
		switch (*dat) {
		case tp_cmd_bam:
		case tp_cmd_rts:
			j1939xtp_rx_rts(skb, REGULAR);
			break;
		case tp_cmd_cts:
			j1939xtp_rx_cts(skb, REGULAR);
			break;
		case tp_cmd_eof:
			j1939xtp_rx_eof(skb, REGULAR);
			break;
		case tp_cmd_abort:
			j1939xtp_rx_abort(skb, REGULAR);
			break;
		default:
			j1939xtp_rx_bad_message(skb, REGULAR);
			break;
		}
		break;
	default:
		return 0; /* no problem */
	}
	return 1; /* "I processed the message" */
}

static struct session *j1939session_fresh_new(int size,
		struct sk_buff *rel_skb, pgn_t pgn)
{
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *cb;
	struct session *session;

	/* this SKB is allocated without headroom for CAN skb's.
	 * This may not pose a problem, this SKB will never
	 * enter generic CAN functions
	 */
	skb = alloc_skb(size, GFP_ATOMIC);
	if (!skb)
		return NULL;

	cb = (void *)skb->cb;
	memcpy(cb, rel_skb->cb, sizeof(*cb));
	fix_cb(cb);
	cb->pgn = pgn;

	session = j1939session_new(skb);
	if (!session) {
		kfree(skb);
		return NULL;
	}
	session->skb_iif = rel_skb->skb_iif;
	skb->dev = rel_skb->dev;
	/* alloc data area */
	skb_put(skb, size);
	return session;
}
static struct session *j1939session_new(struct sk_buff *skb)
{
	struct session *session;

	session = kzalloc(sizeof(*session), gfp_any());
	if (!session)
		return NULL;
	INIT_LIST_HEAD(&session->list);
	spin_lock_init(&session->lock);
	session->skb = skb;

	session->cb = (void *)session->skb->cb;
	hrtimer_init(&session->txtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	session->txtimer.function = j1939tp_txtimer;
	hrtimer_init(&session->rxtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	session->rxtimer.function = j1939tp_rxtimer;
	tasklet_init(&session->txtask, j1939tp_txtask, (unsigned long)session);
	tasklet_init(&session->rxtask, j1939tp_rxtask, (unsigned long)session);
	return session;
}

int j1939tp_rmdev_notifier(struct net_device *netdev)
{
	struct session *session, *saved;

	sessionlist_lock();
	list_for_each_entry_safe(session, saved, &tp_sessionq, list) {
		if (session->skb_iif != netdev->ifindex)
			continue;
		list_del_init(&session->list);
		put_session(session);
	}
	list_for_each_entry_safe(session, saved, &tp_extsessionq, list) {
		if (session->skb_iif != netdev->ifindex)
			continue;
		list_del_init(&session->list);
		put_session(session);
	}
	sessionlist_unlock();
	return NOTIFY_DONE;
}

/* PROC */
static int j1939tp_proc_show_session(struct seq_file *sqf,
		struct session *session)
{
	seq_printf(sqf, "%i", session->skb_iif);
	if (session->cb->srcname)
		seq_printf(sqf, "\t%016llx", session->cb->srcname);
	else
		seq_printf(sqf, "\t%02x", session->cb->srcaddr);
	if (session->cb->dstname)
		seq_printf(sqf, "\t%016llx", session->cb->dstname);
	else if (j1939_address_is_unicast(session->cb->dstaddr))
		seq_printf(sqf, "\t%02x", session->cb->dstaddr);
	else
		seq_printf(sqf, "\t-");
	seq_printf(sqf, "\t%05x\t%u/%u\n", session->cb->pgn,
			session->pkt.done*7, session->skb->len);
	return 0;
}

static int j1939tp_proc_show(struct seq_file *sqf, void *v)
{
	struct session *session;

	seq_printf(sqf, "iface\tsrc\tdst\tpgn\tdone/total\n");
	sessionlist_lock();
	list_for_each_entry(session, &tp_sessionq, list)
		j1939tp_proc_show_session(sqf, session);
	list_for_each_entry(session, &tp_extsessionq, list)
		j1939tp_proc_show_session(sqf, session);
	sessionlist_unlock();
	return 0;
}

static int j1939tp_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, j1939tp_proc_show, NULL);
}

static const struct file_operations j1939tp_proc_ops = {
	.owner		= THIS_MODULE,
	.open		= j1939tp_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* module init */
int __init j1939tp_module_init(void)
{
	if (!proc_create("transport", 0444, j1939_procdir, &j1939tp_proc_ops))
		return -ENOMEM;
	return 0;
}

void j1939tp_module_exit(void)
{
	struct session *session, *saved;

	wake_up_all(&tp_wait);

	remove_proc_entry("transport", j1939_procdir);
	sessionlist_lock();
	list_for_each_entry_safe(session, saved, &tp_extsessionq, list) {
		list_del_init(&session->list);
		put_session(session);
	}
	list_for_each_entry_safe(session, saved, &tp_sessionq, list) {
		list_del_init(&session->list);
		put_session(session);
	}
	sessionlist_unlock();
	flush_scheduled_work();
}

