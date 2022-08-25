// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2009, Microsoft Corporation.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hyperv.h>
#include <asm/mshyperv.h>

#include "hyperv_vmbus.h"

static void init_vp_index(struct vmbus_channel *channel, u16 dev_type);

static const struct vmbus_device vmbus_devs[] = {
	/* IDE */
	{ .dev_type = HV_IDE,
	  HV_IDE_GUID,
	  .perf_device = true,
	},

	/* SCSI */
	{ .dev_type = HV_SCSI,
	  HV_SCSI_GUID,
	  .perf_device = true,
	},

	/* Fibre Channel */
	{ .dev_type = HV_FC,
	  HV_SYNTHFC_GUID,
	  .perf_device = true,
	},

	/* Synthetic NIC */
	{ .dev_type = HV_NIC,
	  HV_NIC_GUID,
	  .perf_device = true,
	},

	/* Network Direct */
	{ .dev_type = HV_ND,
	  HV_ND_GUID,
	  .perf_device = true,
	},

	/* PCIE */
	{ .dev_type = HV_PCIE,
	  HV_PCIE_GUID,
	  .perf_device = false,
	},

	/* Synthetic Frame Buffer */
	{ .dev_type = HV_FB,
	  HV_SYNTHVID_GUID,
	  .perf_device = false,
	},

	/* Synthetic Keyboard */
	{ .dev_type = HV_KBD,
	  HV_KBD_GUID,
	  .perf_device = false,
	},

	/* Synthetic MOUSE */
	{ .dev_type = HV_MOUSE,
	  HV_MOUSE_GUID,
	  .perf_device = false,
	},

	/* KVP */
	{ .dev_type = HV_KVP,
	  HV_KVP_GUID,
	  .perf_device = false,
	},

	/* Time Synch */
	{ .dev_type = HV_TS,
	  HV_TS_GUID,
	  .perf_device = false,
	},

	/* Heartbeat */
	{ .dev_type = HV_HB,
	  HV_HEART_BEAT_GUID,
	  .perf_device = false,
	},

	/* Shutdown */
	{ .dev_type = HV_SHUTDOWN,
	  HV_SHUTDOWN_GUID,
	  .perf_device = false,
	},

	/* File copy */
	{ .dev_type = HV_FCOPY,
	  HV_FCOPY_GUID,
	  .perf_device = false,
	},

	/* Backup */
	{ .dev_type = HV_BACKUP,
	  HV_VSS_GUID,
	  .perf_device = false,
	},

	/* Dynamic Memory */
	{ .dev_type = HV_DM,
	  HV_DM_GUID,
	  .perf_device = false,
	},

	/* Unknown GUID */
	{ .dev_type = HV_UNKNOWN,
	  .perf_device = false,
	},
};

static const struct {
	guid_t guid;
} vmbus_unsupported_devs[] = {
	{ HV_AVMA1_GUID },
	{ HV_AVMA2_GUID },
	{ HV_RDV_GUID	},
};

/*
 * The rescinded channel may be blocked waiting for a response from the host;
 * take care of that.
 */
static void vmbus_rescind_cleanup(struct vmbus_channel *channel)
{
	struct vmbus_channel_msginfo *msginfo;
	unsigned long flags;


	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);
	channel->rescind = true;
	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {

		if (msginfo->waiting_channel == channel) {
			complete(&msginfo->waitevent);
			break;
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

static bool is_unsupported_vmbus_devs(const guid_t *guid)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vmbus_unsupported_devs); i++)
		if (guid_equal(guid, &vmbus_unsupported_devs[i].guid))
			return true;
	return false;
}

static u16 hv_get_dev_type(const struct vmbus_channel *channel)
{
	const guid_t *guid = &channel->offermsg.offer.if_type;
	u16 i;

	if (is_hvsock_channel(channel) || is_unsupported_vmbus_devs(guid))
		return HV_UNKNOWN;

	for (i = HV_IDE; i < HV_UNKNOWN; i++) {
		if (guid_equal(guid, &vmbus_devs[i].guid))
			return i;
	}
	pr_info("Unknown GUID: %pUl\n", guid);
	return i;
}

/**
 * vmbus_prep_negotiate_resp() - Create default response for Negotiate message
 * @icmsghdrp: Pointer to msg header structure
 * @buf: Raw buffer channel data
 * @fw_version: The framework versions we can support.
 * @fw_vercnt: The size of @fw_version.
 * @srv_version: The service versions we can support.
 * @srv_vercnt: The size of @srv_version.
 * @nego_fw_version: The selected framework version.
 * @nego_srv_version: The selected service version.
 *
 * Note: Versions are given in decreasing order.
 *
 * Set up and fill in default negotiate response message.
 * Mainly used by Hyper-V drivers.
 */
