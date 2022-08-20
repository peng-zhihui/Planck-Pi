// SPDX-License-Identifier: GPL-2.0
/*
 * Common code for the NVMe target.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/pci-p2pdma.h>
#include <linux/scatterlist.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

#include "nvmet.h"

struct workqueue_struct *buffered_io_wq;
static const struct nvmet_fabrics_ops *nvmet_transports[NVMF_TRTYPE_MAX];
static DEFINE_IDA(cntlid_ida);

/*
 * This read/write semaphore is used to synchronize access to configuration
 * information on a target system that will result in discovery log page
 * information change for at least one host.
 * The full list of resources to protected by this semaphore is:
 *
 *  - subsystems list
 *  - per-subsystem allowed hosts list
 *  - allow_any_host subsystem attribute
 *  - nvmet_genctr
 *  - the nvmet_transports array
 *
 * When updating any of those lists/structures write lock should be obtained,
 * while when reading (popolating discovery log page or checking host-subsystem
 * link) read lock is obtained to allow concurrent reads.
 */
DECLARE_RWSEM(nvmet_config_sem);

u32 nvmet_ana_group_enabled[NVMET_MAX_ANAGRPS + 1];
u64 nvmet_ana_chgcnt;
DECLARE_RWSEM(nvmet_ana_sem);

inline u16 errno_to_nvme_status(struct nvmet_req *req, int errno)
{
	u16 status;

	switch (errno) {
	case 0:
		status = NVME_SC_SUCCESS;
		break;
	case -ENOSPC:
		req->error_loc = offsetof(struct nvme_rw_command, length);
		status = NVME_SC_CAP_EXCEEDED | NVME_SC_DNR;
		break;
	case -EREMOTEIO:
		req->error_loc = offsetof(struct nvme_rw_command, slba);
		status = NVME_SC_LBA_RANGE | NVME_SC_DNR;
		break;
	case -EOPNOTSUPP:
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		switch (req->cmd->common.opcode) {
		case nvme_cmd_dsm:
		case nvme_cmd_write_zeroes:
			status = NVME_SC_ONCS_NOT_SUPPORTED | NVME_SC_DNR;
			break;
		default:
			status = NVME_SC_INVALID_OPCODE | NVME_SC_DNR;
		}
		break;
	case -ENODATA:
		req->error_loc = offsetof(struct nvme_rw_command, nsid);
		status = NVME_SC_ACCESS_DENIED;
		break;
	case -EIO:
		/* FALLTHRU */
	default:
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		status = NVME_SC_INTERNAL | NVME_SC_DNR;
	}

	return status;
}

static struct nvmet_subsys *nvmet_find_get_subsys(struct nvmet_port *port,
		const char *subsysnqn);

u16 nvmet_copy_to_sgl(struct nvmet_req *req, off_t off, const void *buf,
		size_t len)
{
	if (sg_pcopy_from_buffer(req->sg, req->sg_cnt, buf, len, off) != len) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		return NVME_SC_SGL_INVALID_DATA | NVME_SC_DNR;
	}
	return 0;
}

u16 nvmet_copy_from_sgl(struct nvmet_req *req, off_t off, void *buf, size_t len)
{
	if (sg_pcopy_to_buffer(req->sg, req->sg_cnt, buf, len, off) != len) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		return NVME_SC_SGL_INVALID_DATA | NVME_SC_DNR;
	}
	return 0;
}

u16 nvmet_zero_sgl(struct nvmet_req *req, off_t off, size_t len)
{
	if (sg_zero_buffer(req->sg, req->sg_cnt, len, off) != len) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		return NVME_SC_SGL_INVALID_DATA | NVME_SC_DNR;
	}
	return 0;
}

static unsigned int nvmet_max_nsid(struct nvmet_subsys *subsys)
{
	struct nvmet_ns *ns;

	if (list_empty(&subsys->namespaces))
		return 0;

	ns = list_last_entry(&subsys->namespaces, struct nvmet_ns, dev_link);
	return ns->nsid;
}

static u32 nvmet_async_event_result(struct nvmet_async_event *aen)
{
	return aen->event_type | (aen->event_info << 8) | (aen->log_page << 16);
}

static void nvmet_async_events_free(struct nvmet_ctrl *ctrl)
{
	struct nvmet_req *req;

	while (1) {
		mutex_lock(&ctrl->lock);
		if (!ctrl->nr_async_event_cmds) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		req = ctrl->async_event_cmds[--ctrl->nr_async_event_cmds];
		mutex_unlock(&ctrl->lock);
		nvmet_req_complete(req, NVME_SC_INTERNAL | NVME_SC_DNR);
	}
}

static void nvmet_async_event_work(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl =
		container_of(work, struct nvmet_ctrl, async_event_work);
	struct nvmet_async_event *aen;
	struct nvmet_req *req;

	while (1) {
		mutex_lock(&ctrl->lock);
		aen = list_first_entry_or_null(&ctrl->async_events,
				struct nvmet_async_event, entry);
		if (!aen || !ctrl->nr_async_event_cmds) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		req = ctrl->async_event_cmds[--ctrl->nr_async_event_cmds];
		nvmet_set_result(req, nvmet_async_event_result(aen));

		list_del(&aen->entry);
		kfree(aen);

		mutex_unlock(&ctrl->lock);
		nvmet_req_complete(req, 0);
	}
}

void nvmet_add_async_event(struct nvmet_ctrl *ctrl, u8 event_type,
		u8 event_info, u8 log_page)
{
	struct nvmet_async_event *aen;

	aen = kmalloc(sizeof(*aen), GFP_KERNEL);
	if (!aen)
		return;

	aen->event_type = event_type;
	aen->event_info = event_info;
	aen->log_page = log_page;

	mutex_lock(&ctrl->lock);
	list_add_tail(&aen->entry, &ctrl->async_events);
	mutex_unlock(&ctrl->lock);

	schedule_work(&ctrl->async_event_work);
}

