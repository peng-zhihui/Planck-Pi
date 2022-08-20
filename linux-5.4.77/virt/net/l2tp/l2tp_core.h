/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * L2TP internal definitions.
 *
 * Copyright (c) 2008,2009 Katalix Systems Ltd
 */
#include <linux/refcount.h>

#ifndef _L2TP_CORE_H_
#define _L2TP_CORE_H_

#include <net/dst.h>
#include <net/sock.h>

#ifdef CONFIG_XFRM
#include <net/xfrm.h>
#endif

/* Just some random numbers */
#define L2TP_TUNNEL_MAGIC	0x42114DDA
#define L2TP_SESSION_MAGIC	0x0C04EB7D

/* Per tunnel, session hash table size */
#define L2TP_HASH_BITS	4
#define L2TP_HASH_SIZE	(1 << L2TP_HASH_BITS)

/* System-wide, session hash table size */
#define L2TP_HASH_BITS_2	8
#define L2TP_HASH_SIZE_2	(1 << L2TP_HASH_BITS_2)

struct sk_buff;

struct l2tp_stats {
	atomic_long_t		tx_packets;
	atomic_long_t		tx_bytes;
	atomic_long_t		tx_errors;
	atomic_long_t		rx_packets;
	atomic_long_t		rx_bytes;
	atomic_long_t		rx_seq_discards;
	atomic_long_t		rx_oos_packets;
	atomic_long_t		rx_errors;
	atomic_long_t		rx_cookie_discards;
};

struct l2tp_tunnel;

/* Describes a session. Contains information to determine incoming
 * packets and transmit outgoing ones.
 */
struct l2tp_session_cfg {
	enum l2tp_pwtype	pw_type;
	unsigned int		recv_seq:1;	/* expect receive packets with
						 * sequence numbers? */
	unsigned int		send_seq:1;	/* send packets with sequence
						 * numbers? */
	unsigned int		lns_mode:1;	/* behave as LNS? LAC enables
						 * sequence numbers under
						 * control of LNS. */
	int			debug;		/* bitmask of debug message
						 * categories */
	u16			l2specific_type; /* Layer 2 specific type */
	u8			cookie[8];	/* optional cookie */
	int			cookie_len;	/* 0, 4 or 8 bytes */
	u8			peer_cookie[8];	/* peer's cookie */
	int			peer_cookie_len; /* 0, 4 or 8 bytes */
	int			reorder_timeout; /* configured reorder timeout
						  * (in jiffies) */
	char			*ifname;
};

struct l2tp_session {
	int			magic;		/* should be
						 * L2TP_SESSION_MAGIC */
	long			dead;

	struct l2tp_tunnel	*tunnel;	/* back pointer to tunnel
						 * context */
	u32			session_id;
	u32			peer_session_id;
	u8			cookie[8];
	int			cookie_len;
	u8			peer_cookie[8];
	int			peer_cookie_len;
	u16			l2specific_type;
	u16			hdr_len;
	u32			nr;		/* session NR state (receive) */
	u32			ns;		/* session NR state (send) */
	struct sk_buff_head	reorder_q;	/* receive reorder queue */
	u32			nr_max;		/* max NR. Depends on tunnel */
	u32			nr_window_size;	/* NR window size */
	u32			nr_oos;		/* NR of last OOS packet */
	int			nr_oos_count;	/* For OOS recovery */
	int			nr_oos_count_max;
	struct hlist_node	hlist;		/* Hash list node */
	refcount_t		ref_count;

	char			name[32];	/* for logging */
	char			ifname[IFNAMSIZ];
	unsigned int		recv_seq:1;	/* expect receive packets with
						 * sequence numbers? */
	unsigned int		send_seq:1;	/* send packets with sequence
						 * numbers? */
	unsigned int		lns_mode:1;	/* behave as LNS? LAC enables
						 * sequence numbers under
						 * control of LNS. */
	int			debug;		/* bitmask of debug message
						 * categories */
	int			reorder_timeout; /* configured reorder timeout
						  * (in jiffies) */
	int			reorder_skip;	/* set if skip to next nr */
	enum l2tp_pwtype	pwtype;
	struct l2tp_stats	stats;
	struct hlist_node	global_hlist;	/* Global hash list node */

