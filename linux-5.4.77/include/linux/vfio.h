/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VFIO API definition
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */
#ifndef VFIO_H
#define VFIO_H


#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <uapi/linux/vfio.h>

/**
 * struct vfio_device_ops - VFIO bus driver device callbacks
 *
 * @open: Called when userspace creates new file descriptor for device
 * @release: Called when userspace releases file descriptor for device
 * @read: Perform read(2) on device file descriptor
 * @write: Perform write(2) on device file descriptor
 * @ioctl: Perform ioctl(2) on device file descriptor, supporting VFIO_DEVICE_*
 *         operations documented below
 * @mmap: Perform mmap(2) on a region of the device file descriptor
 * @request: Request for the bus driver to release the device
 */
struct vfio_device_ops {
	char	*name;
	int	(*open)(void *device_data);
	void	(*release)(void *device_data);
	ssize_t	(*read)(void *device_data, char __user *buf,
			size_t count, loff_t *ppos);
	ssize_t	(*write)(void *device_data, const char __user *buf,
			 size_t count, loff_t *size);
	long	(*ioctl)(void *device_data, unsigned int cmd,
			 unsigned long arg);
	int	(*mmap)(void *device_data, struct vm_area_struct *vma);
	void	(*request)(void *device_data, unsigned int count);
};

extern struct iommu_group *vfio_iommu_group_get(struct device *dev);
extern void vfio_iommu_group_put(struct iommu_group *group, struct device *dev);

extern int vfio_add_group_dev(struct device *dev,
			      const struct vfio_device_ops *ops,
			      void *device_data);

extern void *vfio_del_group_dev(struct device *dev);
extern struct vfio_device *vfio_device_get_from_dev(struct device *dev);
extern void vfio_device_put(struct vfio_device *device);
extern void *vfio_device_data(struct vfio_device *device);

/**
 * struct vfio_iommu_driver_ops - VFIO IOMMU driver callbacks
 */
struct vfio_iommu_driver_ops {
	char		*name;
	struct module	*owner;
	void		*(*open)(unsigned long arg);
	void		(*release)(void *iommu_data);
	ssize_t		(*read)(void *iommu_data, char __user *buf,
				size_t count, loff_t *ppos);
	ssize_t		(*write)(void *iommu_data, const char __user *buf,
				 size_t count, loff_t *size);
	long		(*ioctl)(void *iommu_data, unsigned int cmd,
				 unsigned long arg);
	int		(*mmap)(void *iommu_data, struct vm_area_struct *vma);
	int		(*attach_group)(void *iommu_data,
					struct iommu_group *group);
	void		(*detach_group)(void *iommu_data,
					struct iommu_group *group);
	int		(*pin_pages)(void *iommu_data, unsigned long *user_pfn,
				     int npage, int prot,
				     unsigned long *phys_pfn);
	int		(*unpin_pages)(void *iommu_data,
				       unsigned long *user_pfn, int npage);
	int		(*register_notifier)(void *iommu_data,
					     unsigned long *events,
					     struct notifier_block *nb);
	int		(*unregister_notifier)(void *iommu_data,
					       struct notifier_block *nb);
};

extern int vfio_register_iommu_driver(const struct vfio_iommu_driver_ops *ops);

extern void vfio_unregister_iommu_driver(
				const struct vfio_iommu_driver_ops *ops);

/*
 * External user API
 */
extern struct vfio_group *vfio_group_get_external_user(struct file *filep);
extern void vfio_group_put_external_user(struct vfio_group *group);
extern bool vfio_external_group_match_file(struct vfio_group *group,
					   struct file *filep);
extern int vfio_external_user_iommu_id(struct vfio_group *group);
extern long vfio_external_check_extension(struct vfio_group *group,
					  unsigned long arg);

#define VFIO_PIN_PAGES_MAX_ENTRIES	(PAGE_SIZE/sizeof(unsigned long))

extern int vfio_pin_pages(struct device *dev, unsigned long *user_pfn,
			  int npage, int prot, unsigned long *phys_pfn);
extern int vfio_unpin_pages(struct device *dev, unsigned long *user_pfn,
			    int npage);

/* each type has independent events */
enum vfio_notify_type {
	VFIO_IOMMU_NOTIFY = 0,
	VFIO_GROUP_NOTIFY = 1,
};

/* events for VFIO_IOMMU_NOTIFY */
#define VFIO_IOMMU_NOTIFY_DMA_UNMAP	BIT(0)

/* events for VFIO_GROUP_NOTIFY */
#define VFIO_GROUP_NOTIFY_SET_KVM	BIT(0)

extern int vfio_register_notifier(struct device *dev,
				  enum vfio_notify_type type,
				  unsigned long *required_events,
				  struct notifier_block *nb);
extern int vfio_unregister_notifier(struct device *dev,
				    enum vfio_notify_type type,
				    struct notifier_block *nb);

struct kvm;
extern void vfio_group_set_kvm(struct vfio_group *group, struct kvm *kvm);

/*
 * Sub-module helpers
 */
struct vfio_info_cap {
	struct vfio_info_cap_header *buf;
	size_t size;
};
extern struct vfio_info_cap_header *vfio_info_cap_add(
		struct vfio_info_cap *caps, size_t size, u16 id, u16 version);
extern void vfio_info_cap_shift(struct vfio_info_cap *caps, size_t offset);

extern int vfio_info_add_capability(struct vfio_info_cap *caps,
				    struct vfio_info_cap_header *cap,
				    size_t size);

extern int vfio_set_irqs_validate_and_prepare(struct vfio_irq_set *hdr,
					      int num_irqs, int max_irq_type,
					      size_t *data_size);

struct pci_dev;
#if IS_ENABLED(CONFIG_VFIO_SPAPR_EEH)
extern void vfio_spapr_pci_eeh_open(struct pci_dev *pdev);
extern void vfio_spapr_pci_eeh_release(struct pci_dev *pdev);
extern long vfio_spapr_iommu_eeh_ioctl(struct iommu_group *group,
				       unsigned int cmd,
				       unsigned long arg);
#else
static inline void vfio_spapr_pci_eeh_open(struct pci_dev *pdev)
{
}

static inline void vfio_spapr_pci_eeh_release(struct pci_dev *pdev)
{
}

static inline long vfio_spapr_iommu_eeh_ioctl(struct iommu_group *group,
					      unsigned int cmd,
					      unsigned long arg)
{
	return -ENOTTY;
}
#endif /* CONFIG_VFIO_SPAPR_EEH */

/*
 * IRQfd - generic
 */
struct virqfd {
	void			*opaque;
	struct eventfd_ctx	*eventfd;
	int			(*handler)(void *, void *);
	void			(*thread)(void *, void *);
	void			*data;
	struct work_struct	inject;
	wait_queue_entry_t		wait;
	poll_table		pt;
	struct work_struct	shutdown;
	struct virqfd		**pvirqfd;
};

extern int vfio_virqfd_enable(void *opaque,
			      int (*handler)(void *, void *),
			      void (*thread)(void *, void *),
			      void *data, struct virqfd **pvirqfd, int fd);
extern void vfio_virqfd_disable(struct virqfd **pvirqfd);

#endif /* VFIO_H */