static void nvmet_add_to_changed_ns_log(struct nvmet_ctrl *ctrl, __le32 nsid)
{
	u32 i;

	mutex_lock(&ctrl->lock);
	if (ctrl->nr_changed_ns > NVME_MAX_CHANGED_NAMESPACES)
		goto out_unlock;

	for (i = 0; i < ctrl->nr_changed_ns; i++) {
		if (ctrl->changed_ns_list[i] == nsid)
			goto out_unlock;
	}

	if (ctrl->nr_changed_ns == NVME_MAX_CHANGED_NAMESPACES) {
		ctrl->changed_ns_list[0] = cpu_to_le32(0xffffffff);
		ctrl->nr_changed_ns = U32_MAX;
		goto out_unlock;
	}

	ctrl->changed_ns_list[ctrl->nr_changed_ns++] = nsid;
out_unlock:
	mutex_unlock(&ctrl->lock);
}

void nvmet_ns_changed(struct nvmet_subsys *subsys, u32 nsid)
{
	struct nvmet_ctrl *ctrl;

	lockdep_assert_held(&subsys->lock);

	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		nvmet_add_to_changed_ns_log(ctrl, cpu_to_le32(nsid));
		if (nvmet_aen_bit_disabled(ctrl, NVME_AEN_BIT_NS_ATTR))
			continue;
		nvmet_add_async_event(ctrl, NVME_AER_TYPE_NOTICE,
				NVME_AER_NOTICE_NS_CHANGED,
				NVME_LOG_CHANGED_NS);
	}
}

void nvmet_send_ana_event(struct nvmet_subsys *subsys,
		struct nvmet_port *port)
{
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (port && ctrl->port != port)
			continue;
		if (nvmet_aen_bit_disabled(ctrl, NVME_AEN_BIT_ANA_CHANGE))
			continue;
		nvmet_add_async_event(ctrl, NVME_AER_TYPE_NOTICE,
				NVME_AER_NOTICE_ANA, NVME_LOG_ANA);
	}
	mutex_unlock(&subsys->lock);
}

void nvmet_port_send_ana_event(struct nvmet_port *port)
{
	struct nvmet_subsys_link *p;

	down_read(&nvmet_config_sem);
	list_for_each_entry(p, &port->subsystems, entry)
		nvmet_send_ana_event(p->subsys, port);
	up_read(&nvmet_config_sem);
}

int nvmet_register_transport(const struct nvmet_fabrics_ops *ops)
{
	int ret = 0;

	down_write(&nvmet_config_sem);
	if (nvmet_transports[ops->type])
		ret = -EINVAL;
	else
		nvmet_transports[ops->type] = ops;
	up_write(&nvmet_config_sem);

	return ret;
}
EXPORT_SYMBOL_GPL(nvmet_register_transport);

void nvmet_unregister_transport(const struct nvmet_fabrics_ops *ops)
{
	down_write(&nvmet_config_sem);
	nvmet_transports[ops->type] = NULL;
	up_write(&nvmet_config_sem);
}
EXPORT_SYMBOL_GPL(nvmet_unregister_transport);

void nvmet_port_del_ctrls(struct nvmet_port *port, struct nvmet_subsys *subsys)
{
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (ctrl->port == port)
			ctrl->ops->delete_ctrl(ctrl);
	}
	mutex_unlock(&subsys->lock);
}

int nvmet_enable_port(struct nvmet_port *port)
{
	const struct nvmet_fabrics_ops *ops;
	int ret;

	lockdep_assert_held(&nvmet_config_sem);

	ops = nvmet_transports[port->disc_addr.trtype];
	if (!ops) {
		up_write(&nvmet_config_sem);
		request_module("nvmet-transport-%d", port->disc_addr.trtype);
		down_write(&nvmet_config_sem);
		ops = nvmet_transports[port->disc_addr.trtype];
		if (!ops) {
			pr_err("transport type %d not supported\n",
				port->disc_addr.trtype);
			return -EINVAL;
		}
	}

	if (!try_module_get(ops->owner))
		return -EINVAL;

	ret = ops->add_port(port);
	if (ret) {
		module_put(ops->owner);
		return ret;
	}

	/* If the transport didn't set inline_data_size, then disable it. */
	if (port->inline_data_size < 0)
		port->inline_data_size = 0;

	port->enabled = true;
	port->tr_ops = ops;
	return 0;
}

void nvmet_disable_port(struct nvmet_port *port)
{
	const struct nvmet_fabrics_ops *ops;

	lockdep_assert_held(&nvmet_config_sem);

	port->enabled = false;
	port->tr_ops = NULL;

	ops = nvmet_transports[port->disc_addr.trtype];
	ops->remove_port(port);
	module_put(ops->owner);
}

static void nvmet_keep_alive_timer(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl = container_of(to_delayed_work(work),
			struct nvmet_ctrl, ka_work);
	bool cmd_seen = ctrl->cmd_seen;

	ctrl->cmd_seen = false;
	if (cmd_seen) {
		pr_debug("ctrl %d reschedule traffic based keep-alive timer\n",
			ctrl->cntlid);
		schedule_delayed_work(&ctrl->ka_work, ctrl->kato * HZ);
		return;
	}

	pr_err("ctrl %d keep-alive timer (%d seconds) expired!\n",
		ctrl->cntlid, ctrl->kato);

	nvmet_ctrl_fatal_error(ctrl);
}

static void nvmet_start_keep_alive_timer(struct nvmet_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	pr_debug("ctrl %d start keep-alive timer for %d secs\n",
		ctrl->cntlid, ctrl->kato);

	INIT_DELAYED_WORK(&ctrl->ka_work, nvmet_keep_alive_timer);
	schedule_delayed_work(&ctrl->ka_work, ctrl->kato * HZ);
}

static void nvmet_stop_keep_alive_timer(struct nvmet_ctrl *ctrl)
{
	if (unlikely(ctrl->kato == 0))
		return;

	pr_debug("ctrl %d stop keep-alive\n", ctrl->cntlid);

	cancel_delayed_work_sync(&ctrl->ka_work);
}

static struct nvmet_ns *__nvmet_find_namespace(struct nvmet_ctrl *ctrl,
		__le32 nsid)
{
	struct nvmet_ns *ns;

	list_for_each_entry_rcu(ns, &ctrl->subsys->namespaces, dev_link) {
		if (ns->nsid == le32_to_cpu(nsid))
			return ns;
	}

	return NULL;
}