bool vmbus_prep_negotiate_resp(struct icmsg_hdr *icmsghdrp,
				u8 *buf, const int *fw_version, int fw_vercnt,
				const int *srv_version, int srv_vercnt,
				int *nego_fw_version, int *nego_srv_version)
{
	int icframe_major, icframe_minor;
	int icmsg_major, icmsg_minor;
	int fw_major, fw_minor;
	int srv_major, srv_minor;
	int i, j;
	bool found_match = false;
	struct icmsg_negotiate *negop;

	icmsghdrp->icmsgsize = 0x10;
	negop = (struct icmsg_negotiate *)&buf[
		sizeof(struct vmbuspipe_hdr) +
		sizeof(struct icmsg_hdr)];

	icframe_major = negop->icframe_vercnt;
	icframe_minor = 0;

	icmsg_major = negop->icmsg_vercnt;
	icmsg_minor = 0;

	/*
	 * Select the framework version number we will
	 * support.
	 */

	for (i = 0; i < fw_vercnt; i++) {
		fw_major = (fw_version[i] >> 16);
		fw_minor = (fw_version[i] & 0xFFFF);

		for (j = 0; j < negop->icframe_vercnt; j++) {
			if ((negop->icversion_data[j].major == fw_major) &&
			    (negop->icversion_data[j].minor == fw_minor)) {
				icframe_major = negop->icversion_data[j].major;
				icframe_minor = negop->icversion_data[j].minor;
				found_match = true;
				break;
			}
		}

		if (found_match)
			break;
	}

	if (!found_match)
		goto fw_error;

	found_match = false;

	for (i = 0; i < srv_vercnt; i++) {
		srv_major = (srv_version[i] >> 16);
		srv_minor = (srv_version[i] & 0xFFFF);

		for (j = negop->icframe_vercnt;
			(j < negop->icframe_vercnt + negop->icmsg_vercnt);
			j++) {

			if ((negop->icversion_data[j].major == srv_major) &&
				(negop->icversion_data[j].minor == srv_minor)) {

				icmsg_major = negop->icversion_data[j].major;
				icmsg_minor = negop->icversion_data[j].minor;
				found_match = true;
				break;
			}
		}

		if (found_match)
			break;
	}

	/*
	 * Respond with the framework and service
	 * version numbers we can support.
	 */

fw_error:
	if (!found_match) {
		negop->icframe_vercnt = 0;
		negop->icmsg_vercnt = 0;
	} else {
		negop->icframe_vercnt = 1;
		negop->icmsg_vercnt = 1;
	}

	if (nego_fw_version)
		*nego_fw_version = (icframe_major << 16) | icframe_minor;

	if (nego_srv_version)
		*nego_srv_version = (icmsg_major << 16) | icmsg_minor;

	negop->icversion_data[0].major = icframe_major;
	negop->icversion_data[0].minor = icframe_minor;
	negop->icversion_data[1].major = icmsg_major;
	negop->icversion_data[1].minor = icmsg_minor;
	return found_match;
}

EXPORT_SYMBOL_GPL(vmbus_prep_negotiate_resp);

/*
 * alloc_channel - Allocate and initialize a vmbus channel object
 */
static struct vmbus_channel *alloc_channel(void)
{
	struct vmbus_channel *channel;

	channel = kzalloc(sizeof(*channel), GFP_ATOMIC);
	if (!channel)
		return NULL;

	spin_lock_init(&channel->lock);
	init_completion(&channel->rescind_event);

	INIT_LIST_HEAD(&channel->sc_list);
	INIT_LIST_HEAD(&channel->percpu_list);

	tasklet_init(&channel->callback_event,
		     vmbus_on_event, (unsigned long)channel);

	hv_ringbuffer_pre_init(channel);

	return channel;
}

/*
 * free_channel - Release the resources used by the vmbus channel object
 */
static void free_channel(struct vmbus_channel *channel)
{
	tasklet_kill(&channel->callback_event);
	vmbus_remove_channel_attr_group(channel);

	kobject_put(&channel->kobj);
}

static void percpu_channel_enq(void *arg)
{
	struct vmbus_channel *channel = arg;
	struct hv_per_cpu_context *hv_cpu
		= this_cpu_ptr(hv_context.cpu_context);

	list_add_tail_rcu(&channel->percpu_list, &hv_cpu->chan_list);
}

static void percpu_channel_deq(void *arg)
{
	struct vmbus_channel *channel = arg;

	list_del_rcu(&channel->percpu_list);
}


static void vmbus_release_relid(u32 relid)
{
	struct vmbus_channel_relid_released msg;
	int ret;

	memset(&msg, 0, sizeof(struct vmbus_channel_relid_released));
	msg.child_relid = relid;
	msg.header.msgtype = CHANNELMSG_RELID_RELEASED;
	ret = vmbus_post_msg(&msg, sizeof(struct vmbus_channel_relid_released),
			     true);

	trace_vmbus_release_relid(&msg, ret);
}

void hv_process_channel_removal(struct vmbus_channel *channel)
{
	struct vmbus_channel *primary_channel;
	unsigned long flags;

	BUG_ON(!mutex_is_locked(&vmbus_connection.channel_mutex));
	BUG_ON(!channel->rescind);

	if (channel->target_cpu != get_cpu()) {
		put_cpu();
		smp_call_function_single(channel->target_cpu,
					 percpu_channel_deq, channel, true);
	} else {
		percpu_channel_deq(channel);
		put_cpu();
	}

	if (channel->primary_channel == NULL) {
		list_del(&channel->listentry);

		primary_channel = channel;
	} else {
		primary_channel = channel->primary_channel;
		spin_lock_irqsave(&primary_channel->lock, flags);
		list_del(&channel->sc_list);
		spin_unlock_irqrestore(&primary_channel->lock, flags);
	}

	/*
	 * We need to free the bit for init_vp_index() to work in the case
	 * of sub-channel, when we reload drivers like hv_netvsc.
	 */
	if (channel->affinity_policy == HV_LOCALIZED)
		cpumask_clear_cpu(channel->target_cpu,
				  &primary_channel->alloced_cpus_in_node);

	/*
	 * Upon suspend, an in-use hv_sock channel is marked as "rescinded" and
	 * the relid is invalidated; after hibernation, when the user-space app
	 * destroys the channel, the relid is INVALID_RELID, and in this case
	 * it's unnecessary and unsafe to release the old relid, since the same
	 * relid can refer to a completely different channel now.
	 */
	if (channel->offermsg.child_relid != INVALID_RELID)
		vmbus_release_relid(channel->offermsg.child_relid);

	free_channel(channel);
}

