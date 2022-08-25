/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2004 IBM Corporation
 * Copyright (C) 2015 Intel Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * Device driver for TCG/TCPA TPM (trusted platform module).
 * Specifications at www.trustedcomputinggroup.org
 */

#ifndef __TPM_H__
#define __TPM_H__

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/tpm.h>
#include <linux/highmem.h>
#include <linux/tpm_eventlog.h>

#ifdef CONFIG_X86
#include <asm/intel-family.h>
#endif

#define TPM_MINOR		224	/* officially assigned */
#define TPM_BUFSIZE		4096
#define TPM_NUM_DEVICES		65536
#define TPM_RETRY		50

enum tpm_timeout {
	TPM_TIMEOUT = 5,	/* msecs */
	TPM_TIMEOUT_RETRY = 100, /* msecs */
	TPM_TIMEOUT_RANGE_US = 300,	/* usecs */
	TPM_TIMEOUT_POLL = 1,	/* msecs */
	TPM_TIMEOUT_USECS_MIN = 100,      /* usecs */
	TPM_TIMEOUT_USECS_MAX = 500      /* usecs */
};

/* TPM addresses */
enum tpm_addr {
	TPM_SUPERIO_ADDR = 0x2E,
	TPM_ADDR = 0x4E,
};

#define TPM_WARN_RETRY          0x800
#define TPM_WARN_DOING_SELFTEST 0x802
#define TPM_ERR_DEACTIVATED     0x6
#define TPM_ERR_DISABLED        0x7
#define TPM_ERR_INVALID_POSTINIT 38

#define TPM_HEADER_SIZE		10

enum tpm2_const {
	TPM2_PLATFORM_PCR       =     24,
	TPM2_PCR_SELECT_MIN     = ((TPM2_PLATFORM_PCR + 7) / 8),
};

enum tpm2_timeouts {
	TPM2_TIMEOUT_A          =    750,
	TPM2_TIMEOUT_B          =   2000,
	TPM2_TIMEOUT_C          =    200,
	TPM2_TIMEOUT_D          =     30,
	TPM2_DURATION_SHORT     =     20,
	TPM2_DURATION_MEDIUM    =    750,
	TPM2_DURATION_LONG      =   2000,
	TPM2_DURATION_LONG_LONG = 300000,
	TPM2_DURATION_DEFAULT   = 120000,
};

enum tpm2_structures {
	TPM2_ST_NO_SESSIONS	= 0x8001,
	TPM2_ST_SESSIONS	= 0x8002,
};

/* Indicates from what layer of the software stack the error comes from */
#define TSS2_RC_LAYER_SHIFT	 16
#define TSS2_RESMGR_TPM_RC_LAYER (11 << TSS2_RC_LAYER_SHIFT)

enum tpm2_return_codes {
	TPM2_RC_SUCCESS		= 0x0000,
	TPM2_RC_HASH		= 0x0083, /* RC_FMT1 */
	TPM2_RC_HANDLE		= 0x008B,
	TPM2_RC_INITIALIZE	= 0x0100, /* RC_VER1 */
	TPM2_RC_FAILURE		= 0x0101,
	TPM2_RC_DISABLED	= 0x0120,
	TPM2_RC_COMMAND_CODE    = 0x0143,
	TPM2_RC_TESTING		= 0x090A, /* RC_WARN */
	TPM2_RC_REFERENCE_H0	= 0x0910,
	TPM2_RC_RETRY		= 0x0922,
};

enum tpm2_command_codes {
	TPM2_CC_FIRST		        = 0x011F,
	TPM2_CC_HIERARCHY_CONTROL       = 0x0121,
	TPM2_CC_HIERARCHY_CHANGE_AUTH   = 0x0129,
	TPM2_CC_CREATE_PRIMARY          = 0x0131,
	TPM2_CC_SEQUENCE_COMPLETE       = 0x013E,
	TPM2_CC_SELF_TEST	        = 0x0143,
	TPM2_CC_STARTUP		        = 0x0144,
	TPM2_CC_SHUTDOWN	        = 0x0145,
	TPM2_CC_NV_READ                 = 0x014E,
	TPM2_CC_CREATE		        = 0x0153,
	TPM2_CC_LOAD		        = 0x0157,
	TPM2_CC_SEQUENCE_UPDATE         = 0x015C,
	TPM2_CC_UNSEAL		        = 0x015E,
	TPM2_CC_CONTEXT_LOAD	        = 0x0161,
	TPM2_CC_CONTEXT_SAVE	        = 0x0162,
	TPM2_CC_FLUSH_CONTEXT	        = 0x0165,
	TPM2_CC_VERIFY_SIGNATURE        = 0x0177,
	TPM2_CC_GET_CAPABILITY	        = 0x017A,
	TPM2_CC_GET_RANDOM	        = 0x017B,
	TPM2_CC_PCR_READ	        = 0x017E,
	TPM2_CC_PCR_EXTEND	        = 0x0182,
	TPM2_CC_EVENT_SEQUENCE_COMPLETE = 0x0185,
	TPM2_CC_HASH_SEQUENCE_START     = 0x0186,
	TPM2_CC_CREATE_LOADED           = 0x0191,
	TPM2_CC_LAST		        = 0x0193, /* Spec 1.36 */
};