struct nvmet_ns *nvmet_find_namespace(struct nvmet_ctrl *ctrl, __le32 nsid)
{
	struct nvmet_ns *ns;

	rcu_read_lock();
	ns = __nvmet_find_namespace(ctrl, nsid);
	if (ns)
		percpu_ref_get(&ns->ref);
	rcu_read_unlock();

	return ns;
}

static void nvmet_destroy_namespace(struct percpu_ref *ref)
{
	struct nvmet_ns *ns = container_of(ref, struct nvmet_ns, ref);

	complete(&ns->disable_done);
}

void nvmet_put_namespace(struct nvmet_ns *ns)
{
	percpu_ref_put(&ns->ref);
}

static void nvmet_ns_dev_disable(struct nvmet_ns *ns)
{
	nvmet_bdev_ns_disable(ns);
	nvmet_file_ns_disable(ns);
}

static int nvmet_p2pmem_ns_enable(struct nvmet_ns *ns)
{
	int ret;
	struct pci_dev *p2p_dev;

	if (!ns->use_p2pmem)
		return 0;

	if (!ns->bdev) {
		pr_err("peer-to-peer DMA is not supported by non-block device namespaces\n");
		return -EINVAL;
	}

	if (!blk_queue_pci_p2pdma(ns->bdev->bd_queue)) {
		pr_err("peer-to-peer DMA is not supported by the driver of %s\n",
		       ns->device_path);
		return -EINVAL;
	}

	if (ns->p2p_dev) {
		ret = pci_p2pdma_distance(ns->p2p_dev, nvmet_ns_dev(ns), true);
		if (ret < 0)
			return -EINVAL;
	} else {
		/*
		 * Right now we just check that there is p2pmem available so
		 * we can report an error to the user right away if there
		 * is not. We'll find the actual device to use once we
		 * setup the controller when the port's device is available.
		 */

		p2p_dev = pci_p2pmem_find(nvmet_ns_dev(ns));
		if (!p2p_dev) {
			pr_err("no peer-to-peer memory is available for %s\n",
			       ns->device_path);
			return -EINVAL;
		}

		pci_dev_put(p2p_dev);
	}

	return 0;
}

/*
 * Note: ctrl->subsys->lock should be held when calling this function
 */
static void nvmet_p2pmem_ns_add_p2p(struct nvmet_ctrl *ctrl,
				    struct nvmet_ns *ns)
{
	struct device *clients[2];
	struct pci_dev *p2p_dev;
	int ret;

	if (!ctrl->p2p_client || !ns->use_p2pmem)
		return;

	if (ns->p2p_dev) {
		ret = pci_p2pdma_distance(ns->p2p_dev, ctrl->p2p_client, true);
		if (ret < 0)
			return;

		p2p_dev = pci_dev_get(ns->p2p_dev);
	} else {
		clients[0] = ctrl->p2p_client;
		clients[1] = nvmet_ns_dev(ns);

		p2p_dev = pci_p2pmem_find_many(clients, ARRAY_SIZE(clients));
		if (!p2p_dev) {
			pr_err("no peer-to-peer memory is available that's supported by %s and %s\n",
			       dev_name(ctrl->p2p_client), ns->device_path);
			return;
		}
	}

	ret = radix_tree_insert(&ctrl->p2p_ns_map, ns->nsid, p2p_dev);
	if (ret < 0)
		pci_dev_put(p2p_dev);

	pr_info("using p2pmem on %s for nsid %d\n", pci_name(p2p_dev),
		ns->nsid);
}

int nvmet_ns_enable(struct nvmet_ns *ns)
{
	struct nvmet_subsys *subsys = ns->subsys;
	struct nvmet_ctrl *ctrl;
	int ret;

	mutex_lock(&subsys->lock);
	ret = 0;
	if (ns->enabled)
		goto out_unlock;

	ret = -EMFILE;
	if (subsys->nr_namespaces == NVMET_MAX_NAMESPACES)
		goto out_unlock;

	ret = nvmet_bdev_ns_enable(ns);
	if (ret == -ENOTBLK)
		ret = nvmet_file_ns_enable(ns);
	if (ret)
		goto out_unlock;

	ret = nvmet_p2pmem_ns_enable(ns);
	if (ret)
		goto out_dev_disable;

	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		nvmet_p2pmem_ns_add_p2p(ctrl, ns);

	ret = percpu_ref_init(&ns->ref, nvmet_destroy_namespace,
				0, GFP_KERNEL);
	if (ret)
		goto out_dev_put;

	if (ns->nsid > subsys->max_nsid)
		subsys->max_nsid = ns->nsid;

	/*
	 * The namespaces list needs to be sorted to simplify the implementation
	 * of the Identify Namepace List subcommand.
	 */
	if (list_empty(&subsys->namespaces)) {
		list_add_tail_rcu(&ns->dev_link, &subsys->namespaces);
	} else {
		struct nvmet_ns *old;

		list_for_each_entry_rcu(old, &subsys->namespaces, dev_link,
					lockdep_is_held(&subsys->lock)) {
			BUG_ON(ns->nsid == old->nsid);
			if (ns->nsid < old->nsid)
				break;
		}

		list_add_tail_rcu(&ns->dev_link, &old->dev_link);
	}
	subsys->nr_namespaces++;

	nvmet_ns_changed(subsys, ns->nsid);
	ns->enabled = true;
	ret = 0;
out_unlock:
	mutex_unlock(&subsys->lock);
	return ret;
out_dev_put:
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		pci_dev_put(radix_tree_delete(&ctrl->p2p_ns_map, ns->nsid));
out_dev_disable:
	nvmet_ns_dev_disable(ns);
	goto out_unlock;
}

