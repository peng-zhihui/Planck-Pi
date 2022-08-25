// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright 2016 VMware, Inc., Palo Alto, CA., USA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/frame.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <asm/hypervisor.h>

#include "vmwgfx_drv.h"
#include "vmwgfx_msg.h"

#define MESSAGE_STATUS_SUCCESS  0x0001
#define MESSAGE_STATUS_DORECV   0x0002
#define MESSAGE_STATUS_CPT      0x0010
#define MESSAGE_STATUS_HB       0x0080

#define RPCI_PROTOCOL_NUM       0x49435052
#define GUESTMSG_FLAG_COOKIE    0x80000000

#define RETRIES                 3

#define VMW_HYPERVISOR_MAGIC    0x564D5868

#define VMW_PORT_CMD_MSG        30
#define VMW_PORT_CMD_HB_MSG     0
#define VMW_PORT_CMD_OPEN_CHANNEL  (MSG_TYPE_OPEN << 16 | VMW_PORT_CMD_MSG)
#define VMW_PORT_CMD_CLOSE_CHANNEL (MSG_TYPE_CLOSE << 16 | VMW_PORT_CMD_MSG)
#define VMW_PORT_CMD_SENDSIZE   (MSG_TYPE_SENDSIZE << 16 | VMW_PORT_CMD_MSG)
#define VMW_PORT_CMD_RECVSIZE   (MSG_TYPE_RECVSIZE << 16 | VMW_PORT_CMD_MSG)
#define VMW_PORT_CMD_RECVSTATUS (MSG_TYPE_RECVSTATUS << 16 | VMW_PORT_CMD_MSG)

#define HIGH_WORD(X) ((X & 0xFFFF0000) >> 16)

static u32 vmw_msg_enabled = 1;

enum rpc_msg_type {
	MSG_TYPE_OPEN,
	MSG_TYPE_SENDSIZE,
	MSG_TYPE_SENDPAYLOAD,
	MSG_TYPE_RECVSIZE,
	MSG_TYPE_RECVPAYLOAD,
	MSG_TYPE_RECVSTATUS,
	MSG_TYPE_CLOSE,
};

struct rpc_channel {
	u16 channel_id;
	u32 cookie_high;
	u32 cookie_low;
};



/**
 * vmw_open_channel
 *
 * @channel: RPC channel
 * @protocol:
 *
 * Returns: 0 on success
 */
static int vmw_open_channel(struct rpc_channel *channel, unsigned int protocol)
{
	unsigned long eax, ebx, ecx, edx, si = 0, di = 0;

	VMW_PORT(VMW_PORT_CMD_OPEN_CHANNEL,
		(protocol | GUESTMSG_FLAG_COOKIE), si, di,
		0,
		VMW_HYPERVISOR_MAGIC,
		eax, ebx, ecx, edx, si, di);

	if ((HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS) == 0)
		return -EINVAL;

	channel->channel_id  = HIGH_WORD(edx);
	channel->cookie_high = si;
	channel->cookie_low  = di;

	return 0;
}



/**
 * vmw_close_channel
 *
 * @channel: RPC channel
 *
 * Returns: 0 on success
 */
static int vmw_close_channel(struct rpc_channel *channel)
{
	unsigned long eax, ebx, ecx, edx, si, di;

	/* Set up additional parameters */
	si  = channel->cookie_high;
	di  = channel->cookie_low;

	VMW_PORT(VMW_PORT_CMD_CLOSE_CHANNEL,
		0, si, di,
		channel->channel_id << 16,
		VMW_HYPERVISOR_MAGIC,
		eax, ebx, ecx, edx, si, di);

	if ((HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS) == 0)
		return -EINVAL;

	return 0;
}

/**
 * vmw_port_hb_out - Send the message payload either through the
 * high-bandwidth port if available, or through the backdoor otherwise.
 * @channel: The rpc channel.
 * @msg: NULL-terminated message.
 * @hb: Whether the high-bandwidth port is available.
 *
 * Return: The port status.
 */
static unsigned long vmw_port_hb_out(struct rpc_channel *channel,
				     const char *msg, bool hb)
{
	unsigned long si, di, eax, ebx, ecx, edx;
	unsigned long msg_len = strlen(msg);

	if (hb) {
		unsigned long bp = channel->cookie_high;

		si = (uintptr_t) msg;
		di = channel->cookie_low;

		VMW_PORT_HB_OUT(
			(MESSAGE_STATUS_SUCCESS << 16) | VMW_PORT_CMD_HB_MSG,
			msg_len, si, di,
			VMWARE_HYPERVISOR_HB | (channel->channel_id << 16) |
			VMWARE_HYPERVISOR_OUT,
			VMW_HYPERVISOR_MAGIC, bp,
			eax, ebx, ecx, edx, si, di);

		return ebx;
	}

	/* HB port not available. Send the message 4 bytes at a time. */
	ecx = MESSAGE_STATUS_SUCCESS << 16;
	while (msg_len && (HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS)) {
		unsigned int bytes = min_t(size_t, msg_len, 4);
		unsigned long word = 0;

		memcpy(&word, msg, bytes);
		msg_len -= bytes;
		msg += bytes;
		si = channel->cookie_high;
		di = channel->cookie_low;

		VMW_PORT(VMW_PORT_CMD_MSG | (MSG_TYPE_SENDPAYLOAD << 16),
			 word, si, di,
			 channel->channel_id << 16,
			 VMW_HYPERVISOR_MAGIC,
			 eax, ebx, ecx, edx, si, di);
	}

	return ecx;
}