enum tpm2_permanent_handles {
	TPM2_RS_PW		= 0x40000009,
};

enum tpm2_capabilities {
	TPM2_CAP_HANDLES	= 1,
	TPM2_CAP_COMMANDS	= 2,
	TPM2_CAP_PCRS		= 5,
	TPM2_CAP_TPM_PROPERTIES = 6,
};

enum tpm2_properties {
	TPM_PT_TOTAL_COMMANDS	= 0x0129,
};

enum tpm2_startup_types {
	TPM2_SU_CLEAR	= 0x0000,
	TPM2_SU_STATE	= 0x0001,
};

enum tpm2_cc_attrs {
	TPM2_CC_ATTR_CHANDLES	= 25,
	TPM2_CC_ATTR_RHANDLE	= 28,
};

#define TPM_VID_INTEL    0x8086
#define TPM_VID_WINBOND  0x1050
#define TPM_VID_STM      0x104A

enum tpm_chip_flags {
	TPM_CHIP_FLAG_TPM2		= BIT(1),
	TPM_CHIP_FLAG_IRQ		= BIT(2),
	TPM_CHIP_FLAG_VIRTUAL		= BIT(3),
	TPM_CHIP_FLAG_HAVE_TIMEOUTS	= BIT(4),
	TPM_CHIP_FLAG_ALWAYS_POWERED	= BIT(5),
};

#define to_tpm_chip(d) container_of(d, struct tpm_chip, dev)

struct tpm_header {
	__be16 tag;
	__be32 length;
	union {
		__be32 ordinal;
		__be32 return_code;
	};
} __packed;

#define TPM_TAG_RQU_COMMAND 193

/* TPM2 specific constants. */
#define TPM2_SPACE_BUFFER_SIZE		16384 /* 16 kB */

struct	stclear_flags_t {
	__be16	tag;
	u8	deactivated;
	u8	disableForceClear;
	u8	physicalPresence;
	u8	physicalPresenceLock;
	u8	bGlobalLock;
} __packed;

struct	tpm_version_t {
	u8	Major;
	u8	Minor;
	u8	revMajor;
	u8	revMinor;
} __packed;

struct	tpm_version_1_2_t {
	__be16	tag;
	u8	Major;
	u8	Minor;
	u8	revMajor;
	u8	revMinor;
} __packed;

struct	timeout_t {
	__be32	a;
	__be32	b;
	__be32	c;
	__be32	d;
} __packed;

struct duration_t {
	__be32	tpm_short;
	__be32	tpm_medium;
	__be32	tpm_long;
} __packed;

struct permanent_flags_t {
	__be16	tag;
	u8	disable;
	u8	ownership;
	u8	deactivated;
	u8	readPubek;
	u8	disableOwnerClear;
	u8	allowMaintenance;
	u8	physicalPresenceLifetimeLock;
	u8	physicalPresenceHWEnable;
	u8	physicalPresenceCMDEnable;
	u8	CEKPUsed;
	u8	TPMpost;
	u8	TPMpostLock;
	u8	FIPS;
	u8	operator;
	u8	enableRevokeEK;
	u8	nvLocked;
	u8	readSRKPub;
	u8	tpmEstablished;
	u8	maintenanceDone;
	u8	disableFullDALogicInfo;
} __packed;

typedef union {
	struct	permanent_flags_t perm_flags;
	struct	stclear_flags_t	stclear_flags;
	__u8	owned;
	__be32	num_pcrs;
	struct	tpm_version_t	tpm_version;
	struct	tpm_version_1_2_t tpm_version_1_2;
	__be32	manufacturer_id;
	struct timeout_t  timeout;
	struct duration_t duration;
} cap_t;