void nvmet_ns_disable(struct nvmet_ns *ns)
{
	struct nvmet_subsys *subsys = ns->subsys;
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	if (!ns->enabled)
		goto out_unlock;

	ns->enabled = false;
	list_del_rcu(&ns->dev_link);
	if (ns->nsid == subsys->max_nsid)
		subsys->max_nsid = nvmet_max_nsid(subsys);

	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		pci_dev_put(radix_tree_delete(&ctrl->p2p_ns_map, ns->nsid));

	mutex_unlock(&subsys->lock);

	/*
	 * Now that we removed the namespaces from the lookup list, we
	 * can kill the per_cpu ref and wait for any remaining references
	 * to be dropped, as well as a RCU grace period for anyone only
	 * using the namepace under rcu_read_lock().  Note that we can't
	 * use call_rcu here as we need to ensure the namespaces have
	 * been fully destroyed before unloading the module.
	 */
	percpu_ref_kill(&ns->ref);
	synchronize_rcu();
	wait_for_completion(&ns->disable_done);
	percpu_ref_exit(&ns->ref);

	mutex_lock(&subsys->lock);

	subsys->nr_namespaces--;
	nvmet_ns_changed(subsys, ns->nsid);
	nvmet_ns_dev_disable(ns);
out_unlock:
	mutex_unlock(&subsys->lock);
}

void nvmet_ns_free(struct nvmet_ns *ns)
{
	nvmet_ns_disable(ns);

	down_write(&nvmet_ana_sem);
	nvmet_ana_group_enabled[ns->anagrpid]--;
	up_write(&nvmet_ana_sem);

	kfree(ns->device_path);
	kfree(ns);
}

struct nvmet_ns *nvmet_ns_alloc(struct nvmet_subsys *subsys, u32 nsid)
{
	struct nvmet_ns *ns;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	INIT_LIST_HEAD(&ns->dev_link);
	init_completion(&ns->disable_done);

	ns->nsid = nsid;
	ns->subsys = subsys;

	down_write(&nvmet_ana_sem);
	ns->anagrpid = NVMET_DEFAULT_ANA_GRPID;
	nvmet_ana_group_enabled[ns->anagrpid]++;
	up_write(&nvmet_ana_sem);

	uuid_gen(&ns->uuid);
	ns->buffered_io = false;

	return ns;
}

static void nvmet_update_sq_head(struct nvmet_req *req)
{
	if (req->sq->size) {
		u32 old_sqhd, new_sqhd;

		do {
			old_sqhd = req->sq->sqhd;
			new_sqhd = (old_sqhd + 1) % req->sq->size;
		} while (cmpxchg(&req->sq->sqhd, old_sqhd, new_sqhd) !=
					old_sqhd);
	}
	req->cqe->sq_head = cpu_to_le16(req->sq->sqhd & 0x0000FFFF);
}

static void nvmet_set_error(struct nvmet_req *req, u16 status)
{
	struct nvmet_ctrl *ctrl = req->sq->ctrl;
	struct nvme_error_slot *new_error_slot;
	unsigned long flags;

	req->cqe->status = cpu_to_le16(status << 1);

	if (!ctrl || req->error_loc == NVMET_NO_ERROR_LOC)
		return;

	spin_lock_irqsave(&ctrl->error_lock, flags);
	ctrl->err_counter++;
	new_error_slot =
		&ctrl->slots[ctrl->err_counter % NVMET_ERROR_LOG_SLOTS];

	new_error_slot->error_count = cpu_to_le64(ctrl->err_counter);
	new_error_slot->sqid = cpu_to_le16(req->sq->qid);
	new_error_slot->cmdid = cpu_to_le16(req->cmd->common.command_id);
	new_error_slot->status_field = cpu_to_le16(status << 1);
	new_error_slot->param_error_location = cpu_to_le16(req->error_loc);
	new_error_slot->lba = cpu_to_le64(req->error_slba);
	new_error_slot->nsid = req->cmd->common.nsid;
	spin_unlock_irqrestore(&ctrl->error_lock, flags);

	/* set the more bit for this request */
	req->cqe->status |= cpu_to_le16(1 << 14);
}

static void __nvmet_req_complete(struct nvmet_req *req, u16 status)
{
	if (!req->sq->sqhd_disabled)
		nvmet_update_sq_head(req);
	req->cqe->sq_id = cpu_to_le16(req->sq->qid);
	req->cqe->command_id = req->cmd->common.command_id;

	if (unlikely(status))
		nvmet_set_error(req, status);

	trace_nvmet_req_complete(req);

	if (req->ns)
		nvmet_put_namespace(req->ns);
	req->ops->queue_response(req);
}

void nvmet_req_complete(struct nvmet_req *req, u16 status)
{
	__nvmet_req_complete(req, status);
	percpu_ref_put(&req->sq->ref);
}
EXPORT_SYMBOL_GPL(nvmet_req_complete);

void nvmet_cq_setup(struct nvmet_ctrl *ctrl, struct nvmet_cq *cq,
		u16 qid, u16 size)
{
	cq->qid = qid;
	cq->size = size;

	ctrl->cqs[qid] = cq;
}

void nvmet_sq_setup(struct nvmet_ctrl *ctrl, struct nvmet_sq *sq,
		u16 qid, u16 size)
{
	sq->sqhd = 0;
	sq->qid = qid;
	sq->size = size;

	ctrl->sqs[qid] = sq;
}

static void nvmet_confirm_sq(struct percpu_ref *ref)
{
	struct nvmet_sq *sq = container_of(ref, struct nvmet_sq, ref);

	complete(&sq->confirm_done);
}

void nvmet_sq_destroy(struct nvmet_sq *sq)
{
	/*
	 * If this is the admin queue, complete all AERs so that our
	 * queue doesn't have outstanding requests on it.
	 */
	if (sq->ctrl && sq->ctrl->sqs && sq->ctrl->sqs[0] == sq)
		nvmet_async_events_free(sq->ctrl);
	percpu_ref_kill_and_confirm(&sq->ref, nvmet_confirm_sq);
	wait_for_completion(&sq->confirm_done);
	wait_for_completion(&sq->free_done);
	percpu_ref_exit(&sq->ref);

	if (sq->ctrl) {
		nvmet_ctrl_put(sq->ctrl);
		sq->ctrl = NULL; /* allows reusing the queue later */
	}
}
EXPORT_SYMBOL_GPL(nvmet_sq_destroy);

static void nvmet_sq_free(struct percpu_ref *ref)
{
	struct nvmet_sq *sq = container_of(ref, struct nvmet_sq, ref);

	complete(&sq->free_done);
}