/**
 * vmw_port_hb_in - Receive the message payload either through the
 * high-bandwidth port if available, or through the backdoor otherwise.
 * @channel: The rpc channel.
 * @reply: Pointer to buffer holding reply.
 * @reply_len: Length of the reply.
 * @hb: Whether the high-bandwidth port is available.
 *
 * Return: The port status.
 */
static unsigned long vmw_port_hb_in(struct rpc_channel *channel, char *reply,
				    unsigned long reply_len, bool hb)
{
	unsigned long si, di, eax, ebx, ecx, edx;

	if (hb) {
		unsigned long bp = channel->cookie_low;

		si = channel->cookie_high;
		di = (uintptr_t) reply;

		VMW_PORT_HB_IN(
			(MESSAGE_STATUS_SUCCESS << 16) | VMW_PORT_CMD_HB_MSG,
			reply_len, si, di,
			VMWARE_HYPERVISOR_HB | (channel->channel_id << 16),
			VMW_HYPERVISOR_MAGIC, bp,
			eax, ebx, ecx, edx, si, di);

		return ebx;
	}

	/* HB port not available. Retrieve the message 4 bytes at a time. */
	ecx = MESSAGE_STATUS_SUCCESS << 16;
	while (reply_len) {
		unsigned int bytes = min_t(unsigned long, reply_len, 4);

		si = channel->cookie_high;
		di = channel->cookie_low;

		VMW_PORT(VMW_PORT_CMD_MSG | (MSG_TYPE_RECVPAYLOAD << 16),
			 MESSAGE_STATUS_SUCCESS, si, di,
			 channel->channel_id << 16,
			 VMW_HYPERVISOR_MAGIC,
			 eax, ebx, ecx, edx, si, di);

		if ((HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS) == 0)
			break;

		memcpy(reply, &ebx, bytes);
		reply_len -= bytes;
		reply += bytes;
	}

	return ecx;
}


/**
 * vmw_send_msg: Sends a message to the host
 *
 * @channel: RPC channel
 * @logmsg: NULL terminated string
 *
 * Returns: 0 on success
 */
static int vmw_send_msg(struct rpc_channel *channel, const char *msg)
{
	unsigned long eax, ebx, ecx, edx, si, di;
	size_t msg_len = strlen(msg);
	int retries = 0;

	while (retries < RETRIES) {
		retries++;

		/* Set up additional parameters */
		si  = channel->cookie_high;
		di  = channel->cookie_low;

		VMW_PORT(VMW_PORT_CMD_SENDSIZE,
			msg_len, si, di,
			channel->channel_id << 16,
			VMW_HYPERVISOR_MAGIC,
			eax, ebx, ecx, edx, si, di);

		if ((HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS) == 0) {
			/* Expected success. Give up. */
			return -EINVAL;
		}

		/* Send msg */
		ebx = vmw_port_hb_out(channel, msg,
				      !!(HIGH_WORD(ecx) & MESSAGE_STATUS_HB));

		if ((HIGH_WORD(ebx) & MESSAGE_STATUS_SUCCESS) != 0) {
			return 0;
		} else if ((HIGH_WORD(ebx) & MESSAGE_STATUS_CPT) != 0) {
			/* A checkpoint occurred. Retry. */
			continue;
		} else {
			break;
		}
	}

	return -EINVAL;
}
STACK_FRAME_NON_STANDARD(vmw_send_msg);


/**
 * vmw_recv_msg: Receives a message from the host
 *
 * Note:  It is the caller's responsibility to call kfree() on msg.
 *
 * @channel:  channel opened by vmw_open_channel
 * @msg:  [OUT] message received from the host
 * @msg_len: message length
 */
