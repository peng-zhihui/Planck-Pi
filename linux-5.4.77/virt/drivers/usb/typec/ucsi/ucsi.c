// SPDX-License-Identifier: GPL-2.0
/*
 * USB Type-C Connector System Software Interface driver
 *
 * Copyright (C) 2017, Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 */

#include <linux/completion.h>
#include <linux/property.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/usb/typec_dp.h>

#include "ucsi.h"
#include "trace.h"

#define to_ucsi_connector(_cap_) container_of(_cap_, struct ucsi_connector, \
					      typec_cap)

/*
 * UCSI_TIMEOUT_MS - PPM communication timeout
 *
 * Ideally we could use MIN_TIME_TO_RESPOND_WITH_BUSY (which is defined in UCSI
 * specification) here as reference, but unfortunately we can't. It is very
 * difficult to estimate the time it takes for the system to process the command
 * before it is actually passed to the PPM.
 */
#define UCSI_TIMEOUT_MS		5000

/*
 * UCSI_SWAP_TIMEOUT_MS - Timeout for role swap requests
 *
 * 5 seconds is close to the time it takes for CapsCounter to reach 0, so even
 * if the PPM does not generate Connector Change events before that with
 * partners that do not support USB Power Delivery, this should still work.
 */
#define UCSI_SWAP_TIMEOUT_MS	5000

static inline int ucsi_sync(struct ucsi *ucsi)
{
	if (ucsi->ppm && ucsi->ppm->sync)
		return ucsi->ppm->sync(ucsi->ppm);
	return 0;
}

static int ucsi_command(struct ucsi *ucsi, struct ucsi_control *ctrl)
{
	int ret;

	trace_ucsi_command(ctrl);

	set_bit(COMMAND_PENDING, &ucsi->flags);

	ret = ucsi->ppm->cmd(ucsi->ppm, ctrl);
	if (ret)
		goto err_clear_flag;

	if (!wait_for_completion_timeout(&ucsi->complete,
					 msecs_to_jiffies(UCSI_TIMEOUT_MS))) {
		dev_warn(ucsi->dev, "PPM NOT RESPONDING\n");
		ret = -ETIMEDOUT;
	}

err_clear_flag:
	clear_bit(COMMAND_PENDING, &ucsi->flags);

	return ret;
}

static int ucsi_ack(struct ucsi *ucsi, u8 ack)
{
	struct ucsi_control ctrl;
	int ret;

	trace_ucsi_ack(ack);

	set_bit(ACK_PENDING, &ucsi->flags);

	UCSI_CMD_ACK(ctrl, ack);
	ret = ucsi->ppm->cmd(ucsi->ppm, &ctrl);
	if (ret)
		goto out_clear_bit;

	/* Waiting for ACK with ACK CMD, but not with EVENT for now */
	if (ack == UCSI_ACK_EVENT)
		goto out_clear_bit;

	if (!wait_for_completion_timeout(&ucsi->complete,
					 msecs_to_jiffies(UCSI_TIMEOUT_MS)))
		ret = -ETIMEDOUT;

out_clear_bit:
	clear_bit(ACK_PENDING, &ucsi->flags);

	if (ret)
		dev_err(ucsi->dev, "%s: failed\n", __func__);

	return ret;
}