void vmbus_free_channels(void)
{
	struct vmbus_channel *channel, *tmp;

	list_for_each_entry_safe(channel, tmp, &vmbus_connection.chn_list,
		listentry) {
		/* hv_process_channel_removal() needs this */
		channel->rescind = true;

		vmbus_device_unregister(channel->device_obj);
	}
}

/* Note: the function can run concurrently for primary/sub channels. */
static void vmbus_add_channel_work(struct work_struct *work)
{
	struct vmbus_channel *newchannel =
		container_of(work, struct vmbus_channel, add_channel_work);
	struct vmbus_channel *primary_channel = newchannel->primary_channel;
	unsigned long flags;
	u16 dev_type;
	int ret;

	dev_type = hv_get_dev_type(newchannel);

	init_vp_index(newchannel, dev_type);

	if (newchannel->target_cpu != get_cpu()) {
		put_cpu();
		smp_call_function_single(newchannel->target_cpu,
					 percpu_channel_enq,
					 newchannel, true);
	} else {
		percpu_channel_enq(newchannel);
		put_cpu();
	}

	/*
	 * This state is used to indicate a successful open
	 * so that when we do close the channel normally, we
	 * can cleanup properly.
	 */
	newchannel->state = CHANNEL_OPEN_STATE;

	if (primary_channel != NULL) {
		/* newchannel is a sub-channel. */
		struct hv_device *dev = primary_channel->device_obj;

		if (vmbus_add_channel_kobj(dev, newchannel))
			goto err_deq_chan;

		if (primary_channel->sc_creation_callback != NULL)
			primary_channel->sc_creation_callback(newchannel);

		newchannel->probe_done = true;
		return;
	}

	/*
	 * Start the process of binding the primary channel to the driver
	 */
	newchannel->device_obj = vmbus_device_create(
		&newchannel->offermsg.offer.if_type,
		&newchannel->offermsg.offer.if_instance,
		newchannel);
	if (!newchannel->device_obj)
		goto err_deq_chan;

	newchannel->device_obj->device_id = dev_type;
	/*
	 * Add the new device to the bus. This will kick off device-driver
	 * binding which eventually invokes the device driver's AddDevice()
	 * method.
	 */
	ret = vmbus_device_register(newchannel->device_obj);

	if (ret != 0) {
		pr_err("unable to add child device object (relid %d)\n",
			newchannel->offermsg.child_relid);
		kfree(newchannel->device_obj);
		goto err_deq_chan;
	}

	newchannel->probe_done = true;
	return;

err_deq_chan:
	mutex_lock(&vmbus_connection.channel_mutex);

	/*
	 * We need to set the flag, otherwise
	 * vmbus_onoffer_rescind() can be blocked.
	 */
	newchannel->probe_done = true;

	if (primary_channel == NULL) {
		list_del(&newchannel->listentry);
	} else {
		spin_lock_irqsave(&primary_channel->lock, flags);
		list_del(&newchannel->sc_list);
		spin_unlock_irqrestore(&primary_channel->lock, flags);
	}

	mutex_unlock(&vmbus_connection.channel_mutex);

	if (newchannel->target_cpu != get_cpu()) {
		put_cpu();
		smp_call_function_single(newchannel->target_cpu,
					 percpu_channel_deq,
					 newchannel, true);
	} else {
		percpu_channel_deq(newchannel);
		put_cpu();
	}

	vmbus_release_relid(newchannel->offermsg.child_relid);

	free_channel(newchannel);
}

/*
 * vmbus_process_offer - Process the offer by creating a channel/device
 * associated with this offer
 */
