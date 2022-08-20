// SPDX-License-Identifier: GPL-2.0
/* Shared Memory Communications Direct over ISM devices (SMC-D)
 *
 * Functions for ISM device.
 *
 * Copyright IBM Corp. 2018
 */

#include <linux/spinlock.h>
#include <linux/slab.h>
#include <asm/page.h>

#include "smc.h"
#include "smc_core.h"
#include "smc_ism.h"
#include "smc_pnet.h"

struct smcd_dev_list smcd_dev_list = {
	.list = LIST_HEAD_INIT(smcd_dev_list.list),
	.lock = __SPIN_LOCK_UNLOCKED(smcd_dev_list.lock)
};

/* Test if an ISM communication is possible. */
int smc_ism_cantalk(u64 peer_gid, unsigned short vlan_id, struct smcd_dev *smcd)
{
	return smcd->ops->query_remote_gid(smcd, peer_gid, vlan_id ? 1 : 0,
					   vlan_id);
}

int smc_ism_write(struct smcd_dev *smcd, const struct smc_ism_position *pos,
		  void *data, size_t len)
{
	int rc;

	rc = smcd->ops->move_data(smcd, pos->token, pos->index, pos->signal,
				  pos->offset, data, len);

	return rc < 0 ? rc : 0;
}

/* Set a connection using this DMBE. */
void smc_ism_set_conn(struct smc_connection *conn)
{
	unsigned long flags;

	spin_lock_irqsave(&conn->lgr->smcd->lock, flags);
	conn->lgr->smcd->conn[conn->rmb_desc->sba_idx] = conn;
	spin_unlock_irqrestore(&conn->lgr->smcd->lock, flags);
}

/* Unset a connection using this DMBE. */
void smc_ism_unset_conn(struct smc_connection *conn)
{
	unsigned long flags;

	if (!conn->rmb_desc)
		return;

	spin_lock_irqsave(&conn->lgr->smcd->lock, flags);
	conn->lgr->smcd->conn[conn->rmb_desc->sba_idx] = NULL;
	spin_unlock_irqrestore(&conn->lgr->smcd->lock, flags);
}

/* Register a VLAN identifier with the ISM device. Use a reference count
 * and add a VLAN identifier only when the first DMB using this VLAN is
 * registered.
 */
int smc_ism_get_vlan(struct smcd_dev *smcd, unsigned short vlanid)
{
	struct smc_ism_vlanid *new_vlan, *vlan;
	unsigned long flags;
	int rc = 0;

	if (!vlanid)			/* No valid vlan id */
		return -EINVAL;

	/* create new vlan entry, in case we need it */
	new_vlan = kzalloc(sizeof(*new_vlan), GFP_KERNEL);
	if (!new_vlan)
		return -ENOMEM;
	new_vlan->vlanid = vlanid;
	refcount_set(&new_vlan->refcnt, 1);

	/* if there is an existing entry, increase count and return */
	spin_lock_irqsave(&smcd->lock, flags);
	list_for_each_entry(vlan, &smcd->vlan, list) {
		if (vlan->vlanid == vlanid) {
			refcount_inc(&vlan->refcnt);
			kfree(new_vlan);
			goto out;
		}
	}

	/* no existing entry found.
	 * add new entry to device; might fail, e.g., if HW limit reached
	 */
	if (smcd->ops->add_vlan_id(smcd, vlanid)) {
		kfree(new_vlan);
		rc = -EIO;
		goto out;
	}
	list_add_tail(&new_vlan->list, &smcd->vlan);
out:
	spin_unlock_irqrestore(&smcd->lock, flags);
	return rc;
}

/* Unregister a VLAN identifier with the ISM device. Use a reference count
 * and remove a VLAN identifier only when the last DMB using this VLAN is
 * unregistered.
 */
int smc_ism_put_vlan(struct smcd_dev *smcd, unsigned short vlanid)
{
	struct smc_ism_vlanid *vlan;
	unsigned long flags;
	bool found = false;
	int rc = 0;

	if (!vlanid)			/* No valid vlan id */
		return -EINVAL;

	spin_lock_irqsave(&smcd->lock, flags);
	list_for_each_entry(vlan, &smcd->vlan, list) {
		if (vlan->vlanid == vlanid) {
			if (!refcount_dec_and_test(&vlan->refcnt))
				goto out;
			found = true;
			break;
		}
	}
	if (!found) {
		rc = -ENOENT;
		goto out;		/* VLAN id not in table */
	}

	/* Found and the last reference just gone */
	if (smcd->ops->del_vlan_id(smcd, vlanid))
		rc = -EIO;
	list_del(&vlan->list);
	kfree(vlan);
out:
	spin_unlock_irqrestore(&smcd->lock, flags);
	return rc;
}