enum tpm_capabilities {
	TPM_CAP_FLAG = 4,
	TPM_CAP_PROP = 5,
	TPM_CAP_VERSION_1_1 = 0x06,
	TPM_CAP_VERSION_1_2 = 0x1A,
};

enum tpm_sub_capabilities {
	TPM_CAP_PROP_PCR = 0x101,
	TPM_CAP_PROP_MANUFACTURER = 0x103,
	TPM_CAP_FLAG_PERM = 0x108,
	TPM_CAP_FLAG_VOL = 0x109,
	TPM_CAP_PROP_OWNER = 0x111,
	TPM_CAP_PROP_TIS_TIMEOUT = 0x115,
	TPM_CAP_PROP_TIS_DURATION = 0x120,
};


/* 128 bytes is an arbitrary cap. This could be as large as TPM_BUFSIZE - 18
 * bytes, but 128 is still a relatively large number of random bytes and
 * anything much bigger causes users of struct tpm_cmd_t to start getting
 * compiler warnings about stack frame size. */
#define TPM_MAX_RNG_DATA	128

/* A string buffer type for constructing TPM commands. This is based on the
 * ideas of string buffer code in security/keys/trusted.h but is heap based
 * in order to keep the stack usage minimal.
 */

enum tpm_buf_flags {
	TPM_BUF_OVERFLOW	= BIT(0),
};

struct tpm_buf {
	struct page *data_page;
	unsigned int flags;
	u8 *data;
};

static inline void tpm_buf_reset(struct tpm_buf *buf, u16 tag, u32 ordinal)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	head->tag = cpu_to_be16(tag);
	head->length = cpu_to_be32(sizeof(*head));
	head->ordinal = cpu_to_be32(ordinal);
}

static inline int tpm_buf_init(struct tpm_buf *buf, u16 tag, u32 ordinal)
{
	buf->data_page = alloc_page(GFP_HIGHUSER);
	if (!buf->data_page)
		return -ENOMEM;

	buf->flags = 0;
	buf->data = kmap(buf->data_page);
	tpm_buf_reset(buf, tag, ordinal);
	return 0;
}

static inline void tpm_buf_destroy(struct tpm_buf *buf)
{
	kunmap(buf->data_page);
	__free_page(buf->data_page);
}

static inline u32 tpm_buf_length(struct tpm_buf *buf)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	return be32_to_cpu(head->length);
}

static inline u16 tpm_buf_tag(struct tpm_buf *buf)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	return be16_to_cpu(head->tag);
}

static inline void tpm_buf_append(struct tpm_buf *buf,
				  const unsigned char *new_data,
				  unsigned int new_len)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;
	u32 len = tpm_buf_length(buf);

	/* Return silently if overflow has already happened. */
	if (buf->flags & TPM_BUF_OVERFLOW)
		return;

	if ((len + new_len) > PAGE_SIZE) {
		WARN(1, "tpm_buf: overflow\n");
		buf->flags |= TPM_BUF_OVERFLOW;
		return;
	}

	memcpy(&buf->data[len], new_data, new_len);
	head->length = cpu_to_be32(len + new_len);
}

static inline void tpm_buf_append_u8(struct tpm_buf *buf, const u8 value)
{
	tpm_buf_append(buf, &value, 1);
}

static inline void tpm_buf_append_u16(struct tpm_buf *buf, const u16 value)
{
	__be16 value2 = cpu_to_be16(value);

	tpm_buf_append(buf, (u8 *) &value2, 2);
}

static inline void tpm_buf_append_u32(struct tpm_buf *buf, const u32 value)
{
	__be32 value2 = cpu_to_be32(value);

	tpm_buf_append(buf, (u8 *) &value2, 4);
}

extern struct class *tpm_class;
extern struct class *tpmrm_class;
extern dev_t tpm_devt;
extern const struct file_operations tpm_fops;
extern const struct file_operations tpmrm_fops;
extern struct idr dev_nums_idr;

ssize_t tpm_transmit(struct tpm_chip *chip, u8 *buf, size_t bufsiz);
ssize_t tpm_transmit_cmd(struct tpm_chip *chip, struct tpm_buf *buf,
			 size_t min_rsp_body_length, const char *desc);
int tpm_get_timeouts(struct tpm_chip *);
int tpm_auto_startup(struct tpm_chip *chip);