static int vmw_recv_msg(struct rpc_channel *channel, void **msg,
			size_t *msg_len)
{
	unsigned long eax, ebx, ecx, edx, si, di;
	char *reply;
	size_t reply_len;
	int retries = 0;


	*msg_len = 0;
	*msg = NULL;

	while (retries < RETRIES) {
		retries++;

		/* Set up additional parameters */
		si  = channel->cookie_high;
		di  = channel->cookie_low;

		VMW_PORT(VMW_PORT_CMD_RECVSIZE,
			0, si, di,
			channel->channel_id << 16,
			VMW_HYPERVISOR_MAGIC,
			eax, ebx, ecx, edx, si, di);

		if ((HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS) == 0) {
			DRM_ERROR("Failed to get reply size for host message.\n");
			return -EINVAL;
		}

		/* No reply available.  This is okay. */
		if ((HIGH_WORD(ecx) & MESSAGE_STATUS_DORECV) == 0)
			return 0;

		reply_len = ebx;
		reply     = kzalloc(reply_len + 1, GFP_KERNEL);
		if (!reply) {
			DRM_ERROR("Cannot allocate memory for host message reply.\n");
			return -ENOMEM;
		}


		/* Receive buffer */
		ebx = vmw_port_hb_in(channel, reply, reply_len,
				     !!(HIGH_WORD(ecx) & MESSAGE_STATUS_HB));
		if ((HIGH_WORD(ebx) & MESSAGE_STATUS_SUCCESS) == 0) {
			kfree(reply);
			reply = NULL;
			if ((HIGH_WORD(ebx) & MESSAGE_STATUS_CPT) != 0) {
				/* A checkpoint occurred. Retry. */
				continue;
			}

			return -EINVAL;
		}

		reply[reply_len] = '\0';


		/* Ack buffer */
		si  = channel->cookie_high;
		di  = channel->cookie_low;

		VMW_PORT(VMW_PORT_CMD_RECVSTATUS,
			MESSAGE_STATUS_SUCCESS, si, di,
			channel->channel_id << 16,
			VMW_HYPERVISOR_MAGIC,
			eax, ebx, ecx, edx, si, di);

		if ((HIGH_WORD(ecx) & MESSAGE_STATUS_SUCCESS) == 0) {
			kfree(reply);
			reply = NULL;
			if ((HIGH_WORD(ecx) & MESSAGE_STATUS_CPT) != 0) {
				/* A checkpoint occurred. Retry. */
				continue;
			}

			return -EINVAL;
		}

		break;
	}

	if (!reply)
		return -EINVAL;

	*msg_len = reply_len;
	*msg     = reply;

	return 0;
}
STACK_FRAME_NON_STANDARD(vmw_recv_msg);


/**
 * vmw_host_get_guestinfo: Gets a GuestInfo parameter
 *
 * Gets the value of a  GuestInfo.* parameter.  The value returned will be in
 * a string, and it is up to the caller to post-process.
 *
 * @guest_info_param:  Parameter to get, e.g. GuestInfo.svga.gl3
 * @buffer: if NULL, *reply_len will contain reply size.
 * @length: size of the reply_buf.  Set to size of reply upon return
 *
 * Returns: 0 on success
 */
int vmw_host_get_guestinfo(const char *guest_info_param,
			   char *buffer, size_t *length)
{
	struct rpc_channel channel;
	char *msg, *reply = NULL;
	size_t reply_len = 0;

	if (!vmw_msg_enabled)
		return -ENODEV;

	if (!guest_info_param || !length)
		return -EINVAL;

	msg = kasprintf(GFP_KERNEL, "info-get %s", guest_info_param);
	if (!msg) {
		DRM_ERROR("Cannot allocate memory to get guest info \"%s\".",
			  guest_info_param);
		return -ENOMEM;
	}

	if (vmw_open_channel(&channel, RPCI_PROTOCOL_NUM))
		goto out_open;

	if (vmw_send_msg(&channel, msg) ||
	    vmw_recv_msg(&channel, (void *) &reply, &reply_len))
		goto out_msg;

	vmw_close_channel(&channel);
	if (buffer && reply && reply_len > 0) {
		/* Remove reply code, which are the first 2 characters of
		 * the reply
		 */
		reply_len = max(reply_len - 2, (size_t) 0);
		reply_len = min(reply_len, *length);

		if (reply_len > 0)
			memcpy(buffer, reply + 2, reply_len);
	}

	*length = reply_len;

	kfree(reply);
	kfree(msg);

	return 0;

out_msg:
	vmw_close_channel(&channel);
	kfree(reply);
out_open:
	*length = 0;
	kfree(msg);
	DRM_ERROR("Failed to get guest info \"%s\".", guest_info_param);

	return -EINVAL;
}



/**
 * vmw_host_log: Sends a log message to the host
 *
 * @log: NULL terminated string
 *
 * Returns: 0 on success
 */
int vmw_host_log(const char *log)
{
	struct rpc_channel channel;
	char *msg;
	int ret = 0;


	if (!vmw_msg_enabled)
		return -ENODEV;

	if (!log)
		return ret;

	msg = kasprintf(GFP_KERNEL, "log %s", log);
	if (!msg) {
		DRM_ERROR("Cannot allocate memory for host log message.\n");
		return -ENOMEM;
	}

	if (vmw_open_channel(&channel, RPCI_PROTOCOL_NUM))
		goto out_open;

	if (vmw_send_msg(&channel, msg))
		goto out_msg;

	vmw_close_channel(&channel);
	kfree(msg);

	return 0;

out_msg:
	vmw_close_channel(&channel);
out_open:
	kfree(msg);
	DRM_ERROR("Failed to send host log message.\n");

	return -EINVAL;
}
