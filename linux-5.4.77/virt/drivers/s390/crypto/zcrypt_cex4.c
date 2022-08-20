// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright IBM Corp. 2012, 2019
 *  Author(s): Holger Dengler <hd@linux.vnet.ibm.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/mod_devicetable.h>

#include "ap_bus.h"
#include "zcrypt_api.h"
#include "zcrypt_msgtype6.h"
#include "zcrypt_msgtype50.h"
#include "zcrypt_error.h"
#include "zcrypt_cex4.h"
#include "zcrypt_ccamisc.h"

#define CEX4A_MIN_MOD_SIZE	  1	/*    8 bits	*/
#define CEX4A_MAX_MOD_SIZE_2K	256	/* 2048 bits	*/
#define CEX4A_MAX_MOD_SIZE_4K	512	/* 4096 bits	*/

#define CEX4C_MIN_MOD_SIZE	 16	/*  256 bits	*/
#define CEX4C_MAX_MOD_SIZE	512	/* 4096 bits	*/

#define CEX4A_MAX_MESSAGE_SIZE	MSGTYPE50_CRB3_MAX_MSG_SIZE
#define CEX4C_MAX_MESSAGE_SIZE	MSGTYPE06_MAX_MSG_SIZE

/* Waiting time for requests to be processed.
 * Currently there are some types of request which are not deterministic.
 * But the maximum time limit managed by the stomper code is set to 60sec.
 * Hence we have to wait at least that time period.
 */
#define CEX4_CLEANUP_TIME	(900*HZ)

MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("CEX4/CEX5/CEX6/CEX7 Cryptographic Card device driver, " \
		   "Copyright IBM Corp. 2019");
MODULE_LICENSE("GPL");

static struct ap_device_id zcrypt_cex4_card_ids[] = {
	{ .dev_type = AP_DEVICE_TYPE_CEX4,
	  .match_flags = AP_DEVICE_ID_MATCH_CARD_TYPE },
	{ .dev_type = AP_DEVICE_TYPE_CEX5,
	  .match_flags = AP_DEVICE_ID_MATCH_CARD_TYPE },
	{ .dev_type = AP_DEVICE_TYPE_CEX6,
	  .match_flags = AP_DEVICE_ID_MATCH_CARD_TYPE },
	{ .dev_type = AP_DEVICE_TYPE_CEX7,
	  .match_flags = AP_DEVICE_ID_MATCH_CARD_TYPE },
	{ /* end of list */ },
};

MODULE_DEVICE_TABLE(ap, zcrypt_cex4_card_ids);

static struct ap_device_id zcrypt_cex4_queue_ids[] = {
	{ .dev_type = AP_DEVICE_TYPE_CEX4,
	  .match_flags = AP_DEVICE_ID_MATCH_QUEUE_TYPE },
	{ .dev_type = AP_DEVICE_TYPE_CEX5,
	  .match_flags = AP_DEVICE_ID_MATCH_QUEUE_TYPE },
	{ .dev_type = AP_DEVICE_TYPE_CEX6,
	  .match_flags = AP_DEVICE_ID_MATCH_QUEUE_TYPE },
	{ .dev_type = AP_DEVICE_TYPE_CEX7,
	  .match_flags = AP_DEVICE_ID_MATCH_QUEUE_TYPE },
	{ /* end of list */ },
};

MODULE_DEVICE_TABLE(ap, zcrypt_cex4_queue_ids);

/*
 * CCA card addditional device attributes
 */
static ssize_t serialnr_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct cca_info ci;
	struct ap_card *ac = to_ap_card(dev);
	struct zcrypt_card *zc = ac->private;

	memset(&ci, 0, sizeof(ci));

	if (ap_domain_index >= 0)
		cca_get_info(ac->id, ap_domain_index, &ci, zc->online);

	return snprintf(buf, PAGE_SIZE, "%s\n", ci.serial);
}
static DEVICE_ATTR_RO(serialnr);

static struct attribute *cca_card_attrs[] = {
	&dev_attr_serialnr.attr,
	NULL,
};

static const struct attribute_group cca_card_attr_group = {
	.attrs = cca_card_attrs,
};

/*
 * CCA queue addditional device attributes
 */