int smc_ism_unregister_dmb(struct smcd_dev *smcd, struct smc_buf_desc *dmb_desc)
{
	struct smcd_dmb dmb;

	memset(&dmb, 0, sizeof(dmb));
	dmb.dmb_tok = dmb_desc->token;
	dmb.sba_idx = dmb_desc->sba_idx;
	dmb.cpu_addr = dmb_desc->cpu_addr;
	dmb.dma_addr = dmb_desc->dma_addr;
	dmb.dmb_len = dmb_desc->len;
	return smcd->ops->unregister_dmb(smcd, &dmb);
}

int smc_ism_register_dmb(struct smc_link_group *lgr, int dmb_len,
			 struct smc_buf_desc *dmb_desc)
{
	struct smcd_dmb dmb;
	int rc;

	memset(&dmb, 0, sizeof(dmb));
	dmb.dmb_len = dmb_len;
	dmb.sba_idx = dmb_desc->sba_idx;
	dmb.vlan_id = lgr->vlan_id;
	dmb.rgid = lgr->peer_gid;
	rc = lgr->smcd->ops->register_dmb(lgr->smcd, &dmb);
	if (!rc) {
		dmb_desc->sba_idx = dmb.sba_idx;
		dmb_desc->token = dmb.dmb_tok;
		dmb_desc->cpu_addr = dmb.cpu_addr;
		dmb_desc->dma_addr = dmb.dma_addr;
		dmb_desc->len = dmb.dmb_len;
	}
	return rc;
}

struct smc_ism_event_work {
	struct work_struct work;
	struct smcd_dev *smcd;
	struct smcd_event event;
};

#define ISM_EVENT_REQUEST		0x0001
#define ISM_EVENT_RESPONSE		0x0002
#define ISM_EVENT_REQUEST_IR		0x00000001
#define ISM_EVENT_CODE_SHUTDOWN		0x80
#define ISM_EVENT_CODE_TESTLINK		0x83

union smcd_sw_event_info {
	u64	info;
	struct {
		u8		uid[SMC_LGR_ID_SIZE];
		unsigned short	vlan_id;
		u16		code;
	};
};

static void smcd_handle_sw_event(struct smc_ism_event_work *wrk)
{
	union smcd_sw_event_info ev_info;

	ev_info.info = wrk->event.info;
	switch (wrk->event.code) {
	case ISM_EVENT_CODE_SHUTDOWN:	/* Peer shut down DMBs */
		smc_smcd_terminate(wrk->smcd, wrk->event.tok, ev_info.vlan_id);
		break;
	case ISM_EVENT_CODE_TESTLINK:	/* Activity timer */
		if (ev_info.code == ISM_EVENT_REQUEST) {
			ev_info.code = ISM_EVENT_RESPONSE;
			wrk->smcd->ops->signal_event(wrk->smcd,
						     wrk->event.tok,
						     ISM_EVENT_REQUEST_IR,
						     ISM_EVENT_CODE_TESTLINK,
						     ev_info.info);
			}
		break;
	}
}

int smc_ism_signal_shutdown(struct smc_link_group *lgr)
{
	int rc;
	union smcd_sw_event_info ev_info;

	memcpy(ev_info.uid, lgr->id, SMC_LGR_ID_SIZE);
	ev_info.vlan_id = lgr->vlan_id;
	ev_info.code = ISM_EVENT_REQUEST;
	rc = lgr->smcd->ops->signal_event(lgr->smcd, lgr->peer_gid,
					  ISM_EVENT_REQUEST_IR,
					  ISM_EVENT_CODE_SHUTDOWN,
					  ev_info.info);
	return rc;
}

/* worker for SMC-D events */
static void smc_ism_event_work(struct work_struct *work)
{
	struct smc_ism_event_work *wrk =
		container_of(work, struct smc_ism_event_work, work);

	switch (wrk->event.type) {
	case ISM_EVENT_GID:	/* GID event, token is peer GID */
		smc_smcd_terminate(wrk->smcd, wrk->event.tok, VLAN_VID_MASK);
		break;
	case ISM_EVENT_DMB:
		break;
	case ISM_EVENT_SWR:	/* Software defined event */
		smcd_handle_sw_event(wrk);
		break;
	}
	kfree(wrk);
}