static void vmbus_process_offer(struct vmbus_channel *newchannel)
{
	struct vmbus_channel *channel;
	struct workqueue_struct *wq;
	unsigned long flags;
	bool fnew = true;

	mutex_lock(&vmbus_connection.channel_mutex);

	/* Remember the channels that should be cleaned up upon suspend. */
	if (is_hvsock_channel(newchannel) || is_sub_channel(newchannel))
		atomic_inc(&vmbus_connection.nr_chan_close_on_suspend);

	/*
	 * Now that we have acquired the channel_mutex,
	 * we can release the potentially racing rescind thread.
	 */
	atomic_dec(&vmbus_connection.offer_in_progress);

	list_for_each_entry(channel, &vmbus_connection.chn_list, listentry) {
		if (guid_equal(&channel->offermsg.offer.if_type,
			       &newchannel->offermsg.offer.if_type) &&
		    guid_equal(&channel->offermsg.offer.if_instance,
			       &newchannel->offermsg.offer.if_instance)) {
			fnew = false;
			break;
		}
	}

	if (fnew)
		list_add_tail(&newchannel->listentry,
			      &vmbus_connection.chn_list);
	else {
		/*
		 * Check to see if this is a valid sub-channel.
		 */
		if (newchannel->offermsg.offer.sub_channel_index == 0) {
			mutex_unlock(&vmbus_connection.channel_mutex);
			/*
			 * Don't call free_channel(), because newchannel->kobj
			 * is not initialized yet.
			 */
			kfree(newchannel);
			WARN_ON_ONCE(1);
			return;
		}
		/*
		 * Process the sub-channel.
		 */
		newchannel->primary_channel = channel;
		spin_lock_irqsave(&channel->lock, flags);
		list_add_tail(&newchannel->sc_list, &channel->sc_list);
		spin_unlock_irqrestore(&channel->lock, flags);
	}

	mutex_unlock(&vmbus_connection.channel_mutex);

	/*
	 * vmbus_process_offer() mustn't call channel->sc_creation_callback()
	 * directly for sub-channels, because sc_creation_callback() ->
	 * vmbus_open() may never get the host's response to the
	 * OPEN_CHANNEL message (the host may rescind a channel at any time,
	 * e.g. in the case of hot removing a NIC), and vmbus_onoffer_rescind()
	 * may not wake up the vmbus_open() as it's blocked due to a non-zero
	 * vmbus_connection.offer_in_progress, and finally we have a deadlock.
	 *
	 * The above is also true for primary channels, if the related device
	 * drivers use sync probing mode by default.
	 *
	 * And, usually the handling of primary channels and sub-channels can
	 * depend on each other, so we should offload them to different
	 * workqueues to avoid possible deadlock, e.g. in sync-probing mode,
	 * NIC1's netvsc_subchan_work() can race with NIC2's netvsc_probe() ->
	 * rtnl_lock(), and causes deadlock: the former gets the rtnl_lock
	 * and waits for all the sub-channels to appear, but the latter
	 * can't get the rtnl_lock and this blocks the handling of
	 * sub-channels.
	 */
	INIT_WORK(&newchannel->add_channel_work, vmbus_add_channel_work);
	wq = fnew ? vmbus_connection.handle_primary_chan_wq :
		    vmbus_connection.handle_sub_chan_wq;
	queue_work(wq, &newchannel->add_channel_work);
}

/*
 * We use this state to statically distribute the channel interrupt load.
 */
static int next_numa_node_id;
/*
 * init_vp_index() accesses global variables like next_numa_node_id, and
 * it can run concurrently for primary channels and sub-channels: see
 * vmbus_process_offer(), so we need the lock to protect the global
 * variables.
 */
static DEFINE_SPINLOCK(bind_channel_to_cpu_lock);

/*
 * Starting with Win8, we can statically distribute the incoming
 * channel interrupt load by binding a channel to VCPU.
 * We distribute the interrupt loads to one or more NUMA nodes based on
 * the channel's affinity_policy.
 *
 * For pre-win8 hosts or non-performance critical channels we assign the
 * first CPU in the first NUMA node.
 */
static void init_vp_index(struct vmbus_channel *channel, u16 dev_type)
{
	u32 cur_cpu;
	bool perf_chn = vmbus_devs[dev_type].perf_device;
	struct vmbus_channel *primary = channel->primary_channel;
	int next_node;
	cpumask_var_t available_mask;
	struct cpumask *alloced_mask;

	if ((vmbus_proto_version == VERSION_WS2008) ||
	    (vmbus_proto_version == VERSION_WIN7) || (!perf_chn) ||
	    !alloc_cpumask_var(&available_mask, GFP_KERNEL)) {
		/*
		 * Prior to win8, all channel interrupts are
		 * delivered on cpu 0.
		 * Also if the channel is not a performance critical
		 * channel, bind it to cpu 0.
		 * In case alloc_cpumask_var() fails, bind it to cpu 0.
		 */
		channel->numa_node = 0;
		channel->target_cpu = 0;
		channel->target_vp = hv_cpu_number_to_vp_number(0);
		return;
	}

	spin_lock(&bind_channel_to_cpu_lock);

	/*
	 * Based on the channel affinity policy, we will assign the NUMA
	 * nodes.
	 */

	if ((channel->affinity_policy == HV_BALANCED) || (!primary)) {
		while (true) {
			next_node = next_numa_node_id++;
			if (next_node == nr_node_ids) {
				next_node = next_numa_node_id = 0;
				continue;
			}
			if (cpumask_empty(cpumask_of_node(next_node)))
				continue;
			break;
		}
		channel->numa_node = next_node;
		primary = channel;
	}
	alloced_mask = &hv_context.hv_numa_map[primary->numa_node];

	if (cpumask_weight(alloced_mask) ==
	    cpumask_weight(cpumask_of_node(primary->numa_node))) {
		/*
		 * We have cycled through all the CPUs in the node;
		 * reset the alloced map.
		 */
		cpumask_clear(alloced_mask);
	}

	cpumask_xor(available_mask, alloced_mask,
		    cpumask_of_node(primary->numa_node));

	cur_cpu = -1;

	if (primary->affinity_policy == HV_LOCALIZED) {
		/*
		 * Normally Hyper-V host doesn't create more subchannels
		 * than there are VCPUs on the node but it is possible when not
		 * all present VCPUs on the node are initialized by guest.
		 * Clear the alloced_cpus_in_node to start over.
		 */
		if (cpumask_equal(&primary->alloced_cpus_in_node,
				  cpumask_of_node(primary->numa_node)))
			cpumask_clear(&primary->alloced_cpus_in_node);
	}

	while (true) {
		cur_cpu = cpumask_next(cur_cpu, available_mask);
		if (cur_cpu >= nr_cpu_ids) {
			cur_cpu = -1;
			cpumask_copy(available_mask,
				     cpumask_of_node(primary->numa_node));
			continue;
		}

		if (primary->affinity_policy == HV_LOCALIZED) {
			/*
			 * NOTE: in the case of sub-channel, we clear the
			 * sub-channel related bit(s) in
			 * primary->alloced_cpus_in_node in
			 * hv_process_channel_removal(), so when we
			 * reload drivers like hv_netvsc in SMP guest, here
			 * we're able to re-allocate
			 * bit from primary->alloced_cpus_in_node.
			 */
			if (!cpumask_test_cpu(cur_cpu,
					      &primary->alloced_cpus_in_node)) {
				cpumask_set_cpu(cur_cpu,
						&primary->alloced_cpus_in_node);
				cpumask_set_cpu(cur_cpu, alloced_mask);
				break;
			}
		} else {
			cpumask_set_cpu(cur_cpu, alloced_mask);
			break;
		}
	}

	channel->target_cpu = cur_cpu;
	channel->target_vp = hv_cpu_number_to_vp_number(cur_cpu);

	spin_unlock(&bind_channel_to_cpu_lock);

	free_cpumask_var(available_mask);
}