static ssize_t mkvps_show(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	int n = 0;
	struct cca_info ci;
	struct zcrypt_queue *zq = to_ap_queue(dev)->private;
	static const char * const cao_state[] = { "invalid", "valid" };
	static const char * const new_state[] = { "empty", "partial", "full" };

	memset(&ci, 0, sizeof(ci));

	cca_get_info(AP_QID_CARD(zq->queue->qid),
		     AP_QID_QUEUE(zq->queue->qid),
		     &ci, zq->online);

	if (ci.new_mk_state >= '1' && ci.new_mk_state <= '3')
		n = snprintf(buf, PAGE_SIZE, "AES NEW: %s 0x%016llx\n",
			     new_state[ci.new_mk_state - '1'], ci.new_mkvp);
	else
		n = snprintf(buf, PAGE_SIZE, "AES NEW: - -\n");

	if (ci.cur_mk_state >= '1' && ci.cur_mk_state <= '2')
		n += snprintf(buf + n, PAGE_SIZE - n, "AES CUR: %s 0x%016llx\n",
			      cao_state[ci.cur_mk_state - '1'], ci.cur_mkvp);
	else
		n += snprintf(buf + n, PAGE_SIZE - n, "AES CUR: - -\n");

	if (ci.old_mk_state >= '1' && ci.old_mk_state <= '2')
		n += snprintf(buf + n, PAGE_SIZE - n, "AES OLD: %s 0x%016llx\n",
			      cao_state[ci.old_mk_state - '1'], ci.old_mkvp);
	else
		n += snprintf(buf + n, PAGE_SIZE - n, "AES OLD: - -\n");

	return n;
}
static DEVICE_ATTR_RO(mkvps);

static struct attribute *cca_queue_attrs[] = {
	&dev_attr_mkvps.attr,
	NULL,
};

static const struct attribute_group cca_queue_attr_group = {
	.attrs = cca_queue_attrs,
};

/**
 * Probe function for CEX4/CEX5/CEX6/CEX7 card device. It always
 * accepts the AP device since the bus_match already checked
 * the hardware type.
 * @ap_dev: pointer to the AP device.
 */
