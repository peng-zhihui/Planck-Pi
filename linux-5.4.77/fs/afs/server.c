// SPDX-License-Identifier: GPL-2.0-or-later
/* AFS server record management
 *
 * Copyright (C) 2002, 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include "afs_fs.h"
#include "internal.h"
#include "protocol_yfs.h"

static unsigned afs_server_gc_delay = 10;	/* Server record timeout in seconds */
static unsigned afs_server_update_delay = 30;	/* Time till VLDB recheck in secs */
static atomic_t afs_server_debug_id;

static void afs_inc_servers_outstanding(struct afs_net *net)
{
	atomic_inc(&net->servers_outstanding);
}

static void afs_dec_servers_outstanding(struct afs_net *net)
{
	if (atomic_dec_and_test(&net->servers_outstanding))
		wake_up_var(&net->servers_outstanding);
}

/*
 * Find a server by one of its addresses.
 */
struct afs_server *afs_find_server(struct afs_net *net,
				   const struct sockaddr_rxrpc *srx)
{
	const struct afs_addr_list *alist;
	struct afs_server *server = NULL;
	unsigned int i;
	int seq = 0, diff;

	rcu_read_lock();

	do {
		if (server)
			afs_put_server(net, server, afs_server_trace_put_find_rsq);
		server = NULL;
		read_seqbegin_or_lock(&net->fs_addr_lock, &seq);

		if (srx->transport.family == AF_INET6) {
			const struct sockaddr_in6 *a = &srx->transport.sin6, *b;
			hlist_for_each_entry_rcu(server, &net->fs_addresses6, addr6_link) {
				alist = rcu_dereference(server->addresses);
				for (i = alist->nr_ipv4; i < alist->nr_addrs; i++) {
					b = &alist->addrs[i].transport.sin6;
					diff = ((u16 __force)a->sin6_port -
						(u16 __force)b->sin6_port);
					if (diff == 0)
						diff = memcmp(&a->sin6_addr,
							      &b->sin6_addr,
							      sizeof(struct in6_addr));
					if (diff == 0)
						goto found;
				}
			}
		} else {
			const struct sockaddr_in *a = &srx->transport.sin, *b;
			hlist_for_each_entry_rcu(server, &net->fs_addresses4, addr4_link) {
				alist = rcu_dereference(server->addresses);
				for (i = 0; i < alist->nr_ipv4; i++) {
					b = &alist->addrs[i].transport.sin;
					diff = ((u16 __force)a->sin_port -
						(u16 __force)b->sin_port);
					if (diff == 0)
						diff = ((u32 __force)a->sin_addr.s_addr -
							(u32 __force)b->sin_addr.s_addr);
					if (diff == 0)
						goto found;
				}
			}
		}

		server = NULL;
	found:
		if (server && !atomic_inc_not_zero(&server->usage))
			server = NULL;

	} while (need_seqretry(&net->fs_addr_lock, seq));

	done_seqretry(&net->fs_addr_lock, seq);

	rcu_read_unlock();
	return server;
}

/*
 * Look up a server by its UUID
 */
struct afs_server *afs_find_server_by_uuid(struct afs_net *net, const uuid_t *uuid)
{
	struct afs_server *server = NULL;
	struct rb_node *p;
	int diff, seq = 0;

	_enter("%pU", uuid);

	do {
		/* Unfortunately, rbtree walking doesn't give reliable results
		 * under just the RCU read lock, so we have to check for
		 * changes.
		 */
		if (server)
			afs_put_server(net, server, afs_server_trace_put_uuid_rsq);
		server = NULL;

		read_seqbegin_or_lock(&net->fs_lock, &seq);

		p = net->fs_servers.rb_node;
		while (p) {
			server = rb_entry(p, struct afs_server, uuid_rb);

			diff = memcmp(uuid, &server->uuid, sizeof(*uuid));
			if (diff < 0) {
				p = p->rb_left;
			} else if (diff > 0) {
				p = p->rb_right;
			} else {
				afs_get_server(server, afs_server_trace_get_by_uuid);
				break;
			}

			server = NULL;
		}
	} while (need_seqretry(&net->fs_lock, seq));

	done_seqretry(&net->fs_lock, seq);

	_leave(" = %p", server);
	return server;
}

/*
 * Install a server record in the namespace tree
 */
static struct afs_server *afs_install_server(struct afs_net *net,
					     struct afs_server *candidate)
{
	const struct afs_addr_list *alist;
	struct afs_server *server;
	struct rb_node **pp, *p;
	int ret = -EEXIST, diff;

	_enter("%p", candidate);

	write_seqlock(&net->fs_lock);