int tpm1_pm_suspend(struct tpm_chip *chip, u32 tpm_suspend_pcr);
int tpm1_auto_startup(struct tpm_chip *chip);
int tpm1_do_selftest(struct tpm_chip *chip);
int tpm1_get_timeouts(struct tpm_chip *chip);
unsigned long tpm1_calc_ordinal_duration(struct tpm_chip *chip, u32 ordinal);
int tpm1_pcr_extend(struct tpm_chip *chip, u32 pcr_idx, const u8 *hash,
		    const char *log_msg);
int tpm1_pcr_read(struct tpm_chip *chip, u32 pcr_idx, u8 *res_buf);
ssize_t tpm1_getcap(struct tpm_chip *chip, u32 subcap_id, cap_t *cap,
		    const char *desc, size_t min_cap_length);
int tpm1_get_random(struct tpm_chip *chip, u8 *out, size_t max);
int tpm1_get_pcr_allocation(struct tpm_chip *chip);
unsigned long tpm_calc_ordinal_duration(struct tpm_chip *chip, u32 ordinal);
int tpm_pm_suspend(struct device *dev);
int tpm_pm_resume(struct device *dev);

static inline void tpm_msleep(unsigned int delay_msec)
{
	usleep_range((delay_msec * 1000) - TPM_TIMEOUT_RANGE_US,
		     delay_msec * 1000);
};

int tpm_chip_start(struct tpm_chip *chip);
void tpm_chip_stop(struct tpm_chip *chip);
struct tpm_chip *tpm_find_get_ops(struct tpm_chip *chip);
__must_check int tpm_try_get_ops(struct tpm_chip *chip);
void tpm_put_ops(struct tpm_chip *chip);

struct tpm_chip *tpm_chip_alloc(struct device *dev,
				const struct tpm_class_ops *ops);
struct tpm_chip *tpmm_chip_alloc(struct device *pdev,
				 const struct tpm_class_ops *ops);
int tpm_chip_register(struct tpm_chip *chip);
void tpm_chip_unregister(struct tpm_chip *chip);

void tpm_sysfs_add_device(struct tpm_chip *chip);


#ifdef CONFIG_ACPI
extern void tpm_add_ppi(struct tpm_chip *chip);
#else
static inline void tpm_add_ppi(struct tpm_chip *chip)
{
}
#endif

static inline u32 tpm2_rc_value(u32 rc)
{
	return (rc & BIT(7)) ? rc & 0xff : rc;
}

int tpm2_get_timeouts(struct tpm_chip *chip);
int tpm2_pcr_read(struct tpm_chip *chip, u32 pcr_idx,
		  struct tpm_digest *digest, u16 *digest_size_ptr);
int tpm2_pcr_extend(struct tpm_chip *chip, u32 pcr_idx,
		    struct tpm_digest *digests);
int tpm2_get_random(struct tpm_chip *chip, u8 *dest, size_t max);
void tpm2_flush_context(struct tpm_chip *chip, u32 handle);
int tpm2_seal_trusted(struct tpm_chip *chip,
		      struct trusted_key_payload *payload,
		      struct trusted_key_options *options);
int tpm2_unseal_trusted(struct tpm_chip *chip,
			struct trusted_key_payload *payload,
			struct trusted_key_options *options);
ssize_t tpm2_get_tpm_pt(struct tpm_chip *chip, u32 property_id,
			u32 *value, const char *desc);

ssize_t tpm2_get_pcr_allocation(struct tpm_chip *chip);
int tpm2_auto_startup(struct tpm_chip *chip);
void tpm2_shutdown(struct tpm_chip *chip, u16 shutdown_type);
unsigned long tpm2_calc_ordinal_duration(struct tpm_chip *chip, u32 ordinal);
int tpm2_probe(struct tpm_chip *chip);
int tpm2_find_cc(struct tpm_chip *chip, u32 cc);
int tpm2_init_space(struct tpm_space *space, unsigned int buf_size);
void tpm2_del_space(struct tpm_chip *chip, struct tpm_space *space);
void tpm2_flush_space(struct tpm_chip *chip);
int tpm2_prepare_space(struct tpm_chip *chip, struct tpm_space *space, u8 *cmd,
		       size_t cmdsiz);
int tpm2_commit_space(struct tpm_chip *chip, struct tpm_space *space, void *buf,
		      size_t *bufsiz);

void tpm_bios_log_setup(struct tpm_chip *chip);
void tpm_bios_log_teardown(struct tpm_chip *chip);
int tpm_dev_common_init(void);
void tpm_dev_common_exit(void);
#endif