static void smcd_release(struct device *dev)
{
	struct smcd_dev *smcd = container_of(dev, struct smcd_dev, dev);

	kfree(smcd->conn);
	kfree(smcd);
}

struct smcd_dev *smcd_alloc_dev(struct device *parent, const char *name,
				const struct smcd_ops *ops, int max_dmbs)
{
	struct smcd_dev *smcd;

	smcd = kzalloc(sizeof(*smcd), GFP_KERNEL);
	if (!smcd)
		return NULL;
	smcd->conn = kcalloc(max_dmbs, sizeof(struct smc_connection *),
			     GFP_KERNEL);
	if (!smcd->conn) {
		kfree(smcd);
		return NULL;
	}

	smcd->dev.parent = parent;
	smcd->dev.release = smcd_release;
	device_initialize(&smcd->dev);
	dev_set_name(&smcd->dev, name);
	smcd->ops = ops;
	smc_pnetid_by_dev_port(parent, 0, smcd->pnetid);

	spin_lock_init(&smcd->lock);
	INIT_LIST_HEAD(&smcd->vlan);
	smcd->event_wq = alloc_ordered_workqueue("ism_evt_wq-%s)",
						 WQ_MEM_RECLAIM, name);
	if (!smcd->event_wq) {
		kfree(smcd->conn);
		kfree(smcd);
		return NULL;
	}
	return smcd;
}
EXPORT_SYMBOL_GPL(smcd_alloc_dev);

int smcd_register_dev(struct smcd_dev *smcd)
{
	spin_lock(&smcd_dev_list.lock);
	list_add_tail(&smcd->list, &smcd_dev_list.list);
	spin_unlock(&smcd_dev_list.lock);

	return device_add(&smcd->dev);
}
EXPORT_SYMBOL_GPL(smcd_register_dev);

void smcd_unregister_dev(struct smcd_dev *smcd)
{
	spin_lock(&smcd_dev_list.lock);
	list_del(&smcd->list);
	spin_unlock(&smcd_dev_list.lock);
	flush_workqueue(smcd->event_wq);
	destroy_workqueue(smcd->event_wq);
	smc_smcd_terminate(smcd, 0, VLAN_VID_MASK);

	device_del(&smcd->dev);
}
EXPORT_SYMBOL_GPL(smcd_unregister_dev);

void smcd_free_dev(struct smcd_dev *smcd)
{
	put_device(&smcd->dev);
}
EXPORT_SYMBOL_GPL(smcd_free_dev);

/* SMCD Device event handler. Called from ISM device interrupt handler.
 * Parameters are smcd device pointer,
 * - event->type (0 --> DMB, 1 --> GID),
 * - event->code (event code),
 * - event->tok (either DMB token when event type 0, or GID when event type 1)
 * - event->time (time of day)
 * - event->info (debug info).
 *
 * Context:
 * - Function called in IRQ context from ISM device driver event handler.
 */
void smcd_handle_event(struct smcd_dev *smcd, struct smcd_event *event)
{
	struct smc_ism_event_work *wrk;

	/* copy event to event work queue, and let it be handled there */
	wrk = kmalloc(sizeof(*wrk), GFP_ATOMIC);
	if (!wrk)
		return;
	INIT_WORK(&wrk->work, smc_ism_event_work);
	wrk->smcd = smcd;
	wrk->event = *event;
	queue_work(smcd->event_wq, &wrk->work);
}
EXPORT_SYMBOL_GPL(smcd_handle_event);

/* SMCD Device interrupt handler. Called from ISM device interrupt handler.
 * Parameters are smcd device pointer and DMB number. Find the connection and
 * schedule the tasklet for this connection.
 *
 * Context:
 * - Function called in IRQ context from ISM device driver IRQ handler.
 */
void smcd_handle_irq(struct smcd_dev *smcd, unsigned int dmbno)
{
	struct smc_connection *conn = NULL;
	unsigned long flags;

	spin_lock_irqsave(&smcd->lock, flags);
	conn = smcd->conn[dmbno];
	if (conn)
		tasklet_schedule(&conn->rx_tsklet);
	spin_unlock_irqrestore(&smcd->lock, flags);
}
EXPORT_SYMBOL_GPL(smcd_handle_irq);