	/* Firstly install the server in the UUID lookup tree */
	pp = &net->fs_servers.rb_node;
	p = NULL;
	while (*pp) {
		p = *pp;
		_debug("- consider %p", p);
		server = rb_entry(p, struct afs_server, uuid_rb);
		diff = memcmp(&candidate->uuid, &server->uuid, sizeof(uuid_t));
		if (diff < 0)
			pp = &(*pp)->rb_left;
		else if (diff > 0)
			pp = &(*pp)->rb_right;
		else
			goto exists;
	}

	server = candidate;
	rb_link_node(&server->uuid_rb, p, pp);
	rb_insert_color(&server->uuid_rb, &net->fs_servers);
	hlist_add_head_rcu(&server->proc_link, &net->fs_proc);

	write_seqlock(&net->fs_addr_lock);
	alist = rcu_dereference_protected(server->addresses,
					  lockdep_is_held(&net->fs_addr_lock.lock));

	/* Secondly, if the server has any IPv4 and/or IPv6 addresses, install
	 * it in the IPv4 and/or IPv6 reverse-map lists.
	 *
	 * TODO: For speed we want to use something other than a flat list
	 * here; even sorting the list in terms of lowest address would help a
	 * bit, but anything we might want to do gets messy and memory
	 * intensive.
	 */
	if (alist->nr_ipv4 > 0)
		hlist_add_head_rcu(&server->addr4_link, &net->fs_addresses4);
	if (alist->nr_addrs > alist->nr_ipv4)
		hlist_add_head_rcu(&server->addr6_link, &net->fs_addresses6);

	write_sequnlock(&net->fs_addr_lock);
	ret = 0;

exists:
	afs_get_server(server, afs_server_trace_get_install);
	write_sequnlock(&net->fs_lock);
	return server;
}

/*
 * allocate a new server record
 */
static struct afs_server *afs_alloc_server(struct afs_net *net,
					   const uuid_t *uuid,
					   struct afs_addr_list *alist)
{
	struct afs_server *server;

	_enter("");

	server = kzalloc(sizeof(struct afs_server), GFP_KERNEL);
	if (!server)
		goto enomem;

	atomic_set(&server->usage, 1);
	server->debug_id = atomic_inc_return(&afs_server_debug_id);
	RCU_INIT_POINTER(server->addresses, alist);
	server->addr_version = alist->version;
	server->uuid = *uuid;
	server->update_at = ktime_get_real_seconds() + afs_server_update_delay;
	rwlock_init(&server->fs_lock);
	INIT_HLIST_HEAD(&server->cb_volumes);
	rwlock_init(&server->cb_break_lock);
	init_waitqueue_head(&server->probe_wq);
	spin_lock_init(&server->probe_lock);

	afs_inc_servers_outstanding(net);
	trace_afs_server(server, 1, afs_server_trace_alloc);
	_leave(" = %p", server);
	return server;

enomem:
	_leave(" = NULL [nomem]");
	return NULL;
}

/*
 * Look up an address record for a server
 */
static struct afs_addr_list *afs_vl_lookup_addrs(struct afs_cell *cell,
						 struct key *key, const uuid_t *uuid)
{
	struct afs_vl_cursor vc;
	struct afs_addr_list *alist = NULL;
	int ret;

	ret = -ERESTARTSYS;
	if (afs_begin_vlserver_operation(&vc, cell, key)) {
		while (afs_select_vlserver(&vc)) {
			if (test_bit(AFS_VLSERVER_FL_IS_YFS, &vc.server->flags))
				alist = afs_yfsvl_get_endpoints(&vc, uuid);
			else
				alist = afs_vl_get_addrs_u(&vc, uuid);
		}

		ret = afs_end_vlserver_operation(&vc);
	}

	return ret < 0 ? ERR_PTR(ret) : alist;
}

/*
 * Get or create a fileserver record.
 */
struct afs_server *afs_lookup_server(struct afs_cell *cell, struct key *key,
				     const uuid_t *uuid)
{
	struct afs_addr_list *alist;
	struct afs_server *server, *candidate;

	_enter("%p,%pU", cell->net, uuid);

	server = afs_find_server_by_uuid(cell->net, uuid);
	if (server)
		return server;

	alist = afs_vl_lookup_addrs(cell, key, uuid);
	if (IS_ERR(alist))
		return ERR_CAST(alist);

	candidate = afs_alloc_server(cell->net, uuid, alist);
	if (!candidate) {
		afs_put_addrlist(alist);
		return ERR_PTR(-ENOMEM);
	}

	server = afs_install_server(cell->net, candidate);
	if (server != candidate) {
		afs_put_addrlist(alist);
		kfree(candidate);
	}

	_leave(" = %p{%d}", server, atomic_read(&server->usage));
	return server;
}

/*
 * Set the server timer to fire after a given delay, assuming it's not already
 * set for an earlier time.
 */