int nvmet_sq_init(struct nvmet_sq *sq)
{
	int ret;

	ret = percpu_ref_init(&sq->ref, nvmet_sq_free, 0, GFP_KERNEL);
	if (ret) {
		pr_err("percpu_ref init failed!\n");
		return ret;
	}
	init_completion(&sq->free_done);
	init_completion(&sq->confirm_done);

	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_sq_init);

static inline u16 nvmet_check_ana_state(struct nvmet_port *port,
		struct nvmet_ns *ns)
{
	enum nvme_ana_state state = port->ana_state[ns->anagrpid];

	if (unlikely(state == NVME_ANA_INACCESSIBLE))
		return NVME_SC_ANA_INACCESSIBLE;
	if (unlikely(state == NVME_ANA_PERSISTENT_LOSS))
		return NVME_SC_ANA_PERSISTENT_LOSS;
	if (unlikely(state == NVME_ANA_CHANGE))
		return NVME_SC_ANA_TRANSITION;
	return 0;
}

static inline u16 nvmet_io_cmd_check_access(struct nvmet_req *req)
{
	if (unlikely(req->ns->readonly)) {
		switch (req->cmd->common.opcode) {
		case nvme_cmd_read:
		case nvme_cmd_flush:
			break;
		default:
			return NVME_SC_NS_WRITE_PROTECTED;
		}
	}

	return 0;
}

static u16 nvmet_parse_io_cmd(struct nvmet_req *req)
{
	struct nvme_command *cmd = req->cmd;
	u16 ret;

	ret = nvmet_check_ctrl_status(req, cmd);
	if (unlikely(ret))
		return ret;

	req->ns = nvmet_find_namespace(req->sq->ctrl, cmd->rw.nsid);
	if (unlikely(!req->ns)) {
		req->error_loc = offsetof(struct nvme_common_command, nsid);
		return NVME_SC_INVALID_NS | NVME_SC_DNR;
	}
	ret = nvmet_check_ana_state(req->port, req->ns);
	if (unlikely(ret)) {
		req->error_loc = offsetof(struct nvme_common_command, nsid);
		return ret;
	}
	ret = nvmet_io_cmd_check_access(req);
	if (unlikely(ret)) {
		req->error_loc = offsetof(struct nvme_common_command, nsid);
		return ret;
	}

	if (req->ns->file)
		return nvmet_file_parse_io_cmd(req);
	else
		return nvmet_bdev_parse_io_cmd(req);
}

bool nvmet_req_init(struct nvmet_req *req, struct nvmet_cq *cq,
		struct nvmet_sq *sq, const struct nvmet_fabrics_ops *ops)
{
	u8 flags = req->cmd->common.flags;
	u16 status;

	req->cq = cq;
	req->sq = sq;
	req->ops = ops;
	req->sg = NULL;
	req->sg_cnt = 0;
	req->transfer_len = 0;
	req->cqe->status = 0;
	req->cqe->sq_head = 0;
	req->ns = NULL;
	req->error_loc = NVMET_NO_ERROR_LOC;
	req->error_slba = 0;

	/* no support for fused commands yet */
	if (unlikely(flags & (NVME_CMD_FUSE_FIRST | NVME_CMD_FUSE_SECOND))) {
		req->error_loc = offsetof(struct nvme_common_command, flags);
		status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
		goto fail;
	}

	/*
	 * For fabrics, PSDT field shall describe metadata pointer (MPTR) that
	 * contains an address of a single contiguous physical buffer that is
	 * byte aligned.
	 */
	if (unlikely((flags & NVME_CMD_SGL_ALL) != NVME_CMD_SGL_METABUF)) {
		req->error_loc = offsetof(struct nvme_common_command, flags);
		status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
		goto fail;
	}

	if (unlikely(!req->sq->ctrl))
		/* will return an error for any Non-connect command: */
		status = nvmet_parse_connect_cmd(req);
	else if (likely(req->sq->qid != 0))
		status = nvmet_parse_io_cmd(req);
	else if (nvme_is_fabrics(req->cmd))
		status = nvmet_parse_fabrics_cmd(req);
	else if (req->sq->ctrl->subsys->type == NVME_NQN_DISC)
		status = nvmet_parse_discovery_cmd(req);
	else
		status = nvmet_parse_admin_cmd(req);

	if (status)
		goto fail;

	trace_nvmet_req_init(req, req->cmd);

	if (unlikely(!percpu_ref_tryget_live(&sq->ref))) {
		status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
		goto fail;
	}

	if (sq->ctrl)
		sq->ctrl->cmd_seen = true;

	return true;

fail:
	__nvmet_req_complete(req, status);
	return false;
}
EXPORT_SYMBOL_GPL(nvmet_req_init);

void nvmet_req_uninit(struct nvmet_req *req)
{
	percpu_ref_put(&req->sq->ref);
	if (req->ns)
		nvmet_put_namespace(req->ns);
}
EXPORT_SYMBOL_GPL(nvmet_req_uninit);

void nvmet_req_execute(struct nvmet_req *req)
{
	if (unlikely(req->data_len != req->transfer_len)) {
		req->error_loc = offsetof(struct nvme_common_command, dptr);
		nvmet_req_complete(req, NVME_SC_SGL_INVALID_DATA | NVME_SC_DNR);
	} else
		req->execute(req);
}
EXPORT_SYMBOL_GPL(nvmet_req_execute);

int nvmet_req_alloc_sgl(struct nvmet_req *req)
{
	struct pci_dev *p2p_dev = NULL;

	if (IS_ENABLED(CONFIG_PCI_P2PDMA)) {
		if (req->sq->ctrl && req->ns)
			p2p_dev = radix_tree_lookup(&req->sq->ctrl->p2p_ns_map,
						    req->ns->nsid);

		req->p2p_dev = NULL;
		if (req->sq->qid && p2p_dev) {
			req->sg = pci_p2pmem_alloc_sgl(p2p_dev, &req->sg_cnt,
						       req->transfer_len);
			if (req->sg) {
				req->p2p_dev = p2p_dev;
				return 0;
			}
		}

		/*
		 * If no P2P memory was available we fallback to using
		 * regular memory
		 */
	}

	req->sg = sgl_alloc(req->transfer_len, GFP_KERNEL, &req->sg_cnt);
	if (!req->sg)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_req_alloc_sgl);