static int zcrypt_cex4_card_probe(struct ap_device *ap_dev)
{
	/*
	 * Normalized speed ratings per crypto adapter
	 * MEX_1k, MEX_2k, MEX_4k, CRT_1k, CRT_2k, CRT_4k, RNG, SECKEY
	 */
	static const int CEX4A_SPEED_IDX[] = {
		 14,  19, 249, 42, 228, 1458, 0, 0};
	static const int CEX5A_SPEED_IDX[] = {
		  8,   9,  20, 18,  66,	 458, 0, 0};
	static const int CEX6A_SPEED_IDX[] = {
		  6,   9,  20, 17,  65,	 438, 0, 0};
	static const int CEX7A_SPEED_IDX[] = {
		  6,   8,  17, 15,  54,	 362, 0, 0};

	static const int CEX4C_SPEED_IDX[] = {
		 59,  69, 308, 83, 278, 2204, 209, 40};
	static const int CEX5C_SPEED_IDX[] = {
		 24,  31,  50, 37,  90,	 479,  27, 10};
	static const int CEX6C_SPEED_IDX[] = {
		 16,  20,  32, 27,  77,	 455,  24,  9};
	static const int CEX7C_SPEED_IDX[] = {
		 14,  16,  26, 23,  64,	 376,  23,  8};

	static const int CEX4P_SPEED_IDX[] = {
		  0,   0,   0,	 0,   0,   0,	0,  50};
	static const int CEX5P_SPEED_IDX[] = {
		  0,   0,   0,	 0,   0,   0,	0,  10};
	static const int CEX6P_SPEED_IDX[] = {
		  0,   0,   0,	 0,   0,   0,	0,   9};
	static const int CEX7P_SPEED_IDX[] = {
		  0,   0,   0,	 0,   0,   0,	0,   8};

	struct ap_card *ac = to_ap_card(&ap_dev->device);
	struct zcrypt_card *zc;
	int rc = 0;

	zc = zcrypt_card_alloc();
	if (!zc)
		return -ENOMEM;
	zc->card = ac;
	ac->private = zc;
	if (ap_test_bit(&ac->functions, AP_FUNC_ACCEL)) {
		if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX4) {
			zc->type_string = "CEX4A";
			zc->user_space_type = ZCRYPT_CEX4;
			memcpy(zc->speed_rating, CEX4A_SPEED_IDX,
			       sizeof(CEX4A_SPEED_IDX));
		} else if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX5) {
			zc->type_string = "CEX5A";
			zc->user_space_type = ZCRYPT_CEX5;
			memcpy(zc->speed_rating, CEX5A_SPEED_IDX,
			       sizeof(CEX5A_SPEED_IDX));
		} else if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX6) {
			zc->type_string = "CEX6A";
			zc->user_space_type = ZCRYPT_CEX6;
			memcpy(zc->speed_rating, CEX6A_SPEED_IDX,
			       sizeof(CEX6A_SPEED_IDX));
		} else {
			zc->type_string = "CEX7A";
			/* wrong user space type, just for compatibility
			 * with the ZCRYPT_STATUS_MASK ioctl.
			 */
			zc->user_space_type = ZCRYPT_CEX6;
			memcpy(zc->speed_rating, CEX7A_SPEED_IDX,
			       sizeof(CEX7A_SPEED_IDX));
		}
		zc->min_mod_size = CEX4A_MIN_MOD_SIZE;
		if (ap_test_bit(&ac->functions, AP_FUNC_MEX4K) &&
		    ap_test_bit(&ac->functions, AP_FUNC_CRT4K)) {
			zc->max_mod_size = CEX4A_MAX_MOD_SIZE_4K;
			zc->max_exp_bit_length =
				CEX4A_MAX_MOD_SIZE_4K;
		} else {
			zc->max_mod_size = CEX4A_MAX_MOD_SIZE_2K;
			zc->max_exp_bit_length =
				CEX4A_MAX_MOD_SIZE_2K;
		}
	} else if (ap_test_bit(&ac->functions, AP_FUNC_COPRO)) {
		if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX4) {
			zc->type_string = "CEX4C";
			/* wrong user space type, must be CEX4
			 * just keep it for cca compatibility
			 */
			zc->user_space_type = ZCRYPT_CEX3C;
			memcpy(zc->speed_rating, CEX4C_SPEED_IDX,
			       sizeof(CEX4C_SPEED_IDX));
		} else if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX5) {
			zc->type_string = "CEX5C";
			/* wrong user space type, must be CEX5
			 * just keep it for cca compatibility
			 */
			zc->user_space_type = ZCRYPT_CEX3C;
			memcpy(zc->speed_rating, CEX5C_SPEED_IDX,
			       sizeof(CEX5C_SPEED_IDX));
		} else if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX6) {
			zc->type_string = "CEX6C";
			/* wrong user space type, must be CEX6
			 * just keep it for cca compatibility
			 */
			zc->user_space_type = ZCRYPT_CEX3C;
			memcpy(zc->speed_rating, CEX6C_SPEED_IDX,
			       sizeof(CEX6C_SPEED_IDX));
		} else {
			zc->type_string = "CEX7C";
			/* wrong user space type, must be CEX7
			 * just keep it for cca compatibility
			 */
			zc->user_space_type = ZCRYPT_CEX3C;
			memcpy(zc->speed_rating, CEX7C_SPEED_IDX,
			       sizeof(CEX7C_SPEED_IDX));
		}
		zc->min_mod_size = CEX4C_MIN_MOD_SIZE;
		zc->max_mod_size = CEX4C_MAX_MOD_SIZE;
		zc->max_exp_bit_length = CEX4C_MAX_MOD_SIZE;
	} else if (ap_test_bit(&ac->functions, AP_FUNC_EP11)) {
		if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX4) {
			zc->type_string = "CEX4P";
			zc->user_space_type = ZCRYPT_CEX4;
			memcpy(zc->speed_rating, CEX4P_SPEED_IDX,
			       sizeof(CEX4P_SPEED_IDX));
		} else if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX5) {
			zc->type_string = "CEX5P";
			zc->user_space_type = ZCRYPT_CEX5;
			memcpy(zc->speed_rating, CEX5P_SPEED_IDX,
			       sizeof(CEX5P_SPEED_IDX));
		} else if (ac->ap_dev.device_type == AP_DEVICE_TYPE_CEX6) {
			zc->type_string = "CEX6P";
			zc->user_space_type = ZCRYPT_CEX6;
			memcpy(zc->speed_rating, CEX6P_SPEED_IDX,
			       sizeof(CEX6P_SPEED_IDX));
		} else {
			zc->type_string = "CEX7P";
			/* wrong user space type, just for compatibility
			 * with the ZCRYPT_STATUS_MASK ioctl.
			 */
			zc->user_space_type = ZCRYPT_CEX6;
			memcpy(zc->speed_rating, CEX7P_SPEED_IDX,
			       sizeof(CEX7P_SPEED_IDX));
		}
		zc->min_mod_size = CEX4C_MIN_MOD_SIZE;
		zc->max_mod_size = CEX4C_MAX_MOD_SIZE;
		zc->max_exp_bit_length = CEX4C_MAX_MOD_SIZE;
	} else {
		zcrypt_card_free(zc);
		return -ENODEV;
	}
	zc->online = 1;

	rc = zcrypt_card_register(zc);
	if (rc) {
		ac->private = NULL;
		zcrypt_card_free(zc);
		goto out;
	}

	if (ap_test_bit(&ac->functions, AP_FUNC_COPRO)) {
		rc = sysfs_create_group(&ap_dev->device.kobj,
					&cca_card_attr_group);
		if (rc)
			zcrypt_card_unregister(zc);
	}

out:
	return rc;
}

/**
 * This is called to remove the CEX4/CEX5/CEX6/CEX7 card driver
 * information if an AP card device is removed.
 */