	int (*build_header)(struct l2tp_session *session, void *buf);
	void (*recv_skb)(struct l2tp_session *session, struct sk_buff *skb, int data_len);
	void (*session_close)(struct l2tp_session *session);
	void (*show)(struct seq_file *m, void *priv);
	uint8_t			priv[0];	/* private data */
};

/* Describes the tunnel. It contains info to track all the associated
 * sessions so incoming packets can be sorted out
 */
struct l2tp_tunnel_cfg {
	int			debug;		/* bitmask of debug message
						 * categories */
	enum l2tp_encap_type	encap;

	/* Used only for kernel-created sockets */
	struct in_addr		local_ip;
	struct in_addr		peer_ip;
#if IS_ENABLED(CONFIG_IPV6)
	struct in6_addr		*local_ip6;
	struct in6_addr		*peer_ip6;
#endif
	u16			local_udp_port;
	u16			peer_udp_port;
	unsigned int		use_udp_checksums:1,
				udp6_zero_tx_checksums:1,
				udp6_zero_rx_checksums:1;
};

struct l2tp_tunnel {
	int			magic;		/* Should be L2TP_TUNNEL_MAGIC */

	unsigned long		dead;

	struct rcu_head rcu;
	rwlock_t		hlist_lock;	/* protect session_hlist */
	bool			acpt_newsess;	/* Indicates whether this
						 * tunnel accepts new sessions.
						 * Protected by hlist_lock.
						 */
	struct hlist_head	session_hlist[L2TP_HASH_SIZE];
						/* hashed list of sessions,
						 * hashed by id */
	u32			tunnel_id;
	u32			peer_tunnel_id;
	int			version;	/* 2=>L2TPv2, 3=>L2TPv3 */

	char			name[20];	/* for logging */
	int			debug;		/* bitmask of debug message
						 * categories */
	enum l2tp_encap_type	encap;
	struct l2tp_stats	stats;

	struct list_head	list;		/* Keep a list of all tunnels */
	struct net		*l2tp_net;	/* the net we belong to */

	refcount_t		ref_count;
	void (*old_sk_destruct)(struct sock *);
	struct sock		*sock;		/* Parent socket */
	int			fd;		/* Parent fd, if tunnel socket
						 * was created by userspace */

	struct work_struct	del_work;
};

struct l2tp_nl_cmd_ops {
	int (*session_create)(struct net *net, struct l2tp_tunnel *tunnel,
			      u32 session_id, u32 peer_session_id,
			      struct l2tp_session_cfg *cfg);
	int (*session_delete)(struct l2tp_session *session);
};

static inline void *l2tp_session_priv(struct l2tp_session *session)
{
	return &session->priv[0];
}

struct l2tp_tunnel *l2tp_tunnel_get(const struct net *net, u32 tunnel_id);
struct l2tp_tunnel *l2tp_tunnel_get_nth(const struct net *net, int nth);
struct l2tp_session *l2tp_tunnel_get_session(struct l2tp_tunnel *tunnel,
					     u32 session_id);

void l2tp_tunnel_free(struct l2tp_tunnel *tunnel);

struct l2tp_session *l2tp_session_get(const struct net *net, u32 session_id);
struct l2tp_session *l2tp_session_get_nth(struct l2tp_tunnel *tunnel, int nth);
struct l2tp_session *l2tp_session_get_by_ifname(const struct net *net,
						const char *ifname);

int l2tp_tunnel_create(struct net *net, int fd, int version, u32 tunnel_id,
		       u32 peer_tunnel_id, struct l2tp_tunnel_cfg *cfg,
		       struct l2tp_tunnel **tunnelp);
int l2tp_tunnel_register(struct l2tp_tunnel *tunnel, struct net *net,
			 struct l2tp_tunnel_cfg *cfg);

void l2tp_tunnel_delete(struct l2tp_tunnel *tunnel);
struct l2tp_session *l2tp_session_create(int priv_size,
					 struct l2tp_tunnel *tunnel,
					 u32 session_id, u32 peer_session_id,
					 struct l2tp_session_cfg *cfg);
int l2tp_session_register(struct l2tp_session *session,
			  struct l2tp_tunnel *tunnel);