static int ucsi_run_command(struct ucsi *ucsi, struct ucsi_control *ctrl,
			    void *data, size_t size)
{
	struct ucsi_control _ctrl;
	u8 data_length;
	u16 error;
	int ret;

	ret = ucsi_command(ucsi, ctrl);
	if (ret)
		goto err;

	switch (ucsi->status) {
	case UCSI_IDLE:
		ret = ucsi_sync(ucsi);
		if (ret)
			dev_warn(ucsi->dev, "%s: sync failed\n", __func__);

		if (data)
			memcpy(data, ucsi->ppm->data->message_in, size);

		data_length = ucsi->ppm->data->cci.data_length;

		ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
		if (!ret)
			ret = data_length;
		break;
	case UCSI_BUSY:
		/* The caller decides whether to cancel or not */
		ret = -EBUSY;
		break;
	case UCSI_ERROR:
		ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
		if (ret)
			break;

		_ctrl.raw_cmd = 0;
		_ctrl.cmd.cmd = UCSI_GET_ERROR_STATUS;
		ret = ucsi_command(ucsi, &_ctrl);
		if (ret) {
			dev_err(ucsi->dev, "reading error failed!\n");
			break;
		}

		memcpy(&error, ucsi->ppm->data->message_in, sizeof(error));

		/* Something has really gone wrong */
		if (WARN_ON(ucsi->status == UCSI_ERROR)) {
			ret = -ENODEV;
			break;
		}

		ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
		if (ret)
			break;

		switch (error) {
		case UCSI_ERROR_INCOMPATIBLE_PARTNER:
			ret = -EOPNOTSUPP;
			break;
		case UCSI_ERROR_CC_COMMUNICATION_ERR:
			ret = -ECOMM;
			break;
		case UCSI_ERROR_CONTRACT_NEGOTIATION_FAIL:
			ret = -EPROTO;
			break;
		case UCSI_ERROR_DEAD_BATTERY:
			dev_warn(ucsi->dev, "Dead battery condition!\n");
			ret = -EPERM;
			break;
		/* The following mean a bug in this driver */
		case UCSI_ERROR_INVALID_CON_NUM:
		case UCSI_ERROR_UNREGONIZED_CMD:
		case UCSI_ERROR_INVALID_CMD_ARGUMENT:
			dev_warn(ucsi->dev,
				 "%s: possible UCSI driver bug - error 0x%x\n",
				 __func__, error);
			ret = -EINVAL;
			break;
		default:
			dev_warn(ucsi->dev,
				 "%s: error without status\n", __func__);
			ret = -EIO;
			break;
		}
		break;
	}

err:
	trace_ucsi_run_command(ctrl, ret);

	return ret;
}