void nvmet_req_free_sgl(struct nvmet_req *req)
{
	if (req->p2p_dev)
		pci_p2pmem_free_sgl(req->p2p_dev, req->sg);
	else
		sgl_free(req->sg);

	req->sg = NULL;
	req->sg_cnt = 0;
}
EXPORT_SYMBOL_GPL(nvmet_req_free_sgl);

static inline bool nvmet_cc_en(u32 cc)
{
	return (cc >> NVME_CC_EN_SHIFT) & 0x1;
}

static inline u8 nvmet_cc_css(u32 cc)
{
	return (cc >> NVME_CC_CSS_SHIFT) & 0x7;
}

static inline u8 nvmet_cc_mps(u32 cc)
{
	return (cc >> NVME_CC_MPS_SHIFT) & 0xf;
}

static inline u8 nvmet_cc_ams(u32 cc)
{
	return (cc >> NVME_CC_AMS_SHIFT) & 0x7;
}

static inline u8 nvmet_cc_shn(u32 cc)
{
	return (cc >> NVME_CC_SHN_SHIFT) & 0x3;
}

static inline u8 nvmet_cc_iosqes(u32 cc)
{
	return (cc >> NVME_CC_IOSQES_SHIFT) & 0xf;
}

static inline u8 nvmet_cc_iocqes(u32 cc)
{
	return (cc >> NVME_CC_IOCQES_SHIFT) & 0xf;
}

static void nvmet_start_ctrl(struct nvmet_ctrl *ctrl)
{
	lockdep_assert_held(&ctrl->lock);

	if (nvmet_cc_iosqes(ctrl->cc) != NVME_NVM_IOSQES ||
	    nvmet_cc_iocqes(ctrl->cc) != NVME_NVM_IOCQES ||
	    nvmet_cc_mps(ctrl->cc) != 0 ||
	    nvmet_cc_ams(ctrl->cc) != 0 ||
	    nvmet_cc_css(ctrl->cc) != 0) {
		ctrl->csts = NVME_CSTS_CFS;
		return;
	}

	ctrl->csts = NVME_CSTS_RDY;

	/*
	 * Controllers that are not yet enabled should not really enforce the
	 * keep alive timeout, but we still want to track a timeout and cleanup
	 * in case a host died before it enabled the controller.  Hence, simply
	 * reset the keep alive timer when the controller is enabled.
	 */
	if (ctrl->kato)
		mod_delayed_work(system_wq, &ctrl->ka_work, ctrl->kato * HZ);
}

static void nvmet_clear_ctrl(struct nvmet_ctrl *ctrl)
{
	lockdep_assert_held(&ctrl->lock);

	/* XXX: tear down queues? */
	ctrl->csts &= ~NVME_CSTS_RDY;
	ctrl->cc = 0;
}

void nvmet_update_cc(struct nvmet_ctrl *ctrl, u32 new)
{
	u32 old;

	mutex_lock(&ctrl->lock);
	old = ctrl->cc;
	ctrl->cc = new;

	if (nvmet_cc_en(new) && !nvmet_cc_en(old))
		nvmet_start_ctrl(ctrl);
	if (!nvmet_cc_en(new) && nvmet_cc_en(old))
		nvmet_clear_ctrl(ctrl);
	if (nvmet_cc_shn(new) && !nvmet_cc_shn(old)) {
		nvmet_clear_ctrl(ctrl);
		ctrl->csts |= NVME_CSTS_SHST_CMPLT;
	}
	if (!nvmet_cc_shn(new) && nvmet_cc_shn(old))
		ctrl->csts &= ~NVME_CSTS_SHST_CMPLT;
	mutex_unlock(&ctrl->lock);
}

static void nvmet_init_cap(struct nvmet_ctrl *ctrl)
{
	/* command sets supported: NVMe command set: */
	ctrl->cap = (1ULL << 37);
	/* CC.EN timeout in 500msec units: */
	ctrl->cap |= (15ULL << 24);
	/* maximum queue entries supported: */
	ctrl->cap |= NVMET_QUEUE_SIZE - 1;
}

u16 nvmet_ctrl_find_get(const char *subsysnqn, const char *hostnqn, u16 cntlid,
		struct nvmet_req *req, struct nvmet_ctrl **ret)
{
	struct nvmet_subsys *subsys;
	struct nvmet_ctrl *ctrl;
	u16 status = 0;

	subsys = nvmet_find_get_subsys(req->port, subsysnqn);
	if (!subsys) {
		pr_warn("connect request for invalid subsystem %s!\n",
			subsysnqn);
		req->cqe->result.u32 = IPO_IATTR_CONNECT_DATA(subsysnqn);
		return NVME_SC_CONNECT_INVALID_PARAM | NVME_SC_DNR;
	}

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (ctrl->cntlid == cntlid) {
			if (strncmp(hostnqn, ctrl->hostnqn, NVMF_NQN_SIZE)) {
				pr_warn("hostnqn mismatch.\n");
				continue;
			}
			if (!kref_get_unless_zero(&ctrl->ref))
				continue;

			*ret = ctrl;
			goto out;
		}
	}

	pr_warn("could not find controller %d for subsys %s / host %s\n",
		cntlid, subsysnqn, hostnqn);
	req->cqe->result.u32 = IPO_IATTR_CONNECT_DATA(cntlid);
	status = NVME_SC_CONNECT_INVALID_PARAM | NVME_SC_DNR;

out:
	mutex_unlock(&subsys->lock);
	nvmet_subsys_put(subsys);
	return status;
}

u16 nvmet_check_ctrl_status(struct nvmet_req *req, struct nvme_command *cmd)
{
	if (unlikely(!(req->sq->ctrl->cc & NVME_CC_ENABLE))) {
		pr_err("got cmd %d while CC.EN == 0 on qid = %d\n",
		       cmd->common.opcode, req->sq->qid);
		return NVME_SC_CMD_SEQ_ERROR | NVME_SC_DNR;
	}

	if (unlikely(!(req->sq->ctrl->csts & NVME_CSTS_RDY))) {
		pr_err("got cmd %d while CSTS.RDY == 0 on qid = %d\n",
		       cmd->common.opcode, req->sq->qid);
		return NVME_SC_CMD_SEQ_ERROR | NVME_SC_DNR;
	}
	return 0;
}