static void vmbus_wait_for_unload(void)
{
	int cpu;
	void *page_addr;
	struct hv_message *msg;
	struct vmbus_channel_message_header *hdr;
	u32 message_type, i;

	/*
	 * CHANNELMSG_UNLOAD_RESPONSE is always delivered to the CPU which was
	 * used for initial contact or to CPU0 depending on host version. When
	 * we're crashing on a different CPU let's hope that IRQ handler on
	 * the cpu which receives CHANNELMSG_UNLOAD_RESPONSE is still
	 * functional and vmbus_unload_response() will complete
	 * vmbus_connection.unload_event. If not, the last thing we can do is
	 * read message pages for all CPUs directly.
	 *
	 * Wait no more than 10 seconds so that the panic path can't get
	 * hung forever in case the response message isn't seen.
	 */
	for (i = 0; i < 1000; i++) {
		if (completion_done(&vmbus_connection.unload_event))
			break;

		for_each_online_cpu(cpu) {
			struct hv_per_cpu_context *hv_cpu
				= per_cpu_ptr(hv_context.cpu_context, cpu);

			page_addr = hv_cpu->synic_message_page;
			msg = (struct hv_message *)page_addr
				+ VMBUS_MESSAGE_SINT;

			message_type = READ_ONCE(msg->header.message_type);
			if (message_type == HVMSG_NONE)
				continue;

			hdr = (struct vmbus_channel_message_header *)
				msg->u.payload;

			if (hdr->msgtype == CHANNELMSG_UNLOAD_RESPONSE)
				complete(&vmbus_connection.unload_event);

			vmbus_signal_eom(msg, message_type);
		}

		mdelay(10);
	}

	/*
	 * We're crashing and already got the UNLOAD_RESPONSE, cleanup all
	 * maybe-pending messages on all CPUs to be able to receive new
	 * messages after we reconnect.
	 */
	for_each_online_cpu(cpu) {
		struct hv_per_cpu_context *hv_cpu
			= per_cpu_ptr(hv_context.cpu_context, cpu);

		page_addr = hv_cpu->synic_message_page;
		msg = (struct hv_message *)page_addr + VMBUS_MESSAGE_SINT;
		msg->header.message_type = HVMSG_NONE;
	}
}

/*
 * vmbus_unload_response - Handler for the unload response.
 */
static void vmbus_unload_response(struct vmbus_channel_message_header *hdr)
{
	/*
	 * This is a global event; just wakeup the waiting thread.
	 * Once we successfully unload, we can cleanup the monitor state.
	 */
	complete(&vmbus_connection.unload_event);
}

void vmbus_initiate_unload(bool crash)
{
	struct vmbus_channel_message_header hdr;

	if (xchg(&vmbus_connection.conn_state, DISCONNECTED) == DISCONNECTED)
		return;

	/* Pre-Win2012R2 hosts don't support reconnect */
	if (vmbus_proto_version < VERSION_WIN8_1)
		return;

	init_completion(&vmbus_connection.unload_event);
	memset(&hdr, 0, sizeof(struct vmbus_channel_message_header));
	hdr.msgtype = CHANNELMSG_UNLOAD;
	vmbus_post_msg(&hdr, sizeof(struct vmbus_channel_message_header),
		       !crash);

	/*
	 * vmbus_initiate_unload() is also called on crash and the crash can be
	 * happening in an interrupt context, where scheduling is impossible.
	 */
	if (!crash)
		wait_for_completion(&vmbus_connection.unload_event);
	else
		vmbus_wait_for_unload();
}

static void check_ready_for_resume_event(void)
{
	/*
	 * If all the old primary channels have been fixed up, then it's safe
	 * to resume.
	 */
	if (atomic_dec_and_test(&vmbus_connection.nr_chan_fixup_on_resume))
		complete(&vmbus_connection.ready_for_resume_event);
}

static void vmbus_setup_channel_state(struct vmbus_channel *channel,
				      struct vmbus_channel_offer_channel *offer)
{
	/*
	 * Setup state for signalling the host.
	 */
	channel->sig_event = VMBUS_EVENT_CONNECTION_ID;

	if (vmbus_proto_version != VERSION_WS2008) {
		channel->is_dedicated_interrupt =
				(offer->is_dedicated_interrupt != 0);
		channel->sig_event = offer->connection_id;
	}

	memcpy(&channel->offermsg, offer,
	       sizeof(struct vmbus_channel_offer_channel));
	channel->monitor_grp = (u8)offer->monitorid / 32;
	channel->monitor_bit = (u8)offer->monitorid % 32;
}

/*
 * find_primary_channel_by_offer - Get the channel object given the new offer.
 * This is only used in the resume path of hibernation.
 */
