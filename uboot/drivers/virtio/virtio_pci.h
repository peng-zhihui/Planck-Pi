/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2018, Bin Meng <bmeng.cn@gmail.com>
 *
 * From Linux kernel include/uapi/linux/virtio_pci.h
 */

#ifndef _LINUX_VIRTIO_PCI_H
#define _LINUX_VIRTIO_PCI_H

#ifndef VIRTIO_PCI_NO_LEGACY

/* A 32-bit r/o bitmask of the features supported by the host */
#define VIRTIO_PCI_HOST_FEATURES	0

/* A 32-bit r/w bitmask of features activated by the guest */
#define VIRTIO_PCI_GUEST_FEATURES	4

/* A 32-bit r/w PFN for the currently selected queue */
#define VIRTIO_PCI_QUEUE_PFN		8

/* A 16-bit r/o queue size for the currently selected queue */
#define VIRTIO_PCI_QUEUE_NUM		12

/* A 16-bit r/w queue selector */
#define VIRTIO_PCI_QUEUE_SEL		14

/* A 16-bit r/w queue notifier */
#define VIRTIO_PCI_QUEUE_NOTIFY		16

/* An 8-bit device status register */
#define VIRTIO_PCI_STATUS		18

/*
 * An 8-bit r/o interrupt status register. Reading the value will return the
 * current contents of the ISR and will also clear it. This is effectively
 * a read-and-acknowledge.
 */
#define VIRTIO_PCI_ISR			19

/* MSI-X registers: only enabled if MSI-X is enabled */

/* A 16-bit vector for configuration changes */
#define VIRTIO_MSI_CONFIG_VECTOR	20
/* A 16-bit vector for selected queue notifications */
#define VIRTIO_MSI_QUEUE_VECTOR		22

/*
 * The remaining space is defined by each driver as the per-driver
 * configuration space
 */
#define VIRTIO_PCI_CONFIG_OFF(msix)	((msix) ? 24 : 20)

/* Virtio ABI version, this must match exactly */
#define VIRTIO_PCI_ABI_VERSION		0

/*
 * How many bits to shift physical queue address written to QUEUE_PFN.
 * 12 is historical, and due to x86 page size.
 */
#define VIRTIO_PCI_QUEUE_ADDR_SHIFT	12

/*
 * The alignment to use between consumer and producer parts of vring.
 * x86 pagesize again.
 */
#define VIRTIO_PCI_VRING_ALIGN		4096

#endif /* VIRTIO_PCI_NO_LEGACY */

/* The bit of the ISR which indicates a device configuration change */
#define VIRTIO_PCI_ISR_CONFIG		0x2
/* Vector value used to disable MSI for queue */
#define VIRTIO_MSI_NO_VECTOR		0xffff

#ifndef VIRTIO_PCI_NO_MODERN

/* IDs for different capabilities.  Must all exist. */

/* Common configuration */
#define VIRTIO_PCI_CAP_COMMON_CFG	1
/* Notifications */
#define VIRTIO_PCI_CAP_NOTIFY_CFG	2
/* ISR access */
#define VIRTIO_PCI_CAP_ISR_CFG		3
/* Device specific configuration */
#define VIRTIO_PCI_CAP_DEVICE_CFG	4
/* PCI configuration access */
#define VIRTIO_PCI_CAP_PCI_CFG		5

/* This is the PCI capability header: */
struct virtio_pci_cap {
	__u8 cap_vndr;		/* Generic PCI field: PCI_CAP_ID_VNDR */
	__u8 cap_next;		/* Generic PCI field: next ptr */
	__u8 cap_len;		/* Generic PCI field: capability length */
	__u8 cfg_type;		/* Identifies the structure */
	__u8 bar;		/* Where to find it */
	__u8 padding[3];	/* Pad to full dword */
	__le32 offset;		/* Offset within bar */
	__le32 length;		/* Length of the structure, in bytes */
};

struct virtio_pci_notify_cap {
	struct virtio_pci_cap cap;
	__le32 notify_off_multiplier;	/* Multiplier for queue_notify_off */
};

/* Fields in VIRTIO_PCI_CAP_COMMON_CFG: */
struct virtio_pci_common_cfg {
	/* About the whole device */
	__le32 device_feature_select;	/* read-write */
	__le32 device_feature;		/* read-only */
	__le32 guest_feature_select;	/* read-write */
	__le32 guest_feature;		/* read-write */
	__le16 msix_config;		/* read-write */
	__le16 num_queues;		/* read-only */
	__u8 device_status;		/* read-write */
	__u8 config_generation;		/* read-only */

	/* About a specific virtqueue */
	__le16 queue_select;		/* read-write */
	__le16 queue_size;		/* read-write, power of 2 */
	__le16 queue_msix_vector;	/* read-write */
	__le16 queue_enable;		/* read-write */
	__le16 queue_notify_off;	/* read-only */
	__le32 queue_desc_lo;		/* read-write */
	__le32 queue_desc_hi;		/* read-write */
	__le32 queue_avail_lo;		/* read-write */
	__le32 queue_avail_hi;		/* read-write */
	__le32 queue_used_lo;		/* read-write */
	__le32 queue_used_hi;		/* read-write */
};

/* Fields in VIRTIO_PCI_CAP_PCI_CFG: */
struct virtio_pci_cfg_cap {
	struct virtio_pci_cap cap;
	__u8 pci_cfg_data[4];		/* Data for BAR access */
};

/* Macro versions of offsets for the Old Timers! */
#define VIRTIO_PCI_CAP_VNDR		0
#define VIRTIO_PCI_CAP_NEXT		1
#define VIRTIO_PCI_CAP_LEN		2
#define VIRTIO_PCI_CAP_CFG_TYPE		3
#define VIRTIO_PCI_CAP_BAR		4
#define VIRTIO_PCI_CAP_OFFSET		8
#define VIRTIO_PCI_CAP_LENGTH		12

#define VIRTIO_PCI_NOTIFY_CAP_MULT	16

#define VIRTIO_PCI_COMMON_DFSELECT	0
#define VIRTIO_PCI_COMMON_DF		4
#define VIRTIO_PCI_COMMON_GFSELECT	8
#define VIRTIO_PCI_COMMON_GF		12
#define VIRTIO_PCI_COMMON_MSIX		16
#define VIRTIO_PCI_COMMON_NUMQ		18
#define VIRTIO_PCI_COMMON_STATUS	20
#define VIRTIO_PCI_COMMON_CFGGENERATION	21
#define VIRTIO_PCI_COMMON_Q_SELECT	22
#define VIRTIO_PCI_COMMON_Q_SIZE	24
#define VIRTIO_PCI_COMMON_Q_MSIX	26
#define VIRTIO_PCI_COMMON_Q_ENABLE	28
#define VIRTIO_PCI_COMMON_Q_NOFF	30
#define VIRTIO_PCI_COMMON_Q_DESCLO	32
#define VIRTIO_PCI_COMMON_Q_DESCHI	36
#define VIRTIO_PCI_COMMON_Q_AVAILLO	40
#define VIRTIO_PCI_COMMON_Q_AVAILHI	44
#define VIRTIO_PCI_COMMON_Q_USEDLO	48
#define VIRTIO_PCI_COMMON_Q_USEDHI	52

#endif /* VIRTIO_PCI_NO_MODERN */

#endif /* _LINUX_VIRTIO_PCI_H */