static void zcrypt_cex4_card_remove(struct ap_device *ap_dev)
{
	struct ap_card *ac = to_ap_card(&ap_dev->device);
	struct zcrypt_card *zc = ac->private;

	if (ap_test_bit(&ac->functions, AP_FUNC_COPRO))
		sysfs_remove_group(&ap_dev->device.kobj, &cca_card_attr_group);
	if (zc)
		zcrypt_card_unregister(zc);
}

static struct ap_driver zcrypt_cex4_card_driver = {
	.probe = zcrypt_cex4_card_probe,
	.remove = zcrypt_cex4_card_remove,
	.ids = zcrypt_cex4_card_ids,
	.flags = AP_DRIVER_FLAG_DEFAULT,
};

/**
 * Probe function for CEX4/CEX5/CEX6/CEX7 queue device. It always
 * accepts the AP device since the bus_match already checked
 * the hardware type.
 * @ap_dev: pointer to the AP device.
 */
static int zcrypt_cex4_queue_probe(struct ap_device *ap_dev)
{
	struct ap_queue *aq = to_ap_queue(&ap_dev->device);
	struct zcrypt_queue *zq;
	int rc;

	if (ap_test_bit(&aq->card->functions, AP_FUNC_ACCEL)) {
		zq = zcrypt_queue_alloc(CEX4A_MAX_MESSAGE_SIZE);
		if (!zq)
			return -ENOMEM;
		zq->ops = zcrypt_msgtype(MSGTYPE50_NAME,
					 MSGTYPE50_VARIANT_DEFAULT);
	} else if (ap_test_bit(&aq->card->functions, AP_FUNC_COPRO)) {
		zq = zcrypt_queue_alloc(CEX4C_MAX_MESSAGE_SIZE);
		if (!zq)
			return -ENOMEM;
		zq->ops = zcrypt_msgtype(MSGTYPE06_NAME,
					 MSGTYPE06_VARIANT_DEFAULT);
	} else if (ap_test_bit(&aq->card->functions, AP_FUNC_EP11)) {
		zq = zcrypt_queue_alloc(CEX4C_MAX_MESSAGE_SIZE);
		if (!zq)
			return -ENOMEM;
		zq->ops = zcrypt_msgtype(MSGTYPE06_NAME,
					 MSGTYPE06_VARIANT_EP11);
	} else {
		return -ENODEV;
	}

	zq->queue = aq;
	zq->online = 1;
	atomic_set(&zq->load, 0);
	ap_queue_init_state(aq);
	ap_queue_init_reply(aq, &zq->reply);
	aq->request_timeout = CEX4_CLEANUP_TIME,
	aq->private = zq;
	rc = zcrypt_queue_register(zq);
	if (rc) {
		aq->private = NULL;
		zcrypt_queue_free(zq);
		goto out;
	}

	if (ap_test_bit(&aq->card->functions, AP_FUNC_COPRO)) {
		rc = sysfs_create_group(&ap_dev->device.kobj,
					&cca_queue_attr_group);
		if (rc)
			zcrypt_queue_unregister(zq);
	}

out:
	return rc;
}

/**
 * This is called to remove the CEX4/CEX5/CEX6/CEX7 queue driver
 * information if an AP queue device is removed.
 */
static void zcrypt_cex4_queue_remove(struct ap_device *ap_dev)
{
	struct ap_queue *aq = to_ap_queue(&ap_dev->device);
	struct zcrypt_queue *zq = aq->private;

	if (ap_test_bit(&aq->card->functions, AP_FUNC_COPRO))
		sysfs_remove_group(&ap_dev->device.kobj, &cca_queue_attr_group);
	if (zq)
		zcrypt_queue_unregister(zq);
}

static struct ap_driver zcrypt_cex4_queue_driver = {
	.probe = zcrypt_cex4_queue_probe,
	.remove = zcrypt_cex4_queue_remove,
	.suspend = ap_queue_suspend,
	.resume = ap_queue_resume,
	.ids = zcrypt_cex4_queue_ids,
	.flags = AP_DRIVER_FLAG_DEFAULT,
};

int __init zcrypt_cex4_init(void)
{
	int rc;

	rc = ap_driver_register(&zcrypt_cex4_card_driver,
				THIS_MODULE, "cex4card");
	if (rc)
		return rc;

	rc = ap_driver_register(&zcrypt_cex4_queue_driver,
				THIS_MODULE, "cex4queue");
	if (rc)
		ap_driver_unregister(&zcrypt_cex4_card_driver);

	return rc;
}

void __exit zcrypt_cex4_exit(void)
{
	ap_driver_unregister(&zcrypt_cex4_queue_driver);
	ap_driver_unregister(&zcrypt_cex4_card_driver);
}

module_init(zcrypt_cex4_init);
module_exit(zcrypt_cex4_exit);