void __l2tp_session_unhash(struct l2tp_session *session);
int l2tp_session_delete(struct l2tp_session *session);
void l2tp_session_free(struct l2tp_session *session);
void l2tp_recv_common(struct l2tp_session *session, struct sk_buff *skb,
		      unsigned char *ptr, unsigned char *optr, u16 hdrflags,
		      int length);
int l2tp_udp_encap_recv(struct sock *sk, struct sk_buff *skb);
void l2tp_session_set_header_len(struct l2tp_session *session, int version);

int l2tp_xmit_skb(struct l2tp_session *session, struct sk_buff *skb,
		  int hdr_len);

int l2tp_nl_register_ops(enum l2tp_pwtype pw_type,
			 const struct l2tp_nl_cmd_ops *ops);
void l2tp_nl_unregister_ops(enum l2tp_pwtype pw_type);
int l2tp_ioctl(struct sock *sk, int cmd, unsigned long arg);

static inline void l2tp_tunnel_inc_refcount(struct l2tp_tunnel *tunnel)
{
	refcount_inc(&tunnel->ref_count);
}

static inline void l2tp_tunnel_dec_refcount(struct l2tp_tunnel *tunnel)
{
	if (refcount_dec_and_test(&tunnel->ref_count))
		l2tp_tunnel_free(tunnel);
}

/* Session reference counts. Incremented when code obtains a reference
 * to a session.
 */
static inline void l2tp_session_inc_refcount(struct l2tp_session *session)
{
	refcount_inc(&session->ref_count);
}

static inline void l2tp_session_dec_refcount(struct l2tp_session *session)
{
	if (refcount_dec_and_test(&session->ref_count))
		l2tp_session_free(session);
}

static inline int l2tp_get_l2specific_len(struct l2tp_session *session)
{
	switch (session->l2specific_type) {
	case L2TP_L2SPECTYPE_DEFAULT:
		return 4;
	case L2TP_L2SPECTYPE_NONE:
	default:
		return 0;
	}
}

static inline u32 l2tp_tunnel_dst_mtu(const struct l2tp_tunnel *tunnel)
{
	struct dst_entry *dst;
	u32 mtu;

	dst = sk_dst_get(tunnel->sock);
	if (!dst)
		return 0;

	mtu = dst_mtu(dst);
	dst_release(dst);

	return mtu;
}

#ifdef CONFIG_XFRM
static inline bool l2tp_tunnel_uses_xfrm(const struct l2tp_tunnel *tunnel)
{
	struct sock *sk = tunnel->sock;

	return sk && (rcu_access_pointer(sk->sk_policy[0]) ||
		      rcu_access_pointer(sk->sk_policy[1]));
}
#else
static inline bool l2tp_tunnel_uses_xfrm(const struct l2tp_tunnel *tunnel)
{
	return false;
}
#endif

static inline int l2tp_v3_ensure_opt_in_linear(struct l2tp_session *session, struct sk_buff *skb,
					       unsigned char **ptr, unsigned char **optr)
{
	int opt_len = session->peer_cookie_len + l2tp_get_l2specific_len(session);

	if (opt_len > 0) {
		int off = *ptr - *optr;

		if (!pskb_may_pull(skb, off + opt_len))
			return -1;

		if (skb->data != *optr) {
			*optr = skb->data;
			*ptr = skb->data + off;
		}
	}

	return 0;
}

#define l2tp_printk(ptr, type, func, fmt, ...)				\
do {									\
	if (((ptr)->debug) & (type))					\
		func(fmt, ##__VA_ARGS__);				\
} while (0)

#define l2tp_warn(ptr, type, fmt, ...)					\
	l2tp_printk(ptr, type, pr_warn, fmt, ##__VA_ARGS__)
#define l2tp_info(ptr, type, fmt, ...)					\
	l2tp_printk(ptr, type, pr_info, fmt, ##__VA_ARGS__)
#define l2tp_dbg(ptr, type, fmt, ...)					\
	l2tp_printk(ptr, type, pr_debug, fmt, ##__VA_ARGS__)

#define MODULE_ALIAS_L2TP_PWTYPE(type) \
	MODULE_ALIAS("net-l2tp-type-" __stringify(type))

#endif /* _L2TP_CORE_H_ */