bool nvmet_host_allowed(struct nvmet_subsys *subsys, const char *hostnqn)
{
	struct nvmet_host_link *p;

	lockdep_assert_held(&nvmet_config_sem);

	if (subsys->allow_any_host)
		return true;

	if (subsys->type == NVME_NQN_DISC) /* allow all access to disc subsys */
		return true;

	list_for_each_entry(p, &subsys->hosts, entry) {
		if (!strcmp(nvmet_host_name(p->host), hostnqn))
			return true;
	}

	return false;
}

/*
 * Note: ctrl->subsys->lock should be held when calling this function
 */
static void nvmet_setup_p2p_ns_map(struct nvmet_ctrl *ctrl,
		struct nvmet_req *req)
{
	struct nvmet_ns *ns;

	if (!req->p2p_client)
		return;

	ctrl->p2p_client = get_device(req->p2p_client);

	list_for_each_entry_rcu(ns, &ctrl->subsys->namespaces, dev_link,
				lockdep_is_held(&ctrl->subsys->lock))
		nvmet_p2pmem_ns_add_p2p(ctrl, ns);
}

/*
 * Note: ctrl->subsys->lock should be held when calling this function
 */
static void nvmet_release_p2p_ns_map(struct nvmet_ctrl *ctrl)
{
	struct radix_tree_iter iter;
	void __rcu **slot;

	radix_tree_for_each_slot(slot, &ctrl->p2p_ns_map, &iter, 0)
		pci_dev_put(radix_tree_deref_slot(slot));

	put_device(ctrl->p2p_client);
}

static void nvmet_fatal_error_handler(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl =
			container_of(work, struct nvmet_ctrl, fatal_err_work);

	pr_err("ctrl %d fatal error occurred!\n", ctrl->cntlid);
	ctrl->ops->delete_ctrl(ctrl);
}

u16 nvmet_alloc_ctrl(const char *subsysnqn, const char *hostnqn,
		struct nvmet_req *req, u32 kato, struct nvmet_ctrl **ctrlp)
{
	struct nvmet_subsys *subsys;
	struct nvmet_ctrl *ctrl;
	int ret;
	u16 status;

	status = NVME_SC_CONNECT_INVALID_PARAM | NVME_SC_DNR;
	subsys = nvmet_find_get_subsys(req->port, subsysnqn);
	if (!subsys) {
		pr_warn("connect request for invalid subsystem %s!\n",
			subsysnqn);
		req->cqe->result.u32 = IPO_IATTR_CONNECT_DATA(subsysnqn);
		goto out;
	}

	status = NVME_SC_CONNECT_INVALID_PARAM | NVME_SC_DNR;
	down_read(&nvmet_config_sem);
	if (!nvmet_host_allowed(subsys, hostnqn)) {
		pr_info("connect by host %s for subsystem %s not allowed\n",
			hostnqn, subsysnqn);
		req->cqe->result.u32 = IPO_IATTR_CONNECT_DATA(hostnqn);
		up_read(&nvmet_config_sem);
		status = NVME_SC_CONNECT_INVALID_HOST | NVME_SC_DNR;
		goto out_put_subsystem;
	}
	up_read(&nvmet_config_sem);

	status = NVME_SC_INTERNAL;
	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		goto out_put_subsystem;
	mutex_init(&ctrl->lock);

	nvmet_init_cap(ctrl);

	ctrl->port = req->port;

	INIT_WORK(&ctrl->async_event_work, nvmet_async_event_work);
	INIT_LIST_HEAD(&ctrl->async_events);
	INIT_RADIX_TREE(&ctrl->p2p_ns_map, GFP_KERNEL);
	INIT_WORK(&ctrl->fatal_err_work, nvmet_fatal_error_handler);

	memcpy(ctrl->subsysnqn, subsysnqn, NVMF_NQN_SIZE);
	memcpy(ctrl->hostnqn, hostnqn, NVMF_NQN_SIZE);

	kref_init(&ctrl->ref);
	ctrl->subsys = subsys;
	WRITE_ONCE(ctrl->aen_enabled, NVMET_AEN_CFG_OPTIONAL);

	ctrl->changed_ns_list = kmalloc_array(NVME_MAX_CHANGED_NAMESPACES,
			sizeof(__le32), GFP_KERNEL);
	if (!ctrl->changed_ns_list)
		goto out_free_ctrl;

	ctrl->cqs = kcalloc(subsys->max_qid + 1,
			sizeof(struct nvmet_cq *),
			GFP_KERNEL);
	if (!ctrl->cqs)
		goto out_free_changed_ns_list;

	ctrl->sqs = kcalloc(subsys->max_qid + 1,
			sizeof(struct nvmet_sq *),
			GFP_KERNEL);
	if (!ctrl->sqs)
		goto out_free_cqs;

	ret = ida_simple_get(&cntlid_ida,
			     NVME_CNTLID_MIN, NVME_CNTLID_MAX,
			     GFP_KERNEL);
	if (ret < 0) {
		status = NVME_SC_CONNECT_CTRL_BUSY | NVME_SC_DNR;
		goto out_free_sqs;
	}
	ctrl->cntlid = ret;

	ctrl->ops = req->ops;

	/*
	 * Discovery controllers may use some arbitrary high value
	 * in order to cleanup stale discovery sessions
	 */
	if ((ctrl->subsys->type == NVME_NQN_DISC) && !kato)
		kato = NVMET_DISC_KATO_MS;

	/* keep-alive timeout in seconds */
	ctrl->kato = DIV_ROUND_UP(kato, 1000);

	ctrl->err_counter = 0;
	spin_lock_init(&ctrl->error_lock);

	nvmet_start_keep_alive_timer(ctrl);

	mutex_lock(&subsys->lock);
	list_add_tail(&ctrl->subsys_entry, &subsys->ctrls);
	nvmet_setup_p2p_ns_map(ctrl, req);
	mutex_unlock(&subsys->lock);