static void afs_set_server_timer(struct afs_net *net, time64_t delay)
{
	if (net->live) {
		afs_inc_servers_outstanding(net);
		if (timer_reduce(&net->fs_timer, jiffies + delay * HZ))
			afs_dec_servers_outstanding(net);
	}
}

/*
 * Server management timer.  We have an increment on fs_outstanding that we
 * need to pass along to the work item.
 */
void afs_servers_timer(struct timer_list *timer)
{
	struct afs_net *net = container_of(timer, struct afs_net, fs_timer);

	_enter("");
	if (!queue_work(afs_wq, &net->fs_manager))
		afs_dec_servers_outstanding(net);
}

/*
 * Get a reference on a server object.
 */
struct afs_server *afs_get_server(struct afs_server *server,
				  enum afs_server_trace reason)
{
	unsigned int u = atomic_inc_return(&server->usage);

	trace_afs_server(server, u, reason);
	return server;
}

/*
 * Release a reference on a server record.
 */
void afs_put_server(struct afs_net *net, struct afs_server *server,
		    enum afs_server_trace reason)
{
	unsigned int usage;

	if (!server)
		return;

	server->put_time = ktime_get_real_seconds();

	usage = atomic_dec_return(&server->usage);

	trace_afs_server(server, usage, reason);

	if (likely(usage > 0))
		return;

	afs_set_server_timer(net, afs_server_gc_delay);
}

static void afs_server_rcu(struct rcu_head *rcu)
{
	struct afs_server *server = container_of(rcu, struct afs_server, rcu);

	trace_afs_server(server, atomic_read(&server->usage),
			 afs_server_trace_free);
	afs_put_addrlist(rcu_access_pointer(server->addresses));
	kfree(server);
}

/*
 * destroy a dead server
 */
static void afs_destroy_server(struct afs_net *net, struct afs_server *server)
{
	struct afs_addr_list *alist = rcu_access_pointer(server->addresses);
	struct afs_addr_cursor ac = {
		.alist	= alist,
		.index	= alist->preferred,
		.error	= 0,
	};

	trace_afs_server(server, atomic_read(&server->usage),
			 afs_server_trace_give_up_cb);

	if (test_bit(AFS_SERVER_FL_MAY_HAVE_CB, &server->flags))
		afs_fs_give_up_all_callbacks(net, server, &ac, NULL);

	wait_var_event(&server->probe_outstanding,
		       atomic_read(&server->probe_outstanding) == 0);

	trace_afs_server(server, atomic_read(&server->usage),
			 afs_server_trace_destroy);
	call_rcu(&server->rcu, afs_server_rcu);
	afs_dec_servers_outstanding(net);
}

/*
 * Garbage collect any expired servers.
 */
static void afs_gc_servers(struct afs_net *net, struct afs_server *gc_list)
{
	struct afs_server *server;
	bool deleted;
	int usage;

	while ((server = gc_list)) {
		gc_list = server->gc_next;

		write_seqlock(&net->fs_lock);
		usage = 1;
		deleted = atomic_try_cmpxchg(&server->usage, &usage, 0);
		trace_afs_server(server, usage, afs_server_trace_gc);
		if (deleted) {
			rb_erase(&server->uuid_rb, &net->fs_servers);
			hlist_del_rcu(&server->proc_link);
		}
		write_sequnlock(&net->fs_lock);

		if (deleted) {
			write_seqlock(&net->fs_addr_lock);
			if (!hlist_unhashed(&server->addr4_link))
				hlist_del_rcu(&server->addr4_link);
			if (!hlist_unhashed(&server->addr6_link))
				hlist_del_rcu(&server->addr6_link);
			write_sequnlock(&net->fs_addr_lock);
			afs_destroy_server(net, server);
		}
	}
}

/*
 * Manage the records of servers known to be within a network namespace.  This
 * includes garbage collecting unused servers.
 *
 * Note also that we were given an increment on net->servers_outstanding by
 * whoever queued us that we need to deal with before returning.
 */