static struct vmbus_channel *
find_primary_channel_by_offer(const struct vmbus_channel_offer_channel *offer)
{
	struct vmbus_channel *channel = NULL, *iter;
	const guid_t *inst1, *inst2;

	/* Ignore sub-channel offers. */
	if (offer->offer.sub_channel_index != 0)
		return NULL;

	mutex_lock(&vmbus_connection.channel_mutex);

	list_for_each_entry(iter, &vmbus_connection.chn_list, listentry) {
		inst1 = &iter->offermsg.offer.if_instance;
		inst2 = &offer->offer.if_instance;

		if (guid_equal(inst1, inst2)) {
			channel = iter;
			break;
		}
	}

	mutex_unlock(&vmbus_connection.channel_mutex);

	return channel;
}

/*
 * vmbus_onoffer - Handler for channel offers from vmbus in parent partition.
 *
 */
static void vmbus_onoffer(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_offer_channel *offer;
	struct vmbus_channel *oldchannel, *newchannel;
	size_t offer_sz;

	offer = (struct vmbus_channel_offer_channel *)hdr;

	trace_vmbus_onoffer(offer);

	oldchannel = find_primary_channel_by_offer(offer);

	if (oldchannel != NULL) {
		atomic_dec(&vmbus_connection.offer_in_progress);

		/*
		 * We're resuming from hibernation: all the sub-channel and
		 * hv_sock channels we had before the hibernation should have
		 * been cleaned up, and now we must be seeing a re-offered
		 * primary channel that we had before the hibernation.
		 */

		WARN_ON(oldchannel->offermsg.child_relid != INVALID_RELID);
		/* Fix up the relid. */
		oldchannel->offermsg.child_relid = offer->child_relid;

		offer_sz = sizeof(*offer);
		if (memcmp(offer, &oldchannel->offermsg, offer_sz) == 0) {
			check_ready_for_resume_event();
			return;
		}

		/*
		 * This is not an error, since the host can also change the
		 * other field(s) of the offer, e.g. on WS RS5 (Build 17763),
		 * the offer->connection_id of the Mellanox VF vmbus device
		 * can change when the host reoffers the device upon resume.
		 */
		pr_debug("vmbus offer changed: relid=%d\n",
			 offer->child_relid);

		print_hex_dump_debug("Old vmbus offer: ", DUMP_PREFIX_OFFSET,
				     16, 4, &oldchannel->offermsg, offer_sz,
				     false);
		print_hex_dump_debug("New vmbus offer: ", DUMP_PREFIX_OFFSET,
				     16, 4, offer, offer_sz, false);

		/* Fix up the old channel. */
		vmbus_setup_channel_state(oldchannel, offer);

		check_ready_for_resume_event();

		return;
	}

	/* Allocate the channel object and save this offer. */
	newchannel = alloc_channel();
	if (!newchannel) {
		vmbus_release_relid(offer->child_relid);
		atomic_dec(&vmbus_connection.offer_in_progress);
		pr_err("Unable to allocate channel object\n");
		return;
	}

	vmbus_setup_channel_state(newchannel, offer);

	vmbus_process_offer(newchannel);
}

static void check_ready_for_suspend_event(void)
{
	/*
	 * If all the sub-channels or hv_sock channels have been cleaned up,
	 * then it's safe to suspend.
	 */
	if (atomic_dec_and_test(&vmbus_connection.nr_chan_close_on_suspend))
		complete(&vmbus_connection.ready_for_suspend_event);
}

/*
 * vmbus_onoffer_rescind - Rescind offer handler.
 *
 * We queue a work item to process this offer synchronously
 */
static void vmbus_onoffer_rescind(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_rescind_offer *rescind;
	struct vmbus_channel *channel;
	struct device *dev;
	bool clean_up_chan_for_suspend;

	rescind = (struct vmbus_channel_rescind_offer *)hdr;

	trace_vmbus_onoffer_rescind(rescind);

	/*
	 * The offer msg and the corresponding rescind msg
	 * from the host are guranteed to be ordered -
	 * offer comes in first and then the rescind.
	 * Since we process these events in work elements,
	 * and with preemption, we may end up processing
	 * the events out of order. Given that we handle these
	 * work elements on the same CPU, this is possible only
	 * in the case of preemption. In any case wait here
	 * until the offer processing has moved beyond the
	 * point where the channel is discoverable.
	 */

	while (atomic_read(&vmbus_connection.offer_in_progress) != 0) {
		/*
		 * We wait here until any channel offer is currently
		 * being processed.
		 */
		msleep(1);
	}

	mutex_lock(&vmbus_connection.channel_mutex);
	channel = relid2channel(rescind->child_relid);
	mutex_unlock(&vmbus_connection.channel_mutex);

	if (channel == NULL) {
		/*
		 * We failed in processing the offer message;
		 * we would have cleaned up the relid in that
		 * failure path.
		 */
		return;
	}

	clean_up_chan_for_suspend = is_hvsock_channel(channel) ||
				    is_sub_channel(channel);
	/*
	 * Before setting channel->rescind in vmbus_rescind_cleanup(), we
	 * should make sure the channel callback is not running any more.
	 */
	vmbus_reset_channel_cb(channel);

	/*
	 * Now wait for offer handling to complete.
	 */
	vmbus_rescind_cleanup(channel);
	while (READ_ONCE(channel->probe_done) == false) {
		/*
		 * We wait here until any channel offer is currently
		 * being processed.
		 */
		msleep(1);
	}

	/*
	 * At this point, the rescind handling can proceed safely.
	 */

	if (channel->device_obj) {
		if (channel->chn_rescind_callback) {
			channel->chn_rescind_callback(channel);

			if (clean_up_chan_for_suspend)
				check_ready_for_suspend_event();

			return;
		}
		/*
		 * We will have to unregister this device from the
		 * driver core.
		 */
		dev = get_device(&channel->device_obj->device);
		if (dev) {
			vmbus_device_unregister(channel->device_obj);
			put_device(dev);
		}
	}
	if (channel->primary_channel != NULL) {
		/*
		 * Sub-channel is being rescinded. Following is the channel
		 * close sequence when initiated from the driveri (refer to
		 * vmbus_close() for details):
		 * 1. Close all sub-channels first
		 * 2. Then close the primary channel.
		 */
		mutex_lock(&vmbus_connection.channel_mutex);
		if (channel->state == CHANNEL_OPEN_STATE) {
			/*
			 * The channel is currently not open;
			 * it is safe for us to cleanup the channel.
			 */
			hv_process_channel_removal(channel);
		} else {
			complete(&channel->rescind_event);
		}
		mutex_unlock(&vmbus_connection.channel_mutex);
	}

	/* The "channel" may have been freed. Do not access it any longer. */

	if (clean_up_chan_for_suspend)
		check_ready_for_suspend_event();
}