	*ctrlp = ctrl;
	return 0;

out_free_sqs:
	kfree(ctrl->sqs);
out_free_cqs:
	kfree(ctrl->cqs);
out_free_changed_ns_list:
	kfree(ctrl->changed_ns_list);
out_free_ctrl:
	kfree(ctrl);
out_put_subsystem:
	nvmet_subsys_put(subsys);
out:
	return status;
}

static void nvmet_ctrl_free(struct kref *ref)
{
	struct nvmet_ctrl *ctrl = container_of(ref, struct nvmet_ctrl, ref);
	struct nvmet_subsys *subsys = ctrl->subsys;

	mutex_lock(&subsys->lock);
	nvmet_release_p2p_ns_map(ctrl);
	list_del(&ctrl->subsys_entry);
	mutex_unlock(&subsys->lock);

	nvmet_stop_keep_alive_timer(ctrl);

	flush_work(&ctrl->async_event_work);
	cancel_work_sync(&ctrl->fatal_err_work);

	ida_simple_remove(&cntlid_ida, ctrl->cntlid);

	kfree(ctrl->sqs);
	kfree(ctrl->cqs);
	kfree(ctrl->changed_ns_list);
	kfree(ctrl);

	nvmet_subsys_put(subsys);
}

void nvmet_ctrl_put(struct nvmet_ctrl *ctrl)
{
	kref_put(&ctrl->ref, nvmet_ctrl_free);
}

void nvmet_ctrl_fatal_error(struct nvmet_ctrl *ctrl)
{
	mutex_lock(&ctrl->lock);
	if (!(ctrl->csts & NVME_CSTS_CFS)) {
		ctrl->csts |= NVME_CSTS_CFS;
		schedule_work(&ctrl->fatal_err_work);
	}
	mutex_unlock(&ctrl->lock);
}
EXPORT_SYMBOL_GPL(nvmet_ctrl_fatal_error);

static struct nvmet_subsys *nvmet_find_get_subsys(struct nvmet_port *port,
		const char *subsysnqn)
{
	struct nvmet_subsys_link *p;

	if (!port)
		return NULL;

	if (!strcmp(NVME_DISC_SUBSYS_NAME, subsysnqn)) {
		if (!kref_get_unless_zero(&nvmet_disc_subsys->ref))
			return NULL;
		return nvmet_disc_subsys;
	}

	down_read(&nvmet_config_sem);
	list_for_each_entry(p, &port->subsystems, entry) {
		if (!strncmp(p->subsys->subsysnqn, subsysnqn,
				NVMF_NQN_SIZE)) {
			if (!kref_get_unless_zero(&p->subsys->ref))
				break;
			up_read(&nvmet_config_sem);
			return p->subsys;
		}
	}
	up_read(&nvmet_config_sem);
	return NULL;
}

struct nvmet_subsys *nvmet_subsys_alloc(const char *subsysnqn,
		enum nvme_subsys_type type)
{
	struct nvmet_subsys *subsys;

	subsys = kzalloc(sizeof(*subsys), GFP_KERNEL);
	if (!subsys)
		return ERR_PTR(-ENOMEM);

	subsys->ver = NVME_VS(1, 3, 0); /* NVMe 1.3.0 */
	/* generate a random serial number as our controllers are ephemeral: */
	get_random_bytes(&subsys->serial, sizeof(subsys->serial));

	switch (type) {
	case NVME_NQN_NVME:
		subsys->max_qid = NVMET_NR_QUEUES;
		break;
	case NVME_NQN_DISC:
		subsys->max_qid = 0;
		break;
	default:
		pr_err("%s: Unknown Subsystem type - %d\n", __func__, type);
		kfree(subsys);
		return ERR_PTR(-EINVAL);
	}
	subsys->type = type;
	subsys->subsysnqn = kstrndup(subsysnqn, NVMF_NQN_SIZE,
			GFP_KERNEL);
	if (!subsys->subsysnqn) {
		kfree(subsys);
		return ERR_PTR(-ENOMEM);
	}

	kref_init(&subsys->ref);

	mutex_init(&subsys->lock);
	INIT_LIST_HEAD(&subsys->namespaces);
	INIT_LIST_HEAD(&subsys->ctrls);
	INIT_LIST_HEAD(&subsys->hosts);

	return subsys;
}

static void nvmet_subsys_free(struct kref *ref)
{
	struct nvmet_subsys *subsys =
		container_of(ref, struct nvmet_subsys, ref);

	WARN_ON_ONCE(!list_empty(&subsys->namespaces));

	kfree(subsys->subsysnqn);
	kfree(subsys);
}

void nvmet_subsys_del_ctrls(struct nvmet_subsys *subsys)
{
	struct nvmet_ctrl *ctrl;

	mutex_lock(&subsys->lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		ctrl->ops->delete_ctrl(ctrl);
	mutex_unlock(&subsys->lock);
}

void nvmet_subsys_put(struct nvmet_subsys *subsys)
{
	kref_put(&subsys->ref, nvmet_subsys_free);
}

static int __init nvmet_init(void)
{
	int error;

	nvmet_ana_group_enabled[NVMET_DEFAULT_ANA_GRPID] = 1;

	buffered_io_wq = alloc_workqueue("nvmet-buffered-io-wq",
			WQ_MEM_RECLAIM, 0);
	if (!buffered_io_wq) {
		error = -ENOMEM;
		goto out;
	}

	error = nvmet_init_discovery();
	if (error)
		goto out_free_work_queue;

	error = nvmet_init_configfs();
	if (error)
		goto out_exit_discovery;
	return 0;

out_exit_discovery:
	nvmet_exit_discovery();
out_free_work_queue:
	destroy_workqueue(buffered_io_wq);
out:
	return error;
}

static void __exit nvmet_exit(void)
{
	nvmet_exit_configfs();
	nvmet_exit_discovery();
	ida_destroy(&cntlid_ida);
	destroy_workqueue(buffered_io_wq);

	BUILD_BUG_ON(sizeof(struct nvmf_disc_rsp_page_entry) != 1024);
	BUILD_BUG_ON(sizeof(struct nvmf_disc_rsp_page_hdr) != 1024);
}

module_init(nvmet_init);
module_exit(nvmet_exit);

MODULE_LICENSE("GPL v2");