void afs_manage_servers(struct work_struct *work)
{
	struct afs_net *net = container_of(work, struct afs_net, fs_manager);
	struct afs_server *gc_list = NULL;
	struct rb_node *cursor;
	time64_t now = ktime_get_real_seconds(), next_manage = TIME64_MAX;
	bool purging = !net->live;

	_enter("");

	/* Trawl the server list looking for servers that have expired from
	 * lack of use.
	 */
	read_seqlock_excl(&net->fs_lock);

	for (cursor = rb_first(&net->fs_servers); cursor; cursor = rb_next(cursor)) {
		struct afs_server *server =
			rb_entry(cursor, struct afs_server, uuid_rb);
		int usage = atomic_read(&server->usage);

		_debug("manage %pU %u", &server->uuid, usage);

		ASSERTCMP(usage, >=, 1);
		ASSERTIFCMP(purging, usage, ==, 1);

		if (usage == 1) {
			time64_t expire_at = server->put_time;

			if (!test_bit(AFS_SERVER_FL_VL_FAIL, &server->flags) &&
			    !test_bit(AFS_SERVER_FL_NOT_FOUND, &server->flags))
				expire_at += afs_server_gc_delay;
			if (purging || expire_at <= now) {
				server->gc_next = gc_list;
				gc_list = server;
			} else if (expire_at < next_manage) {
				next_manage = expire_at;
			}
		}
	}

	read_sequnlock_excl(&net->fs_lock);

	/* Update the timer on the way out.  We have to pass an increment on
	 * servers_outstanding in the namespace that we are in to the timer or
	 * the work scheduler.
	 */
	if (!purging && next_manage < TIME64_MAX) {
		now = ktime_get_real_seconds();

		if (next_manage - now <= 0) {
			if (queue_work(afs_wq, &net->fs_manager))
				afs_inc_servers_outstanding(net);
		} else {
			afs_set_server_timer(net, next_manage - now);
		}
	}

	afs_gc_servers(net, gc_list);

	afs_dec_servers_outstanding(net);
	_leave(" [%d]", atomic_read(&net->servers_outstanding));
}

static void afs_queue_server_manager(struct afs_net *net)
{
	afs_inc_servers_outstanding(net);
	if (!queue_work(afs_wq, &net->fs_manager))
		afs_dec_servers_outstanding(net);
}

/*
 * Purge list of servers.
 */
void afs_purge_servers(struct afs_net *net)
{
	_enter("");

	if (del_timer_sync(&net->fs_timer))
		atomic_dec(&net->servers_outstanding);

	afs_queue_server_manager(net);

	_debug("wait");
	wait_var_event(&net->servers_outstanding,
		       !atomic_read(&net->servers_outstanding));
	_leave("");
}

/*
 * Get an update for a server's address list.
 */
static noinline bool afs_update_server_record(struct afs_fs_cursor *fc, struct afs_server *server)
{
	struct afs_addr_list *alist, *discard;

	_enter("");

	trace_afs_server(server, atomic_read(&server->usage), afs_server_trace_update);

	alist = afs_vl_lookup_addrs(fc->vnode->volume->cell, fc->key,
				    &server->uuid);
	if (IS_ERR(alist)) {
		if ((PTR_ERR(alist) == -ERESTARTSYS ||
		     PTR_ERR(alist) == -EINTR) &&
		    !(fc->flags & AFS_FS_CURSOR_INTR) &&
		    server->addresses) {
			_leave(" = t [intr]");
			return true;
		}
		fc->error = PTR_ERR(alist);
		_leave(" = f [%d]", fc->error);
		return false;
	}

	discard = alist;
	if (server->addr_version != alist->version) {
		write_lock(&server->fs_lock);
		discard = rcu_dereference_protected(server->addresses,
						    lockdep_is_held(&server->fs_lock));
		rcu_assign_pointer(server->addresses, alist);
		server->addr_version = alist->version;
		write_unlock(&server->fs_lock);
	}

	server->update_at = ktime_get_real_seconds() + afs_server_update_delay;
	afs_put_addrlist(discard);
	_leave(" = t");
	return true;
}

/*
 * See if a server's address list needs updating.
 */
bool afs_check_server_record(struct afs_fs_cursor *fc, struct afs_server *server)
{
	time64_t now = ktime_get_real_seconds();
	long diff;
	bool success;
	int ret, retries = 0;

	_enter("");

	ASSERT(server);

retry:
	diff = READ_ONCE(server->update_at) - now;
	if (diff > 0) {
		_leave(" = t [not now %ld]", diff);
		return true;
	}

	if (!test_and_set_bit_lock(AFS_SERVER_FL_UPDATING, &server->flags)) {
		success = afs_update_server_record(fc, server);
		clear_bit_unlock(AFS_SERVER_FL_UPDATING, &server->flags);
		wake_up_bit(&server->flags, AFS_SERVER_FL_UPDATING);
		_leave(" = %d", success);
		return success;
	}

	ret = wait_on_bit(&server->flags, AFS_SERVER_FL_UPDATING,
			  (fc->flags & AFS_FS_CURSOR_INTR) ?
			  TASK_INTERRUPTIBLE : TASK_UNINTERRUPTIBLE);
	if (ret == -ERESTARTSYS) {
		fc->error = ret;
		_leave(" = f [intr]");
		return false;
	}

	retries++;
	if (retries == 4) {
		_leave(" = f [stale]");
		ret = -ESTALE;
		return false;
	}
	goto retry;
}