int ucsi_send_command(struct ucsi *ucsi, struct ucsi_control *ctrl,
		      void *retval, size_t size)
{
	int ret;

	mutex_lock(&ucsi->ppm_lock);
	ret = ucsi_run_command(ucsi, ctrl, retval, size);
	mutex_unlock(&ucsi->ppm_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ucsi_send_command);

int ucsi_resume(struct ucsi *ucsi)
{
	struct ucsi_control ctrl;

	/* Restore UCSI notification enable mask after system resume */
	UCSI_CMD_SET_NTFY_ENABLE(ctrl, UCSI_ENABLE_NTFY_ALL);
	return ucsi_send_command(ucsi, &ctrl, NULL, 0);
}
EXPORT_SYMBOL_GPL(ucsi_resume);
/* -------------------------------------------------------------------------- */

void ucsi_altmode_update_active(struct ucsi_connector *con)
{
	const struct typec_altmode *altmode = NULL;
	struct ucsi_control ctrl;
	int ret;
	u8 cur;
	int i;

	UCSI_CMD_GET_CURRENT_CAM(ctrl, con->num);
	ret = ucsi_run_command(con->ucsi, &ctrl, &cur, sizeof(cur));
	if (ret < 0) {
		if (con->ucsi->ppm->data->version > 0x0100) {
			dev_err(con->ucsi->dev,
				"GET_CURRENT_CAM command failed\n");
			return;
		}
		cur = 0xff;
	}

	if (cur < UCSI_MAX_ALTMODES)
		altmode = typec_altmode_get_partner(con->port_altmode[cur]);

	for (i = 0; con->partner_altmode[i]; i++)
		typec_altmode_update_active(con->partner_altmode[i],
					    con->partner_altmode[i] == altmode);
}

static int ucsi_altmode_next_mode(struct typec_altmode **alt, u16 svid)
{
	u8 mode = 1;
	int i;

	for (i = 0; alt[i]; i++) {
		if (i > MODE_DISCOVERY_MAX)
			return -ERANGE;

		if (alt[i]->svid == svid)
			mode++;
	}

	return mode;
}

static int ucsi_next_altmode(struct typec_altmode **alt)
{
	int i = 0;

	for (i = 0; i < UCSI_MAX_ALTMODES; i++)
		if (!alt[i])
			return i;

	return -ENOENT;
}

static int ucsi_register_altmode(struct ucsi_connector *con,
				 struct typec_altmode_desc *desc,
				 u8 recipient)
{
	struct typec_altmode *alt;
	bool override;
	int ret;
	int i;

	override = !!(con->ucsi->cap.features & UCSI_CAP_ALT_MODE_OVERRIDE);

	switch (recipient) {
	case UCSI_RECIPIENT_CON:
		i = ucsi_next_altmode(con->port_altmode);
		if (i < 0) {
			ret = i;
			goto err;
		}

		ret = ucsi_altmode_next_mode(con->port_altmode, desc->svid);
		if (ret < 0)
			return ret;

		desc->mode = ret;

		switch (desc->svid) {
		case USB_TYPEC_DP_SID:
		case USB_TYPEC_NVIDIA_VLINK_SID:
			alt = ucsi_register_displayport(con, override, i, desc);
			break;
		default:
			alt = typec_port_register_altmode(con->port, desc);
			break;
		}

		if (IS_ERR(alt)) {
			ret = PTR_ERR(alt);
			goto err;
		}

		con->port_altmode[i] = alt;
		break;
	case UCSI_RECIPIENT_SOP:
		i = ucsi_next_altmode(con->partner_altmode);
		if (i < 0) {
			ret = i;
			goto err;
		}

		ret = ucsi_altmode_next_mode(con->partner_altmode, desc->svid);
		if (ret < 0)
			return ret;

		desc->mode = ret;

		alt = typec_partner_register_altmode(con->partner, desc);
		if (IS_ERR(alt)) {
			ret = PTR_ERR(alt);
			goto err;
		}

		con->partner_altmode[i] = alt;
		break;
	default:
		return -EINVAL;
	}

	trace_ucsi_register_altmode(recipient, alt);

	return 0;

err:
	dev_err(con->ucsi->dev, "failed to registers svid 0x%04x mode %d\n",
		desc->svid, desc->mode);

	return ret;
}

static int ucsi_register_altmodes(struct ucsi_connector *con, u8 recipient)
{
	int max_altmodes = UCSI_MAX_ALTMODES;
	struct typec_altmode_desc desc;
	struct ucsi_altmode alt[2];
	struct ucsi_control ctrl;
	int num = 1;
	int ret;
	int len;
	int j;
	int i;

	if (!(con->ucsi->cap.features & UCSI_CAP_ALT_MODE_DETAILS))
		return 0;

	if (recipient == UCSI_RECIPIENT_SOP && con->partner_altmode[0])
		return 0;

	if (recipient == UCSI_RECIPIENT_CON)
		max_altmodes = con->ucsi->cap.num_alt_modes;

	for (i = 0; i < max_altmodes;) {
		memset(alt, 0, sizeof(alt));
		UCSI_CMD_GET_ALTERNATE_MODES(ctrl, recipient, con->num, i, 1);
		len = ucsi_run_command(con->ucsi, &ctrl, alt, sizeof(alt));
		if (len <= 0)
			return len;

		/*
		 * This code is requesting one alt mode at a time, but some PPMs
		 * may still return two. If that happens both alt modes need be
		 * registered and the offset for the next alt mode has to be
		 * incremented.
		 */
		num = len / sizeof(alt[0]);
		i += num;

		for (j = 0; j < num; j++) {
			if (!alt[j].svid)
				return 0;

			memset(&desc, 0, sizeof(desc));
			desc.vdo = alt[j].mid;
			desc.svid = alt[j].svid;
			desc.roles = TYPEC_PORT_DRD;

			ret = ucsi_register_altmode(con, &desc, recipient);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static void ucsi_unregister_altmodes(struct ucsi_connector *con, u8 recipient)
{
	const struct typec_altmode *pdev;
	struct typec_altmode **adev;
	int i = 0;

	switch (recipient) {
	case UCSI_RECIPIENT_CON:
		adev = con->port_altmode;
		break;
	case UCSI_RECIPIENT_SOP:
		adev = con->partner_altmode;
		break;
	default:
		return;
	}

	while (adev[i]) {
		if (recipient == UCSI_RECIPIENT_SOP &&
		    (adev[i]->svid == USB_TYPEC_DP_SID ||
			adev[i]->svid == USB_TYPEC_NVIDIA_VLINK_SID)) {
			pdev = typec_altmode_get_partner(adev[i]);
			ucsi_displayport_remove_partner((void *)pdev);
		}
		typec_unregister_altmode(adev[i]);
		adev[i++] = NULL;
	}
}

static void ucsi_pwr_opmode_change(struct ucsi_connector *con)
{
	switch (con->status.pwr_op_mode) {
	case UCSI_CONSTAT_PWR_OPMODE_PD:
		typec_set_pwr_opmode(con->port, TYPEC_PWR_MODE_PD);
		break;
	case UCSI_CONSTAT_PWR_OPMODE_TYPEC1_5:
		typec_set_pwr_opmode(con->port, TYPEC_PWR_MODE_1_5A);
		break;
	case UCSI_CONSTAT_PWR_OPMODE_TYPEC3_0:
		typec_set_pwr_opmode(con->port, TYPEC_PWR_MODE_3_0A);
		break;
	default:
		typec_set_pwr_opmode(con->port, TYPEC_PWR_MODE_USB);
		break;
	}
}

static int ucsi_register_partner(struct ucsi_connector *con)
{
	struct typec_partner_desc desc;
	struct typec_partner *partner;

	if (con->partner)
		return 0;

	memset(&desc, 0, sizeof(desc));

	switch (con->status.partner_type) {
	case UCSI_CONSTAT_PARTNER_TYPE_DEBUG:
		desc.accessory = TYPEC_ACCESSORY_DEBUG;
		break;
	case UCSI_CONSTAT_PARTNER_TYPE_AUDIO:
		desc.accessory = TYPEC_ACCESSORY_AUDIO;
		break;
	default:
		break;
	}

	desc.usb_pd = con->status.pwr_op_mode == UCSI_CONSTAT_PWR_OPMODE_PD;

	partner = typec_register_partner(con->port, &desc);
	if (IS_ERR(partner)) {
		dev_err(con->ucsi->dev,
			"con%d: failed to register partner (%ld)\n", con->num,
			PTR_ERR(partner));
		return PTR_ERR(partner);
	}

	con->partner = partner;

	return 0;
}

static void ucsi_unregister_partner(struct ucsi_connector *con)
{
	if (!con->partner)
		return;

	ucsi_unregister_altmodes(con, UCSI_RECIPIENT_SOP);
	typec_unregister_partner(con->partner);
	con->partner = NULL;
}

static void ucsi_partner_change(struct ucsi_connector *con)
{
	int ret;

	if (!con->partner)
		return;

	switch (con->status.partner_type) {
	case UCSI_CONSTAT_PARTNER_TYPE_UFP:
		typec_set_data_role(con->port, TYPEC_HOST);
		break;
	case UCSI_CONSTAT_PARTNER_TYPE_DFP:
		typec_set_data_role(con->port, TYPEC_DEVICE);
		break;
	default:
		break;
	}

	/* Complete pending data role swap */
	if (!completion_done(&con->complete))
		complete(&con->complete);

	/* Can't rely on Partner Flags field. Always checking the alt modes. */
	ret = ucsi_register_altmodes(con, UCSI_RECIPIENT_SOP);
	if (ret)
		dev_err(con->ucsi->dev,
			"con%d: failed to register partner alternate modes\n",
			con->num);
	else
		ucsi_altmode_update_active(con);
}

static void ucsi_connector_change(struct work_struct *work)
{
	struct ucsi_connector *con = container_of(work, struct ucsi_connector,
						  work);
	struct ucsi *ucsi = con->ucsi;
	struct ucsi_control ctrl;
	int ret;

	mutex_lock(&con->lock);

	UCSI_CMD_GET_CONNECTOR_STATUS(ctrl, con->num);
	ret = ucsi_send_command(ucsi, &ctrl, &con->status, sizeof(con->status));
	if (ret < 0) {
		dev_err(ucsi->dev, "%s: GET_CONNECTOR_STATUS failed (%d)\n",
			__func__, ret);
		goto out_unlock;
	}

	if (con->status.change & UCSI_CONSTAT_POWER_OPMODE_CHANGE)
		ucsi_pwr_opmode_change(con);

	if (con->status.change & UCSI_CONSTAT_POWER_DIR_CHANGE) {
		typec_set_pwr_role(con->port, con->status.pwr_dir);

		/* Complete pending power role swap */
		if (!completion_done(&con->complete))
			complete(&con->complete);
	}

	if (con->status.change & UCSI_CONSTAT_CONNECT_CHANGE) {
		typec_set_pwr_role(con->port, con->status.pwr_dir);

		switch (con->status.partner_type) {
		case UCSI_CONSTAT_PARTNER_TYPE_UFP:
			typec_set_data_role(con->port, TYPEC_HOST);
			break;
		case UCSI_CONSTAT_PARTNER_TYPE_DFP:
			typec_set_data_role(con->port, TYPEC_DEVICE);
			break;
		default:
			break;
		}

		if (con->status.connected)
			ucsi_register_partner(con);
		else
			ucsi_unregister_partner(con);
	}

	if (con->status.change & UCSI_CONSTAT_CAM_CHANGE) {
		/*
		 * We don't need to know the currently supported alt modes here.
		 * Running GET_CAM_SUPPORTED command just to make sure the PPM
		 * does not get stuck in case it assumes we do so.
		 */
		UCSI_CMD_GET_CAM_SUPPORTED(ctrl, con->num);
		ucsi_run_command(con->ucsi, &ctrl, NULL, 0);
	}

	if (con->status.change & UCSI_CONSTAT_PARTNER_CHANGE)
		ucsi_partner_change(con);

	ret = ucsi_ack(ucsi, UCSI_ACK_EVENT);
	if (ret)
		dev_err(ucsi->dev, "%s: ACK failed (%d)", __func__, ret);

	trace_ucsi_connector_change(con->num, &con->status);

out_unlock:
	clear_bit(EVENT_PENDING, &ucsi->flags);
	mutex_unlock(&con->lock);
}

/**
 * ucsi_notify - PPM notification handler
 * @ucsi: Source UCSI Interface for the notifications
 *
 * Handle notifications from PPM of @ucsi.
 */
void ucsi_notify(struct ucsi *ucsi)
{
	struct ucsi_cci *cci;

	/* There is no requirement to sync here, but no harm either. */
	ucsi_sync(ucsi);

	cci = &ucsi->ppm->data->cci;

	if (cci->error)
		ucsi->status = UCSI_ERROR;
	else if (cci->busy)
		ucsi->status = UCSI_BUSY;
	else
		ucsi->status = UCSI_IDLE;

	if (cci->cmd_complete && test_bit(COMMAND_PENDING, &ucsi->flags)) {
		complete(&ucsi->complete);
	} else if (cci->ack_complete && test_bit(ACK_PENDING, &ucsi->flags)) {
		complete(&ucsi->complete);
	} else if (cci->connector_change) {
		struct ucsi_connector *con;

		con = &ucsi->connector[cci->connector_change - 1];

		if (!test_and_set_bit(EVENT_PENDING, &ucsi->flags))
			schedule_work(&con->work);
	}

	trace_ucsi_notify(ucsi->ppm->data->raw_cci);
}
EXPORT_SYMBOL_GPL(ucsi_notify);

/* -------------------------------------------------------------------------- */

static int ucsi_reset_connector(struct ucsi_connector *con, bool hard)
{
	struct ucsi_control ctrl;

	UCSI_CMD_CONNECTOR_RESET(ctrl, con, hard);

	return ucsi_send_command(con->ucsi, &ctrl, NULL, 0);
}

static int ucsi_reset_ppm(struct ucsi *ucsi)
{
	struct ucsi_control ctrl;
	unsigned long tmo;
	int ret;

	ctrl.raw_cmd = 0;
	ctrl.cmd.cmd = UCSI_PPM_RESET;
	trace_ucsi_command(&ctrl);
	ret = ucsi->ppm->cmd(ucsi->ppm, &ctrl);
	if (ret)
		goto err;

	tmo = jiffies + msecs_to_jiffies(UCSI_TIMEOUT_MS);

	do {
		/* Here sync is critical. */
		ret = ucsi_sync(ucsi);
		if (ret)
			goto err;

		if (ucsi->ppm->data->cci.reset_complete)
			break;

		/* If the PPM is still doing something else, reset it again. */
		if (ucsi->ppm->data->raw_cci) {
			dev_warn_ratelimited(ucsi->dev,
				"Failed to reset PPM! Trying again..\n");

			trace_ucsi_command(&ctrl);
			ret = ucsi->ppm->cmd(ucsi->ppm, &ctrl);
			if (ret)
				goto err;
		}

		/* Letting the PPM settle down. */
		msleep(20);

		ret = -ETIMEDOUT;
	} while (time_is_after_jiffies(tmo));

err:
	trace_ucsi_reset_ppm(&ctrl, ret);

	return ret;
}

static int ucsi_role_cmd(struct ucsi_connector *con, struct ucsi_control *ctrl)
{
	int ret;

	ret = ucsi_send_command(con->ucsi, ctrl, NULL, 0);
	if (ret == -ETIMEDOUT) {
		struct ucsi_control c;

		/* PPM most likely stopped responding. Resetting everything. */
		mutex_lock(&con->ucsi->ppm_lock);
		ucsi_reset_ppm(con->ucsi);
		mutex_unlock(&con->ucsi->ppm_lock);

		UCSI_CMD_SET_NTFY_ENABLE(c, UCSI_ENABLE_NTFY_ALL);
		ucsi_send_command(con->ucsi, &c, NULL, 0);

		ucsi_reset_connector(con, true);
	}

	return ret;
}

static int
ucsi_dr_swap(const struct typec_capability *cap, enum typec_data_role role)
{
	struct ucsi_connector *con = to_ucsi_connector(cap);
	struct ucsi_control ctrl;
	int ret = 0;

	mutex_lock(&con->lock);

	if (!con->partner) {
		ret = -ENOTCONN;
		goto out_unlock;
	}

	if ((con->status.partner_type == UCSI_CONSTAT_PARTNER_TYPE_DFP &&
	     role == TYPEC_DEVICE) ||
	    (con->status.partner_type == UCSI_CONSTAT_PARTNER_TYPE_UFP &&
	     role == TYPEC_HOST))
		goto out_unlock;

	UCSI_CMD_SET_UOR(ctrl, con, role);
	ret = ucsi_role_cmd(con, &ctrl);
	if (ret < 0)
		goto out_unlock;

	if (!wait_for_completion_timeout(&con->complete,
					msecs_to_jiffies(UCSI_SWAP_TIMEOUT_MS)))
		ret = -ETIMEDOUT;

out_unlock:
	mutex_unlock(&con->lock);

	return ret < 0 ? ret : 0;
}

static int
ucsi_pr_swap(const struct typec_capability *cap, enum typec_role role)
{
	struct ucsi_connector *con = to_ucsi_connector(cap);
	struct ucsi_control ctrl;
	int ret = 0;

	mutex_lock(&con->lock);

	if (!con->partner) {
		ret = -ENOTCONN;
		goto out_unlock;
	}

	if (con->status.pwr_dir == role)
		goto out_unlock;

	UCSI_CMD_SET_PDR(ctrl, con, role);
	ret = ucsi_role_cmd(con, &ctrl);
	if (ret < 0)
		goto out_unlock;

	if (!wait_for_completion_timeout(&con->complete,
				msecs_to_jiffies(UCSI_SWAP_TIMEOUT_MS))) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	/* Something has gone wrong while swapping the role */
	if (con->status.pwr_op_mode != UCSI_CONSTAT_PWR_OPMODE_PD) {
		ucsi_reset_connector(con, true);
		ret = -EPROTO;
	}

out_unlock:
	mutex_unlock(&con->lock);

	return ret;
}

static struct fwnode_handle *ucsi_find_fwnode(struct ucsi_connector *con)
{
	struct fwnode_handle *fwnode;
	int i = 1;

	device_for_each_child_node(con->ucsi->dev, fwnode)
		if (i++ == con->num)
			return fwnode;
	return NULL;
}

static int ucsi_register_port(struct ucsi *ucsi, int index)
{
	struct ucsi_connector *con = &ucsi->connector[index];
	struct typec_capability *cap = &con->typec_cap;
	enum typec_accessory *accessory = cap->accessory;
	struct ucsi_control ctrl;
	int ret;

	INIT_WORK(&con->work, ucsi_connector_change);
	init_completion(&con->complete);
	mutex_init(&con->lock);
	con->num = index + 1;
	con->ucsi = ucsi;

	/* Get connector capability */
	UCSI_CMD_GET_CONNECTOR_CAPABILITY(ctrl, con->num);
	ret = ucsi_run_command(ucsi, &ctrl, &con->cap, sizeof(con->cap));
	if (ret < 0)
		return ret;

	if (con->cap.op_mode & UCSI_CONCAP_OPMODE_DRP)
		cap->data = TYPEC_PORT_DRD;
	else if (con->cap.op_mode & UCSI_CONCAP_OPMODE_DFP)
		cap->data = TYPEC_PORT_DFP;
	else if (con->cap.op_mode & UCSI_CONCAP_OPMODE_UFP)
		cap->data = TYPEC_PORT_UFP;

	if (con->cap.provider && con->cap.consumer)
		cap->type = TYPEC_PORT_DRP;
	else if (con->cap.provider)
		cap->type = TYPEC_PORT_SRC;
	else if (con->cap.consumer)
		cap->type = TYPEC_PORT_SNK;

	cap->revision = ucsi->cap.typec_version;
	cap->pd_revision = ucsi->cap.pd_version;
	cap->prefer_role = TYPEC_NO_PREFERRED_ROLE;

	if (con->cap.op_mode & UCSI_CONCAP_OPMODE_AUDIO_ACCESSORY)
		*accessory++ = TYPEC_ACCESSORY_AUDIO;
	if (con->cap.op_mode & UCSI_CONCAP_OPMODE_DEBUG_ACCESSORY)
		*accessory = TYPEC_ACCESSORY_DEBUG;

	cap->fwnode = ucsi_find_fwnode(con);
	cap->dr_set = ucsi_dr_swap;
	cap->pr_set = ucsi_pr_swap;

	/* Register the connector */
	con->port = typec_register_port(ucsi->dev, cap);
	if (IS_ERR(con->port))
		return PTR_ERR(con->port);

	/* Alternate modes */
	ret = ucsi_register_altmodes(con, UCSI_RECIPIENT_CON);
	if (ret)
		dev_err(ucsi->dev, "con%d: failed to register alt modes\n",
			con->num);

	/* Get the status */
	UCSI_CMD_GET_CONNECTOR_STATUS(ctrl, con->num);
	ret = ucsi_run_command(ucsi, &ctrl, &con->status, sizeof(con->status));
	if (ret < 0) {
		dev_err(ucsi->dev, "con%d: failed to get status\n", con->num);
		return 0;
	}

	ucsi_pwr_opmode_change(con);
	typec_set_pwr_role(con->port, con->status.pwr_dir);

	switch (con->status.partner_type) {
	case UCSI_CONSTAT_PARTNER_TYPE_UFP:
		typec_set_data_role(con->port, TYPEC_HOST);
		break;
	case UCSI_CONSTAT_PARTNER_TYPE_DFP:
		typec_set_data_role(con->port, TYPEC_DEVICE);
		break;
	default:
		break;
	}

	/* Check if there is already something connected */
	if (con->status.connected)
		ucsi_register_partner(con);

	if (con->partner) {
		ret = ucsi_register_altmodes(con, UCSI_RECIPIENT_SOP);
		if (ret)
			dev_err(ucsi->dev,
				"con%d: failed to register alternate modes\n",
				con->num);
		else
			ucsi_altmode_update_active(con);
	}

	trace_ucsi_register_port(con->num, &con->status);

	return 0;
}

static void ucsi_init(struct work_struct *work)
{
	struct ucsi *ucsi = container_of(work, struct ucsi, work);
	struct ucsi_connector *con;
	struct ucsi_control ctrl;
	int ret;
	int i;

	mutex_lock(&ucsi->ppm_lock);

	/* Reset the PPM */
	ret = ucsi_reset_ppm(ucsi);
	if (ret) {
		dev_err(ucsi->dev, "failed to reset PPM!\n");
		goto err;
	}

	/* Enable basic notifications */
	UCSI_CMD_SET_NTFY_ENABLE(ctrl, UCSI_ENABLE_NTFY_CMD_COMPLETE |
					UCSI_ENABLE_NTFY_ERROR);
	ret = ucsi_run_command(ucsi, &ctrl, NULL, 0);
	if (ret < 0)
		goto err_reset;

	/* Get PPM capabilities */
	UCSI_CMD_GET_CAPABILITY(ctrl);
	ret = ucsi_run_command(ucsi, &ctrl, &ucsi->cap, sizeof(ucsi->cap));
	if (ret < 0)
		goto err_reset;

	if (!ucsi->cap.num_connectors) {
		ret = -ENODEV;
		goto err_reset;
	}

	/* Allocate the connectors. Released in ucsi_unregister_ppm() */
	ucsi->connector = kcalloc(ucsi->cap.num_connectors + 1,
				  sizeof(*ucsi->connector), GFP_KERNEL);
	if (!ucsi->connector) {
		ret = -ENOMEM;
		goto err_reset;
	}

	/* Register all connectors */
	for (i = 0; i < ucsi->cap.num_connectors; i++) {
		ret = ucsi_register_port(ucsi, i);
		if (ret)
			goto err_unregister;
	}

	/* Enable all notifications */
	UCSI_CMD_SET_NTFY_ENABLE(ctrl, UCSI_ENABLE_NTFY_ALL);
	ret = ucsi_run_command(ucsi, &ctrl, NULL, 0);
	if (ret < 0)
		goto err_unregister;

	mutex_unlock(&ucsi->ppm_lock);

	return;

err_unregister:
	for (con = ucsi->connector; con->port; con++) {
		ucsi_unregister_partner(con);
		ucsi_unregister_altmodes(con, UCSI_RECIPIENT_CON);
		typec_unregister_port(con->port);
		con->port = NULL;
	}

err_reset:
	ucsi_reset_ppm(ucsi);
err:
	mutex_unlock(&ucsi->ppm_lock);
	dev_err(ucsi->dev, "PPM init failed (%d)\n", ret);
}

/**
 * ucsi_register_ppm - Register UCSI PPM Interface
 * @dev: Device interface to the PPM
 * @ppm: The PPM interface
 *
 * Allocates UCSI instance, associates it with @ppm and returns it to the
 * caller, and schedules initialization of the interface.
 */
struct ucsi *ucsi_register_ppm(struct device *dev, struct ucsi_ppm *ppm)
{
	struct ucsi *ucsi;

	ucsi = kzalloc(sizeof(*ucsi), GFP_KERNEL);
	if (!ucsi)
		return ERR_PTR(-ENOMEM);

	INIT_WORK(&ucsi->work, ucsi_init);
	init_completion(&ucsi->complete);
	mutex_init(&ucsi->ppm_lock);

	ucsi->dev = dev;
	ucsi->ppm = ppm;

	/*
	 * Communication with the PPM takes a lot of time. It is not reasonable
	 * to initialize the driver here. Using a work for now.
	 */
	queue_work(system_long_wq, &ucsi->work);

	return ucsi;
}
EXPORT_SYMBOL_GPL(ucsi_register_ppm);

/**
 * ucsi_unregister_ppm - Unregister UCSI PPM Interface
 * @ucsi: struct ucsi associated with the PPM
 *
 * Unregister UCSI PPM that was created with ucsi_register().
 */
void ucsi_unregister_ppm(struct ucsi *ucsi)
{
	struct ucsi_control ctrl;
	int i;

	/* Make sure that we are not in the middle of driver initialization */
	cancel_work_sync(&ucsi->work);

	/* Disable everything except command complete notification */
	UCSI_CMD_SET_NTFY_ENABLE(ctrl, UCSI_ENABLE_NTFY_CMD_COMPLETE)
	ucsi_send_command(ucsi, &ctrl, NULL, 0);

	for (i = 0; i < ucsi->cap.num_connectors; i++) {
		cancel_work_sync(&ucsi->connector[i].work);
		ucsi_unregister_partner(&ucsi->connector[i]);
		ucsi_unregister_altmodes(&ucsi->connector[i],
					 UCSI_RECIPIENT_CON);
		typec_unregister_port(ucsi->connector[i].port);
	}

	ucsi_reset_ppm(ucsi);

	kfree(ucsi->connector);
	kfree(ucsi);
}
EXPORT_SYMBOL_GPL(ucsi_unregister_ppm);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Type-C Connector System Software Interface driver");