void vmbus_hvsock_device_unregister(struct vmbus_channel *channel)
{
	BUG_ON(!is_hvsock_channel(channel));

	/* We always get a rescind msg when a connection is closed. */
	while (!READ_ONCE(channel->probe_done) || !READ_ONCE(channel->rescind))
		msleep(1);

	vmbus_device_unregister(channel->device_obj);
}
EXPORT_SYMBOL_GPL(vmbus_hvsock_device_unregister);


/*
 * vmbus_onoffers_delivered -
 * This is invoked when all offers have been delivered.
 *
 * Nothing to do here.
 */
static void vmbus_onoffers_delivered(
			struct vmbus_channel_message_header *hdr)
{
}

/*
 * vmbus_onopen_result - Open result handler.
 *
 * This is invoked when we received a response to our channel open request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_onopen_result(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_open_result *result;
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_open_channel *openmsg;
	unsigned long flags;

	result = (struct vmbus_channel_open_result *)hdr;

	trace_vmbus_onopen_result(result);

	/*
	 * Find the open msg, copy the result and signal/unblock the wait event
	 */
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype == CHANNELMSG_OPENCHANNEL) {
			openmsg =
			(struct vmbus_channel_open_channel *)msginfo->msg;
			if (openmsg->child_relid == result->child_relid &&
			    openmsg->openid == result->openid) {
				memcpy(&msginfo->response.open_result,
				       result,
				       sizeof(
					struct vmbus_channel_open_result));
				complete(&msginfo->waitevent);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/*
 * vmbus_ongpadl_created - GPADL created handler.
 *
 * This is invoked when we received a response to our gpadl create request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_ongpadl_created(struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_gpadl_created *gpadlcreated;
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_gpadl_header *gpadlheader;
	unsigned long flags;

	gpadlcreated = (struct vmbus_channel_gpadl_created *)hdr;

	trace_vmbus_ongpadl_created(gpadlcreated);

	/*
	 * Find the establish msg, copy the result and signal/unblock the wait
	 * event
	 */
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype == CHANNELMSG_GPADL_HEADER) {
			gpadlheader =
			(struct vmbus_channel_gpadl_header *)requestheader;

			if ((gpadlcreated->child_relid ==
			     gpadlheader->child_relid) &&
			    (gpadlcreated->gpadl == gpadlheader->gpadl)) {
				memcpy(&msginfo->response.gpadl_created,
				       gpadlcreated,
				       sizeof(
					struct vmbus_channel_gpadl_created));
				complete(&msginfo->waitevent);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/*
 * vmbus_ongpadl_torndown - GPADL torndown handler.
 *
 * This is invoked when we received a response to our gpadl teardown request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_ongpadl_torndown(
			struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_gpadl_torndown *gpadl_torndown;
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_gpadl_teardown *gpadl_teardown;
	unsigned long flags;

	gpadl_torndown = (struct vmbus_channel_gpadl_torndown *)hdr;

	trace_vmbus_ongpadl_torndown(gpadl_torndown);

	/*
	 * Find the open msg, copy the result and signal/unblock the wait event
	 */
	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype == CHANNELMSG_GPADL_TEARDOWN) {
			gpadl_teardown =
			(struct vmbus_channel_gpadl_teardown *)requestheader;

			if (gpadl_torndown->gpadl == gpadl_teardown->gpadl) {
				memcpy(&msginfo->response.gpadl_torndown,
				       gpadl_torndown,
				       sizeof(
					struct vmbus_channel_gpadl_torndown));
				complete(&msginfo->waitevent);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/*
 * vmbus_onversion_response - Version response handler
 *
 * This is invoked when we received a response to our initiate contact request.
 * Find the matching request, copy the response and signal the requesting
 * thread.
 */
static void vmbus_onversion_response(
		struct vmbus_channel_message_header *hdr)
{
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_message_header *requestheader;
	struct vmbus_channel_version_response *version_response;
	unsigned long flags;

	version_response = (struct vmbus_channel_version_response *)hdr;

	trace_vmbus_onversion_response(version_response);

	spin_lock_irqsave(&vmbus_connection.channelmsg_lock, flags);

	list_for_each_entry(msginfo, &vmbus_connection.chn_msg_list,
				msglistentry) {
		requestheader =
			(struct vmbus_channel_message_header *)msginfo->msg;

		if (requestheader->msgtype ==
		    CHANNELMSG_INITIATE_CONTACT) {
			memcpy(&msginfo->response.version_response,
			      version_response,
			      sizeof(struct vmbus_channel_version_response));
			complete(&msginfo->waitevent);
		}
	}
	spin_unlock_irqrestore(&vmbus_connection.channelmsg_lock, flags);
}

/* Channel message dispatch table */
const struct vmbus_channel_message_table_entry
channel_message_table[CHANNELMSG_COUNT] = {
	{ CHANNELMSG_INVALID,			0, NULL },
	{ CHANNELMSG_OFFERCHANNEL,		0, vmbus_onoffer },
	{ CHANNELMSG_RESCIND_CHANNELOFFER,	0, vmbus_onoffer_rescind },
	{ CHANNELMSG_REQUESTOFFERS,		0, NULL },
	{ CHANNELMSG_ALLOFFERS_DELIVERED,	1, vmbus_onoffers_delivered },
	{ CHANNELMSG_OPENCHANNEL,		0, NULL },
	{ CHANNELMSG_OPENCHANNEL_RESULT,	1, vmbus_onopen_result },
	{ CHANNELMSG_CLOSECHANNEL,		0, NULL },
	{ CHANNELMSG_GPADL_HEADER,		0, NULL },
	{ CHANNELMSG_GPADL_BODY,		0, NULL },
	{ CHANNELMSG_GPADL_CREATED,		1, vmbus_ongpadl_created },
	{ CHANNELMSG_GPADL_TEARDOWN,		0, NULL },
	{ CHANNELMSG_GPADL_TORNDOWN,		1, vmbus_ongpadl_torndown },
	{ CHANNELMSG_RELID_RELEASED,		0, NULL },
	{ CHANNELMSG_INITIATE_CONTACT,		0, NULL },
	{ CHANNELMSG_VERSION_RESPONSE,		1, vmbus_onversion_response },
	{ CHANNELMSG_UNLOAD,			0, NULL },
	{ CHANNELMSG_UNLOAD_RESPONSE,		1, vmbus_unload_response },
	{ CHANNELMSG_18,			0, NULL },
	{ CHANNELMSG_19,			0, NULL },
	{ CHANNELMSG_20,			0, NULL },
	{ CHANNELMSG_TL_CONNECT_REQUEST,	0, NULL },
	{ CHANNELMSG_22,			0, NULL },
	{ CHANNELMSG_TL_CONNECT_RESULT,		0, NULL },
};

/*
 * vmbus_onmessage - Handler for channel protocol messages.
 *
 * This is invoked in the vmbus worker thread context.
 */
void vmbus_onmessage(void *context)
{
	struct hv_message *msg = context;
	struct vmbus_channel_message_header *hdr;

	hdr = (struct vmbus_channel_message_header *)msg->u.payload;

	trace_vmbus_on_message(hdr);

	/*
	 * vmbus_on_msg_dpc() makes sure the hdr->msgtype here can not go
	 * out of bound and the message_handler pointer can not be NULL.
	 */
	channel_message_table[hdr->msgtype].message_handler(hdr);
}

/*
 * vmbus_request_offers - Send a request to get all our pending offers.
 */
int vmbus_request_offers(void)
{
	struct vmbus_channel_message_header *msg;
	struct vmbus_channel_msginfo *msginfo;
	int ret;

	msginfo = kmalloc(sizeof(*msginfo) +
			  sizeof(struct vmbus_channel_message_header),
			  GFP_KERNEL);
	if (!msginfo)
		return -ENOMEM;

	msg = (struct vmbus_channel_message_header *)msginfo->msg;

	msg->msgtype = CHANNELMSG_REQUESTOFFERS;

	ret = vmbus_post_msg(msg, sizeof(struct vmbus_channel_message_header),
			     true);

	trace_vmbus_request_offers(ret);

	if (ret != 0) {
		pr_err("Unable to request offers - %d\n", ret);

		goto cleanup;
	}

cleanup:
	kfree(msginfo);

	return ret;
}

static void invoke_sc_cb(struct vmbus_channel *primary_channel)
{
	struct list_head *cur, *tmp;
	struct vmbus_channel *cur_channel;

	if (primary_channel->sc_creation_callback == NULL)
		return;

	list_for_each_safe(cur, tmp, &primary_channel->sc_list) {
		cur_channel = list_entry(cur, struct vmbus_channel, sc_list);

		primary_channel->sc_creation_callback(cur_channel);
	}
}

void vmbus_set_sc_create_callback(struct vmbus_channel *primary_channel,
				void (*sc_cr_cb)(struct vmbus_channel *new_sc))
{
	primary_channel->sc_creation_callback = sc_cr_cb;
}
EXPORT_SYMBOL_GPL(vmbus_set_sc_create_callback);

bool vmbus_are_subchannels_present(struct vmbus_channel *primary)
{
	bool ret;

	ret = !list_empty(&primary->sc_list);

	if (ret) {
		/*
		 * Invoke the callback on sub-channel creation.
		 * This will present a uniform interface to the
		 * clients.
		 */
		invoke_sc_cb(primary);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vmbus_are_subchannels_present);

void vmbus_set_chn_rescind_callback(struct vmbus_channel *channel,
		void (*chn_rescind_cb)(struct vmbus_channel *))
{
	channel->chn_rescind_callback = chn_rescind_cb;
}
EXPORT_SYMBOL_GPL(vmbus_set_chn_rescind_callback);
