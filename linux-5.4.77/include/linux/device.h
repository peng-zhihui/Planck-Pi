// SPDX-License-Identifier: GPL-2.0
/*
 * device.h - generic, centralized driver model
 *
 * Copyright (c) 2001-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2004-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2008-2009 Novell Inc.
 *
 * See Documentation/driver-api/driver-model/ for more information.
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <linux/ioport.h>
#include <linux/kobject.h>
#include <linux/klist.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/uidgid.h>
#include <linux/gfp.h>
#include <linux/overflow.h>
#include <asm/device.h>

struct device;
struct device_private;
struct device_driver;
struct driver_private;
struct module;
struct class;
struct subsys_private;
struct bus_type;
struct device_node;
struct fwnode_handle;
struct iommu_ops;
struct iommu_group;
struct iommu_fwspec;
struct dev_pin_info;
struct iommu_param;

struct bus_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct bus_type *bus, char *buf);
	ssize_t (*store)(struct bus_type *bus, const char *buf, size_t count);
};

#define BUS_ATTR_RW(_name) \
	struct bus_attribute bus_attr_##_name = __ATTR_RW(_name)
#define BUS_ATTR_RO(_name) \
	struct bus_attribute bus_attr_##_name = __ATTR_RO(_name)
#define BUS_ATTR_WO(_name) \
	struct bus_attribute bus_attr_##_name = __ATTR_WO(_name)

extern int __must_check bus_create_file(struct bus_type *,
					struct bus_attribute *);
extern void bus_remove_file(struct bus_type *, struct bus_attribute *);

/**
 * struct bus_type - The bus type of the device
 *
 * @name:	The name of the bus.
 * @dev_name:	Used for subsystems to enumerate devices like ("foo%u", dev->id).
 * @dev_root:	Default device to use as the parent.
 * @bus_groups:	Default attributes of the bus.
 * @dev_groups:	Default attributes of the devices on the bus.
 * @drv_groups: Default attributes of the device drivers on the bus.
 * @match:	Called, perhaps multiple times, whenever a new device or driver
 *		is added for this bus. It should return a positive value if the
 *		given device can be handled by the given driver and zero
 *		otherwise. It may also return error code if determining that
 *		the driver supports the device is not possible. In case of
 *		-EPROBE_DEFER it will queue the device for deferred probing.
 * @uevent:	Called when a device is added, removed, or a few other things
 *		that generate uevents to add the environment variables.
 * @probe:	Called when a new device or driver add to this bus, and callback
 *		the specific driver's probe to initial the matched device.
 * @remove:	Called when a device removed from this bus.
 * @shutdown:	Called at shut-down time to quiesce the device.
 *
 * @online:	Called to put the device back online (after offlining it).
 * @offline:	Called to put the device offline for hot-removal. May fail.
 *
 * @suspend:	Called when a device on this bus wants to go to sleep mode.
 * @resume:	Called to bring a device on this bus out of sleep mode.
 * @num_vf:	Called to find out how many virtual functions a device on this
 *		bus supports.
 * @dma_configure:	Called to setup DMA configuration on a device on
 *			this bus.
 * @pm:		Power management operations of this bus, callback the specific
 *		device driver's pm-ops.
 * @iommu_ops:  IOMMU specific operations for this bus, used to attach IOMMU
 *              driver implementations to a bus and allow the driver to do
 *              bus-specific setup
 * @p:		The private data of the driver core, only the driver core can
 *		touch this.
 * @lock_key:	Lock class key for use by the lock validator
 * @need_parent_lock:	When probing or removing a device on this bus, the
 *			device core should lock the device's parent.
 *
 * A bus is a channel between the processor and one or more devices. For the
 * purposes of the device model, all devices are connected via a bus, even if
 * it is an internal, virtual, "platform" bus. Buses can plug into each other.
 * A USB controller is usually a PCI device, for example. The device model
 * represents the actual connections between buses and the devices they control.
 * A bus is represented by the bus_type structure. It contains the name, the
 * default attributes, the bus' methods, PM operations, and the driver core's
 * private data.
 */
struct bus_type {
	const char		*name;
	const char		*dev_name;
	struct device		*dev_root;
	const struct attribute_group **bus_groups;
	const struct attribute_group **dev_groups;
	const struct attribute_group **drv_groups;

	int (*match)(struct device *dev, struct device_driver *drv);
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
	int (*probe)(struct device *dev);
	int (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);

	int (*online)(struct device *dev);
	int (*offline)(struct device *dev);

	int (*suspend)(struct device *dev, pm_message_t state);
	int (*resume)(struct device *dev);

	int (*num_vf)(struct device *dev);

	int (*dma_configure)(struct device *dev);

	const struct dev_pm_ops *pm;

	const struct iommu_ops *iommu_ops;

	struct subsys_private *p;
	struct lock_class_key lock_key;

	bool need_parent_lock;
};

extern int __must_check bus_register(struct bus_type *bus);

extern void bus_unregister(struct bus_type *bus);

extern int __must_check bus_rescan_devices(struct bus_type *bus);

/* iterator helpers for buses */
struct subsys_dev_iter {
	struct klist_iter		ki;
	const struct device_type	*type;
};
void subsys_dev_iter_init(struct subsys_dev_iter *iter,
			 struct bus_type *subsys,
			 struct device *start,
			 const struct device_type *type);
struct device *subsys_dev_iter_next(struct subsys_dev_iter *iter);
void subsys_dev_iter_exit(struct subsys_dev_iter *iter);

int device_match_name(struct device *dev, const void *name);
int device_match_of_node(struct device *dev, const void *np);
int device_match_fwnode(struct device *dev, const void *fwnode);
int device_match_devt(struct device *dev, const void *pdevt);
int device_match_acpi_dev(struct device *dev, const void *adev);
int device_match_any(struct device *dev, const void *unused);

int bus_for_each_dev(struct bus_type *bus, struct device *start, void *data,
		     int (*fn)(struct device *dev, void *data));
struct device *bus_find_device(struct bus_type *bus, struct device *start,
			       const void *data,
			       int (*match)(struct device *dev, const void *data));
/**
 * bus_find_device_by_name - device iterator for locating a particular device
 * of a specific name.
 * @bus: bus type
 * @start: Device to begin with
 * @name: name of the device to match
 */
static inline struct device *bus_find_device_by_name(struct bus_type *bus,
						     struct device *start,
						     const char *name)
{
	return bus_find_device(bus, start, name, device_match_name);
}

/**
 * bus_find_device_by_of_node : device iterator for locating a particular device
 * matching the of_node.
 * @bus: bus type
 * @np: of_node of the device to match.
 */
static inline struct device *
bus_find_device_by_of_node(struct bus_type *bus, const struct device_node *np)
{
	return bus_find_device(bus, NULL, np, device_match_of_node);
}

/**
 * bus_find_device_by_fwnode : device iterator for locating a particular device
 * matching the fwnode.
 * @bus: bus type
 * @fwnode: fwnode of the device to match.
 */
static inline struct device *
bus_find_device_by_fwnode(struct bus_type *bus, const struct fwnode_handle *fwnode)
{
	return bus_find_device(bus, NULL, fwnode, device_match_fwnode);
}

/**
 * bus_find_device_by_devt : device iterator for locating a particular device
 * matching the device type.
 * @bus: bus type
 * @devt: device type of the device to match.
 */
static inline struct device *bus_find_device_by_devt(struct bus_type *bus,
						     dev_t devt)
{
	return bus_find_device(bus, NULL, &devt, device_match_devt);
}

/**
 * bus_find_next_device - Find the next device after a given device in a
 * given bus.
 * @bus: bus type
 * @cur: device to begin the search with.
 */
static inline struct device *
bus_find_next_device(struct bus_type *bus,struct device *cur)
{
	return bus_find_device(bus, cur, NULL, device_match_any);
}

#ifdef CONFIG_ACPI
struct acpi_device;

/**
 * bus_find_device_by_acpi_dev : device iterator for locating a particular device
 * matching the ACPI COMPANION device.
 * @bus: bus type
 * @adev: ACPI COMPANION device to match.
 */
static inline struct device *
bus_find_device_by_acpi_dev(struct bus_type *bus, const struct acpi_device *adev)
{
	return bus_find_device(bus, NULL, adev, device_match_acpi_dev);
}
#else
static inline struct device *
bus_find_device_by_acpi_dev(struct bus_type *bus, const void *adev)
{
	return NULL;
}
#endif

struct device *subsys_find_device_by_id(struct bus_type *bus, unsigned int id,
					struct device *hint);
int bus_for_each_drv(struct bus_type *bus, struct device_driver *start,
		     void *data, int (*fn)(struct device_driver *, void *));
void bus_sort_breadthfirst(struct bus_type *bus,
			   int (*compare)(const struct device *a,
					  const struct device *b));
/*
 * Bus notifiers: Get notified of addition/removal of devices
 * and binding/unbinding of drivers to devices.
 * In the long run, it should be a replacement for the platform
 * notify hooks.
 */
struct notifier_block;

extern int bus_register_notifier(struct bus_type *bus,
				 struct notifier_block *nb);
extern int bus_unregister_notifier(struct bus_type *bus,
				   struct notifier_block *nb);

/* All 4 notifers below get called with the target struct device *
 * as an argument. Note that those functions are likely to be called
 * with the device lock held in the core, so be careful.
 */
#define BUS_NOTIFY_ADD_DEVICE		0x00000001 /* device added */
#define BUS_NOTIFY_DEL_DEVICE		0x00000002 /* device to be removed */
#define BUS_NOTIFY_REMOVED_DEVICE	0x00000003 /* device removed */
#define BUS_NOTIFY_BIND_DRIVER		0x00000004 /* driver about to be
						      bound */
#define BUS_NOTIFY_BOUND_DRIVER		0x00000005 /* driver bound to device */
#define BUS_NOTIFY_UNBIND_DRIVER	0x00000006 /* driver about to be
						      unbound */
#define BUS_NOTIFY_UNBOUND_DRIVER	0x00000007 /* driver is unbound
						      from the device */
#define BUS_NOTIFY_DRIVER_NOT_BOUND	0x00000008 /* driver fails to be bound */

extern struct kset *bus_get_kset(struct bus_type *bus);
extern struct klist *bus_get_device_klist(struct bus_type *bus);

/**
 * enum probe_type - device driver probe type to try
 *	Device drivers may opt in for special handling of their
 *	respective probe routines. This tells the core what to
 *	expect and prefer.
 *
 * @PROBE_DEFAULT_STRATEGY: Used by drivers that work equally well
 *	whether probed synchronously or asynchronously.
 * @PROBE_PREFER_ASYNCHRONOUS: Drivers for "slow" devices which
 *	probing order is not essential for booting the system may
 *	opt into executing their probes asynchronously.
 * @PROBE_FORCE_SYNCHRONOUS: Use this to annotate drivers that need
 *	their probe routines to run synchronously with driver and
 *	device registration (with the exception of -EPROBE_DEFER
 *	handling - re-probing always ends up being done asynchronously).
 *
 * Note that the end goal is to switch the kernel to use asynchronous
 * probing by default, so annotating drivers with
 * %PROBE_PREFER_ASYNCHRONOUS is a temporary measure that allows us
 * to speed up boot process while we are validating the rest of the
 * drivers.
 */
enum probe_type {
	PROBE_DEFAULT_STRATEGY,
	PROBE_PREFER_ASYNCHRONOUS,
	PROBE_FORCE_SYNCHRONOUS,
};

/**
 * struct device_driver - The basic device driver structure
 * @name:	Name of the device driver.
 * @bus:	The bus which the device of this driver belongs to.
 * @owner:	The module owner.
 * @mod_name:	Used for built-in modules.
 * @suppress_bind_attrs: Disables bind/unbind via sysfs.
 * @probe_type:	Type of the probe (synchronous or asynchronous) to use.
 * @of_match_table: The open firmware table.
 * @acpi_match_table: The ACPI match table.
 * @probe:	Called to query the existence of a specific device,
 *		whether this driver can work with it, and bind the driver
 *		to a specific device.
 * @remove:	Called when the device is removed from the system to
 *		unbind a device from this driver.
 * @shutdown:	Called at shut-down time to quiesce the device.
 * @suspend:	Called to put the device to sleep mode. Usually to a
 *		low power state.
 * @resume:	Called to bring a device from sleep mode.
 * @groups:	Default attributes that get created by the driver core
 *		automatically.
 * @dev_groups:	Additional attributes attached to device instance once the
 *		it is bound to the driver.
 * @pm:		Power management operations of the device which matched
 *		this driver.
 * @coredump:	Called when sysfs entry is written to. The device driver
 *		is expected to call the dev_coredump API resulting in a
 *		uevent.
 * @p:		Driver core's private data, no one other than the driver
 *		core can touch this.
 *
 * The device driver-model tracks all of the drivers known to the system.
 * The main reason for this tracking is to enable the driver core to match
 * up drivers with new devices. Once drivers are known objects within the
 * system, however, a number of other things become possible. Device drivers
 * can export information and configuration variables that are independent
 * of any specific device.
 */
struct device_driver {
	const char		*name;
	struct bus_type		*bus;

	struct module		*owner;
	const char		*mod_name;	/* used for built-in modules */

	bool suppress_bind_attrs;	/* disables bind/unbind via sysfs */
	enum probe_type probe_type;

	const struct of_device_id	*of_match_table;
	const struct acpi_device_id	*acpi_match_table;

	int (*probe) (struct device *dev);
	int (*remove) (struct device *dev);
	void (*shutdown) (struct device *dev);
	int (*suspend) (struct device *dev, pm_message_t state);
	int (*resume) (struct device *dev);
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;

	const struct dev_pm_ops *pm;
	void (*coredump) (struct device *dev);

	struct driver_private *p;
};


extern int __must_check driver_register(struct device_driver *drv);
extern void driver_unregister(struct device_driver *drv);

extern struct device_driver *driver_find(const char *name,
					 struct bus_type *bus);
extern int driver_probe_done(void);
extern void wait_for_device_probe(void);

/* sysfs interface for exporting driver attributes */

struct driver_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device_driver *driver, char *buf);
	ssize_t (*store)(struct device_driver *driver, const char *buf,
			 size_t count);
};

#define DRIVER_ATTR_RW(_name) \
	struct driver_attribute driver_attr_##_name = __ATTR_RW(_name)
#define DRIVER_ATTR_RO(_name) \
	struct driver_attribute driver_attr_##_name = __ATTR_RO(_name)
#define DRIVER_ATTR_WO(_name) \
	struct driver_attribute driver_attr_##_name = __ATTR_WO(_name)

extern int __must_check driver_create_file(struct device_driver *driver,
					const struct driver_attribute *attr);
extern void driver_remove_file(struct device_driver *driver,
			       const struct driver_attribute *attr);

extern int __must_check driver_for_each_device(struct device_driver *drv,
					       struct device *start,
					       void *data,
					       int (*fn)(struct device *dev,
							 void *));
struct device *driver_find_device(struct device_driver *drv,
				  struct device *start, const void *data,
				  int (*match)(struct device *dev, const void *data));

/**
 * driver_find_device_by_name - device iterator for locating a particular device
 * of a specific name.
 * @drv: the driver we're iterating
 * @name: name of the device to match
 */
static inline struct device *driver_find_device_by_name(struct device_driver *drv,
							const char *name)
{
	return driver_find_device(drv, NULL, name, device_match_name);
}

/**
 * driver_find_device_by_of_node- device iterator for locating a particular device
 * by of_node pointer.
 * @drv: the driver we're iterating
 * @np: of_node pointer to match.
 */
static inline struct device *
driver_find_device_by_of_node(struct device_driver *drv,
			      const struct device_node *np)
{
	return driver_find_device(drv, NULL, np, device_match_of_node);
}

/**
 * driver_find_device_by_fwnode- device iterator for locating a particular device
 * by fwnode pointer.
 * @drv: the driver we're iterating
 * @fwnode: fwnode pointer to match.
 */
static inline struct device *
driver_find_device_by_fwnode(struct device_driver *drv,
			     const struct fwnode_handle *fwnode)
{
	return driver_find_device(drv, NULL, fwnode, device_match_fwnode);
}

/**
 * driver_find_device_by_devt- device iterator for locating a particular device
 * by devt.
 * @drv: the driver we're iterating
 * @devt: devt pointer to match.
 */
static inline struct device *driver_find_device_by_devt(struct device_driver *drv,
							dev_t devt)
{
	return driver_find_device(drv, NULL, &devt, device_match_devt);
}

static inline struct device *driver_find_next_device(struct device_driver *drv,
						     struct device *start)
{
	return driver_find_device(drv, start, NULL, device_match_any);
}

#ifdef CONFIG_ACPI
/**
 * driver_find_device_by_acpi_dev : device iterator for locating a particular
 * device matching the ACPI_COMPANION device.
 * @drv: the driver we're iterating
 * @adev: ACPI_COMPANION device to match.
 */
static inline struct device *
driver_find_device_by_acpi_dev(struct device_driver *drv,
			       const struct acpi_device *adev)
{
	return driver_find_device(drv, NULL, adev, device_match_acpi_dev);
}
#else
static inline struct device *
driver_find_device_by_acpi_dev(struct device_driver *drv, const void *adev)
{
	return NULL;
}
#endif

void driver_deferred_probe_add(struct device *dev);
int driver_deferred_probe_check_state(struct device *dev);
int driver_deferred_probe_check_state_continue(struct device *dev);

/**
 * struct subsys_interface - interfaces to device functions
 * @name:       name of the device function
 * @subsys:     subsytem of the devices to attach to
 * @node:       the list of functions registered at the subsystem
 * @add_dev:    device hookup to device function handler
 * @remove_dev: device hookup to device function handler
 *
 * Simple interfaces attached to a subsystem. Multiple interfaces can
 * attach to a subsystem and its devices. Unlike drivers, they do not
 * exclusively claim or control devices. Interfaces usually represent
 * a specific functionality of a subsystem/class of devices.
 */
struct subsys_interface {
	const char *name;
	struct bus_type *subsys;
	struct list_head node;
	int (*add_dev)(struct device *dev, struct subsys_interface *sif);
	void (*remove_dev)(struct device *dev, struct subsys_interface *sif);
};

int subsys_interface_register(struct subsys_interface *sif);
void subsys_interface_unregister(struct subsys_interface *sif);

int subsys_system_register(struct bus_type *subsys,
			   const struct attribute_group **groups);
int subsys_virtual_register(struct bus_type *subsys,
			    const struct attribute_group **groups);

/**
 * struct class - device classes
 * @name:	Name of the class.
 * @owner:	The module owner.
 * @class_groups: Default attributes of this class.
 * @dev_groups:	Default attributes of the devices that belong to the class.
 * @dev_kobj:	The kobject that represents this class and links it into the hierarchy.
 * @dev_uevent:	Called when a device is added, removed from this class, or a
 *		few other things that generate uevents to add the environment
 *		variables.
 * @devnode:	Callback to provide the devtmpfs.
 * @class_release: Called to release this class.
 * @dev_release: Called to release the device.
 * @shutdown_pre: Called at shut-down time before driver shutdown.
 * @ns_type:	Callbacks so sysfs can detemine namespaces.
 * @namespace:	Namespace of the device belongs to this class.
 * @get_ownership: Allows class to specify uid/gid of the sysfs directories
 *		for the devices belonging to the class. Usually tied to
 *		device's namespace.
 * @pm:		The default device power management operations of this class.
 * @p:		The private data of the driver core, no one other than the
 *		driver core can touch this.
 *
 * A class is a higher-level view of a device that abstracts out low-level
 * implementation details. Drivers may see a SCSI disk or an ATA disk, but,
 * at the class level, they are all simply disks. Classes allow user space
 * to work with devices based on what they do, rather than how they are
 * connected or how they work.
 */
struct class {
	const char		*name;
	struct module		*owner;

	const struct attribute_group	**class_groups;
	const struct attribute_group	**dev_groups;
	struct kobject			*dev_kobj;

	int (*dev_uevent)(struct device *dev, struct kobj_uevent_env *env);
	char *(*devnode)(struct device *dev, umode_t *mode);

	void (*class_release)(struct class *class);
	void (*dev_release)(struct device *dev);

	int (*shutdown_pre)(struct device *dev);

	const struct kobj_ns_type_operations *ns_type;
	const void *(*namespace)(struct device *dev);

	void (*get_ownership)(struct device *dev, kuid_t *uid, kgid_t *gid);

	const struct dev_pm_ops *pm;

	struct subsys_private *p;
};

struct class_dev_iter {
	struct klist_iter		ki;
	const struct device_type	*type;
};

extern struct kobject *sysfs_dev_block_kobj;
extern struct kobject *sysfs_dev_char_kobj;
extern int __must_check __class_register(struct class *class,
					 struct lock_class_key *key);
extern void class_unregister(struct class *class);

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
#define class_register(class)			\
({						\
	static struct lock_class_key __key;	\
	__class_register(class, &__key);	\
})

struct class_compat;
struct class_compat *class_compat_register(const char *name);
void class_compat_unregister(struct class_compat *cls);
int class_compat_create_link(struct class_compat *cls, struct device *dev,
			     struct device *device_link);
void class_compat_remove_link(struct class_compat *cls, struct device *dev,
			      struct device *device_link);

extern void class_dev_iter_init(struct class_dev_iter *iter,
				struct class *class,
				struct device *start,
				const struct device_type *type);
extern struct device *class_dev_iter_next(struct class_dev_iter *iter);
extern void class_dev_iter_exit(struct class_dev_iter *iter);

extern int class_for_each_device(struct class *class, struct device *start,
				 void *data,
				 int (*fn)(struct device *dev, void *data));
extern struct device *class_find_device(struct class *class,
					struct device *start, const void *data,
					int (*match)(struct device *, const void *));

/**
 * class_find_device_by_name - device iterator for locating a particular device
 * of a specific name.
 * @class: class type
 * @name: name of the device to match
 */
static inline struct device *class_find_device_by_name(struct class *class,
						       const char *name)
{
	return class_find_device(class, NULL, name, device_match_name);
}

/**
 * class_find_device_by_of_node : device iterator for locating a particular device
 * matching the of_node.
 * @class: class type
 * @np: of_node of the device to match.
 */
static inline struct device *
class_find_device_by_of_node(struct class *class, const struct device_node *np)
{
	return class_find_device(class, NULL, np, device_match_of_node);
}

/**
 * class_find_device_by_fwnode : device iterator for locating a particular device
 * matching the fwnode.
 * @class: class type
 * @fwnode: fwnode of the device to match.
 */
static inline struct device *
class_find_device_by_fwnode(struct class *class,
			    const struct fwnode_handle *fwnode)
{
	return class_find_device(class, NULL, fwnode, device_match_fwnode);
}

/**
 * class_find_device_by_devt : device iterator for locating a particular device
 * matching the device type.
 * @class: class type
 * @devt: device type of the device to match.
 */
static inline struct device *class_find_device_by_devt(struct class *class,
						       dev_t devt)
{
	return class_find_device(class, NULL, &devt, device_match_devt);
}

#ifdef CONFIG_ACPI
struct acpi_device;
/**
 * class_find_device_by_acpi_dev : device iterator for locating a particular
 * device matching the ACPI_COMPANION device.
 * @class: class type
 * @adev: ACPI_COMPANION device to match.
 */
static inline struct device *
class_find_device_by_acpi_dev(struct class *class, const struct acpi_device *adev)
{
	return class_find_device(class, NULL, adev, device_match_acpi_dev);
}
#else
static inline struct device *
class_find_device_by_acpi_dev(struct class *class, const void *adev)
{
	return NULL;
}
#endif

struct class_attribute {
	struct attribute attr;
	ssize_t (*show)(struct class *class, struct class_attribute *attr,
			char *buf);
	ssize_t (*store)(struct class *class, struct class_attribute *attr,
			const char *buf, size_t count);
};

#define CLASS_ATTR_RW(_name) \
	struct class_attribute class_attr_##_name = __ATTR_RW(_name)
#define CLASS_ATTR_RO(_name) \
	struct class_attribute class_attr_##_name = __ATTR_RO(_name)
#define CLASS_ATTR_WO(_name) \
	struct class_attribute class_attr_##_name = __ATTR_WO(_name)

extern int __must_check class_create_file_ns(struct class *class,
					     const struct class_attribute *attr,
					     const void *ns);
extern void class_remove_file_ns(struct class *class,
				 const struct class_attribute *attr,
				 const void *ns);

static inline int __must_check class_create_file(struct class *class,
					const struct class_attribute *attr)
{
	return class_create_file_ns(class, attr, NULL);
}

static inline void class_remove_file(struct class *class,
				     const struct class_attribute *attr)
{
	return class_remove_file_ns(class, attr, NULL);
}

/* Simple class attribute that is just a static string */
struct class_attribute_string {
	struct class_attribute attr;
	char *str;
};

/* Currently read-only only */
#define _CLASS_ATTR_STRING(_name, _mode, _str) \
	{ __ATTR(_name, _mode, show_class_attr_string, NULL), _str }
#define CLASS_ATTR_STRING(_name, _mode, _str) \
	struct class_attribute_string class_attr_##_name = \
		_CLASS_ATTR_STRING(_name, _mode, _str)

extern ssize_t show_class_attr_string(struct class *class, struct class_attribute *attr,
                        char *buf);

struct class_interface {
	struct list_head	node;
	struct class		*class;

	int (*add_dev)		(struct device *, struct class_interface *);
	void (*remove_dev)	(struct device *, struct class_interface *);
};

extern int __must_check class_interface_register(struct class_interface *);
extern void class_interface_unregister(struct class_interface *);

extern struct class * __must_check __class_create(struct module *owner,
						  const char *name,
						  struct lock_class_key *key);
extern void class_destroy(struct class *cls);

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
#define class_create(owner, name)		\
({						\
	static struct lock_class_key __key;	\
	__class_create(owner, name, &__key);	\
})

/*
 * The type of device, "struct device" is embedded in. A class
 * or bus can contain devices of different types
 * like "partitions" and "disks", "mouse" and "event".
 * This identifies the device type and carries type-specific
 * information, equivalent to the kobj_type of a kobject.
 * If "name" is specified, the uevent will contain it in
 * the DEVTYPE variable.
 */
struct device_type {
	const char *name;
	const struct attribute_group **groups;
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
	char *(*devnode)(struct device *dev, umode_t *mode,
			 kuid_t *uid, kgid_t *gid);
	void (*release)(struct device *dev);

	const struct dev_pm_ops *pm;
};

/* interface for exporting device attributes */
struct device_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
			char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};

struct dev_ext_attribute {
	struct device_attribute attr;
	void *var;
};

ssize_t device_show_ulong(struct device *dev, struct device_attribute *attr,
			  char *buf);
ssize_t device_store_ulong(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count);
ssize_t device_show_int(struct device *dev, struct device_attribute *attr,
			char *buf);
ssize_t device_store_int(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
ssize_t device_show_bool(struct device *dev, struct device_attribute *attr,
			char *buf);
ssize_t device_store_bool(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);

#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_PREALLOC(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = \
		__ATTR_PREALLOC(_name, _mode, _show, _store)
#define DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_WO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define DEVICE_ULONG_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_ulong, device_store_ulong), &(_var) }
#define DEVICE_INT_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_int, device_store_int), &(_var) }
#define DEVICE_BOOL_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_bool, device_store_bool), &(_var) }
#define DEVICE_ATTR_IGNORE_LOCKDEP(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name =		\
		__ATTR_IGNORE_LOCKDEP(_name, _mode, _show, _store)

extern int device_create_file(struct device *device,
			      const struct device_attribute *entry);
extern void device_remove_file(struct device *dev,
			       const struct device_attribute *attr);
extern bool device_remove_file_self(struct device *dev,
				    const struct device_attribute *attr);
extern int __must_check device_create_bin_file(struct device *dev,
					const struct bin_attribute *attr);
extern void device_remove_bin_file(struct device *dev,
				   const struct bin_attribute *attr);

/* device resource management */
typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

#ifdef CONFIG_DEBUG_DEVRES
extern void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp,
				 int nid, const char *name) __malloc;
#define devres_alloc(release, size, gfp) \
	__devres_alloc_node(release, size, gfp, NUMA_NO_NODE, #release)
#define devres_alloc_node(release, size, gfp, nid) \
	__devres_alloc_node(release, size, gfp, nid, #release)
#else
extern void *devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp,
			       int nid) __malloc;
static inline void *devres_alloc(dr_release_t release, size_t size, gfp_t gfp)
{
	return devres_alloc_node(release, size, gfp, NUMA_NO_NODE);
}
#endif

extern void devres_for_each_res(struct device *dev, dr_release_t release,
				dr_match_t match, void *match_data,
				void (*fn)(struct device *, void *, void *),
				void *data);
extern void devres_free(void *res);
extern void devres_add(struct device *dev, void *res);
extern void *devres_find(struct device *dev, dr_release_t release,
			 dr_match_t match, void *match_data);
extern void *devres_get(struct device *dev, void *new_res,
			dr_match_t match, void *match_data);
extern void *devres_remove(struct device *dev, dr_release_t release,
			   dr_match_t match, void *match_data);
extern int devres_destroy(struct device *dev, dr_release_t release,
			  dr_match_t match, void *match_data);
extern int devres_release(struct device *dev, dr_release_t release,
			  dr_match_t match, void *match_data);

/* devres group */
extern void * __must_check devres_open_group(struct device *dev, void *id,
					     gfp_t gfp);
extern void devres_close_group(struct device *dev, void *id);
extern void devres_remove_group(struct device *dev, void *id);
extern int devres_release_group(struct device *dev, void *id);

/* managed devm_k.alloc/kfree for device drivers */
extern void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) __malloc;
extern __printf(3, 0)
char *devm_kvasprintf(struct device *dev, gfp_t gfp, const char *fmt,
		      va_list ap) __malloc;
extern __printf(3, 4)
char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...) __malloc;
static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
	return devm_kmalloc(dev, size, gfp | __GFP_ZERO);
}
static inline void *devm_kmalloc_array(struct device *dev,
				       size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;

	return devm_kmalloc(dev, bytes, flags);
}
static inline void *devm_kcalloc(struct device *dev,
				 size_t n, size_t size, gfp_t flags)
{
	return devm_kmalloc_array(dev, n, size, flags | __GFP_ZERO);
}
extern void devm_kfree(struct device *dev, const void *p);
extern char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp) __malloc;
extern const char *devm_kstrdup_const(struct device *dev,
				      const char *s, gfp_t gfp);
extern void *devm_kmemdup(struct device *dev, const void *src, size_t len,
			  gfp_t gfp);

extern unsigned long devm_get_free_pages(struct device *dev,
					 gfp_t gfp_mask, unsigned int order);
extern void devm_free_pages(struct device *dev, unsigned long addr);

void __iomem *devm_ioremap_resource(struct device *dev,
				    const struct resource *res);

void __iomem *devm_of_iomap(struct device *dev,
			    struct device_node *node, int index,
			    resource_size_t *size);

/* allows to add/remove a custom action to devres stack */
int devm_add_action(struct device *dev, void (*action)(void *), void *data);
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);
void devm_release_action(struct device *dev, void (*action)(void *), void *data);

static inline int devm_add_action_or_reset(struct device *dev,
					   void (*action)(void *), void *data)
{
	int ret;

	ret = devm_add_action(dev, action, data);
	if (ret)
		action(data);

	return ret;
}

/**
 * devm_alloc_percpu - Resource-managed alloc_percpu
 * @dev: Device to allocate per-cpu memory for
 * @type: Type to allocate per-cpu memory for
 *
 * Managed alloc_percpu. Per-cpu memory allocated with this function is
 * automatically freed on driver detach.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
#define devm_alloc_percpu(dev, type)      \
	((typeof(type) __percpu *)__devm_alloc_percpu((dev), sizeof(type), \
						      __alignof__(type)))

void __percpu *__devm_alloc_percpu(struct device *dev, size_t size,
				   size_t align);
void devm_free_percpu(struct device *dev, void __percpu *pdata);

struct device_dma_parameters {
	/*
	 * a low level driver may set these to teach IOMMU code about
	 * sg limitations.
	 */
	unsigned int max_segment_size;
	unsigned long segment_boundary_mask;
};

/**
 * struct device_connection - Device Connection Descriptor
 * @fwnode: The device node of the connected device
 * @endpoint: The names of the two devices connected together
 * @id: Unique identifier for the connection
 * @list: List head, private, for internal use only
 *
 * NOTE: @fwnode is not used together with @endpoint. @fwnode is used when
 * platform firmware defines the connection. When the connection is registered
 * with device_connection_add() @endpoint is used instead.
 */
struct device_connection {
	struct fwnode_handle	*fwnode;
	const char		*endpoint[2];
	const char		*id;
	struct list_head	list;
};

typedef void *(*devcon_match_fn_t)(struct device_connection *con, int ep,
				   void *data);

void *fwnode_connection_find_match(struct fwnode_handle *fwnode,
				   const char *con_id, void *data,
				   devcon_match_fn_t match);
void *device_connection_find_match(struct device *dev, const char *con_id,
				   void *data, devcon_match_fn_t match);

struct device *device_connection_find(struct device *dev, const char *con_id);

void device_connection_add(struct device_connection *con);
void device_connection_remove(struct device_connection *con);

/**
 * device_connections_add - Add multiple device connections at once
 * @cons: Zero terminated array of device connection descriptors
 */
static inline void device_connections_add(struct device_connection *cons)
{
	struct device_connection *c;

	for (c = cons; c->endpoint[0]; c++)
		device_connection_add(c);
}

/**
 * device_connections_remove - Remove multiple device connections at once
 * @cons: Zero terminated array of device connection descriptors
 */
static inline void device_connections_remove(struct device_connection *cons)
{
	struct device_connection *c;

	for (c = cons; c->endpoint[0]; c++)
		device_connection_remove(c);
}

/**
 * enum device_link_state - Device link states.
 * @DL_STATE_NONE: The presence of the drivers is not being tracked.
 * @DL_STATE_DORMANT: None of the supplier/consumer drivers is present.
 * @DL_STATE_AVAILABLE: The supplier driver is present, but the consumer is not.
 * @DL_STATE_CONSUMER_PROBE: The consumer is probing (supplier driver present).
 * @DL_STATE_ACTIVE: Both the supplier and consumer drivers are present.
 * @DL_STATE_SUPPLIER_UNBIND: The supplier driver is unbinding.
 */
enum device_link_state {
	DL_STATE_NONE = -1,
	DL_STATE_DORMANT = 0,
	DL_STATE_AVAILABLE,
	DL_STATE_CONSUMER_PROBE,
	DL_STATE_ACTIVE,
	DL_STATE_SUPPLIER_UNBIND,
};

/*
 * Device link flags.
 *
 * STATELESS: The core will not remove this link automatically.
 * AUTOREMOVE_CONSUMER: Remove the link automatically on consumer driver unbind.
 * PM_RUNTIME: If set, the runtime PM framework will use this link.
 * RPM_ACTIVE: Run pm_runtime_get_sync() on the supplier during link creation.
 * AUTOREMOVE_SUPPLIER: Remove the link automatically on supplier driver unbind.
 * AUTOPROBE_CONSUMER: Probe consumer driver automatically after supplier binds.
 * MANAGED: The core tracks presence of supplier/consumer drivers (internal).
 */
#define DL_FLAG_STATELESS		BIT(0)
#define DL_FLAG_AUTOREMOVE_CONSUMER	BIT(1)
#define DL_FLAG_PM_RUNTIME		BIT(2)
#define DL_FLAG_RPM_ACTIVE		BIT(3)
#define DL_FLAG_AUTOREMOVE_SUPPLIER	BIT(4)
#define DL_FLAG_AUTOPROBE_CONSUMER	BIT(5)
#define DL_FLAG_MANAGED			BIT(6)

/**
 * struct device_link - Device link representation.
 * @supplier: The device on the supplier end of the link.
 * @s_node: Hook to the supplier device's list of links to consumers.
 * @consumer: The device on the consumer end of the link.
 * @c_node: Hook to the consumer device's list of links to suppliers.
 * @status: The state of the link (with respect to the presence of drivers).
 * @flags: Link flags.
 * @rpm_active: Whether or not the consumer device is runtime-PM-active.
 * @kref: Count repeated addition of the same link.
 * @rcu_head: An RCU head to use for deferred execution of SRCU callbacks.
 * @supplier_preactivated: Supplier has been made active before consumer probe.
 */
struct device_link {
	struct device *supplier;
	struct list_head s_node;
	struct device *consumer;
	struct list_head c_node;
	enum device_link_state status;
	u32 flags;
	refcount_t rpm_active;
	struct kref kref;
#ifdef CONFIG_SRCU
	struct rcu_head rcu_head;
#endif
	bool supplier_preactivated; /* Owned by consumer probe. */
};

/**
 * enum dl_dev_state - Device driver presence tracking information.
 * @DL_DEV_NO_DRIVER: There is no driver attached to the device.
 * @DL_DEV_PROBING: A driver is probing.
 * @DL_DEV_DRIVER_BOUND: The driver has been bound to the device.
 * @DL_DEV_UNBINDING: The driver is unbinding from the device.
 */
enum dl_dev_state {
	DL_DEV_NO_DRIVER = 0,
	DL_DEV_PROBING,
	DL_DEV_DRIVER_BOUND,
	DL_DEV_UNBINDING,
};

/**
 * struct dev_links_info - Device data related to device links.
 * @suppliers: List of links to supplier devices.
 * @consumers: List of links to consumer devices.
 * @status: Driver status information.
 */
struct dev_links_info {
	struct list_head suppliers;
	struct list_head consumers;
	enum dl_dev_state status;
};

/**
 * struct device - The basic device structure
 * @parent:	The device's "parent" device, the device to which it is attached.
 * 		In most cases, a parent device is some sort of bus or host
 * 		controller. If parent is NULL, the device, is a top-level device,
 * 		which is not usually what you want.
 * @p:		Holds the private data of the driver core portions of the device.
 * 		See the comment of the struct device_private for detail.
 * @kobj:	A top-level, abstract class from which other classes are derived.
 * @init_name:	Initial name of the device.
 * @type:	The type of device.
 * 		This identifies the device type and carries type-specific
 * 		information.
 * @mutex:	Mutex to synchronize calls to its driver.
 * @lockdep_mutex: An optional debug lock that a subsystem can use as a
 * 		peer lock to gain localized lockdep coverage of the device_lock.
 * @bus:	Type of bus device is on.
 * @driver:	Which driver has allocated this
 * @platform_data: Platform data specific to the device.
 * 		Example: For devices on custom boards, as typical of embedded
 * 		and SOC based hardware, Linux often uses platform_data to point
 * 		to board-specific structures describing devices and how they
 * 		are wired.  That can include what ports are available, chip
 * 		variants, which GPIO pins act in what additional roles, and so
 * 		on.  This shrinks the "Board Support Packages" (BSPs) and
 * 		minimizes board-specific #ifdefs in drivers.
 * @driver_data: Private pointer for driver specific info.
 * @links:	Links to suppliers and consumers of this device.
 * @power:	For device power management.
 *		See Documentation/driver-api/pm/devices.rst for details.
 * @pm_domain:	Provide callbacks that are executed during system suspend,
 * 		hibernation, system resume and during runtime PM transitions
 * 		along with subsystem-level and driver-level callbacks.
 * @pins:	For device pin management.
 *		See Documentation/driver-api/pinctl.rst for details.
 * @msi_list:	Hosts MSI descriptors
 * @msi_domain: The generic MSI domain this device is using.
 * @numa_node:	NUMA node this device is close to.
 * @dma_ops:    DMA mapping operations for this device.
 * @dma_mask:	Dma mask (if dma'ble device).
 * @coherent_dma_mask: Like dma_mask, but for alloc_coherent mapping as not all
 * 		hardware supports 64-bit addresses for consistent allocations
 * 		such descriptors.
 * @bus_dma_mask: Mask of an upstream bridge or bus which imposes a smaller DMA
 *		limit than the device itself supports.
 * @dma_pfn_offset: offset of DMA memory range relatively of RAM
 * @dma_parms:	A low level driver may set these to teach IOMMU code about
 * 		segment limitations.
 * @dma_pools:	Dma pools (if dma'ble device).
 * @dma_mem:	Internal for coherent mem override.
 * @cma_area:	Contiguous memory area for dma allocations
 * @archdata:	For arch-specific additions.
 * @of_node:	Associated device tree node.
 * @fwnode:	Associated device node supplied by platform firmware.
 * @devt:	For creating the sysfs "dev".
 * @id:		device instance
 * @devres_lock: Spinlock to protect the resource of the device.
 * @devres_head: The resources list of the device.
 * @knode_class: The node used to add the device to the class list.
 * @class:	The class of the device.
 * @groups:	Optional attribute groups.
 * @release:	Callback to free the device after all references have
 * 		gone away. This should be set by the allocator of the
 * 		device (i.e. the bus driver that discovered the device).
 * @iommu_group: IOMMU group the device belongs to.
 * @iommu_fwspec: IOMMU-specific properties supplied by firmware.
 * @iommu_param: Per device generic IOMMU runtime data
 *
 * @offline_disabled: If set, the device is permanently online.
 * @offline:	Set after successful invocation of bus type's .offline().
 * @of_node_reused: Set if the device-tree node is shared with an ancestor
 *              device.
 * @dma_coherent: this particular device is dma coherent, even if the
 *		architecture supports non-coherent devices.
 *
 * At the lowest level, every device in a Linux system is represented by an
 * instance of struct device. The device structure contains the information
 * that the device model core needs to model the system. Most subsystems,
 * however, track additional information about the devices they host. As a
 * result, it is rare for devices to be represented by bare device structures;
 * instead, that structure, like kobject structures, is usually embedded within
 * a higher-level representation of the device.
 */
struct device {
	struct kobject kobj;
	struct device		*parent;

	struct device_private	*p;

	const char		*init_name; /* initial name of the device */
	const struct device_type *type;

	struct bus_type	*bus;		/* type of bus device is on */
	struct device_driver *driver;	/* which driver has allocated this
					   device */
	void		*platform_data;	/* Platform specific data, device
					   core doesn't touch it */
	void		*driver_data;	/* Driver data, set and get with
					   dev_set_drvdata/dev_get_drvdata */
#ifdef CONFIG_PROVE_LOCKING
	struct mutex		lockdep_mutex;
#endif
	struct mutex		mutex;	/* mutex to synchronize calls to
					 * its driver.
					 */

	struct dev_links_info	links;
	struct dev_pm_info	power;
	struct dev_pm_domain	*pm_domain;

#ifdef CONFIG_GENERIC_MSI_IRQ_DOMAIN
	struct irq_domain	*msi_domain;
#endif
#ifdef CONFIG_PINCTRL
	struct dev_pin_info	*pins;
#endif
#ifdef CONFIG_GENERIC_MSI_IRQ
	struct list_head	msi_list;
#endif

	const struct dma_map_ops *dma_ops;
	u64		*dma_mask;	/* dma mask (if dma'able device) */
	u64		coherent_dma_mask;/* Like dma_mask, but for
					     alloc_coherent mappings as
					     not all hardware supports
					     64 bit addresses for consistent
					     allocations such descriptors. */
	u64		bus_dma_mask;	/* upstream dma_mask constraint */
	unsigned long	dma_pfn_offset;

	struct device_dma_parameters *dma_parms;

	struct list_head	dma_pools;	/* dma pools (if dma'ble) */

#ifdef CONFIG_DMA_DECLARE_COHERENT
	struct dma_coherent_mem	*dma_mem; /* internal for coherent mem
					     override */
#endif
#ifdef CONFIG_DMA_CMA
	struct cma *cma_area;		/* contiguous memory area for dma
					   allocations */
#endif
	/* arch specific additions */
	struct dev_archdata	archdata;

	struct device_node	*of_node; /* associated device tree node */
	struct fwnode_handle	*fwnode; /* firmware device node */

#ifdef CONFIG_NUMA
	int		numa_node;	/* NUMA node this device is close to */
#endif
	dev_t			devt;	/* dev_t, creates the sysfs "dev" */
	u32			id;	/* device instance */

	spinlock_t		devres_lock;
	struct list_head	devres_head;

	struct class		*class;
	const struct attribute_group **groups;	/* optional groups */

	void	(*release)(struct device *dev);
	struct iommu_group	*iommu_group;
	struct iommu_fwspec	*iommu_fwspec;
	struct iommu_param	*iommu_param;

	bool			offline_disabled:1;
	bool			offline:1;
	bool			of_node_reused:1;
#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
	bool			dma_coherent:1;
#endif
};

static inline struct device *kobj_to_dev(struct kobject *kobj)
{
	return container_of(kobj, struct device, kobj);
}

/**
 * device_iommu_mapped - Returns true when the device DMA is translated
 *			 by an IOMMU
 * @dev: Device to perform the check on
 */
static inline bool device_iommu_mapped(struct device *dev)
{
	return (dev->iommu_group != NULL);
}

/* Get the wakeup routines, which depend on struct device */
#include <linux/pm_wakeup.h>

static inline const char *dev_name(const struct device *dev)
{
	/* Use the init name until the kobject becomes available */
	if (dev->init_name)
		return dev->init_name;

	return kobject_name(&dev->kobj);
}

extern __printf(2, 3)
int dev_set_name(struct device *dev, const char *name, ...);

#ifdef CONFIG_NUMA
static inline int dev_to_node(struct device *dev)
{
	return dev->numa_node;
}
static inline void set_dev_node(struct device *dev, int node)
{
	dev->numa_node = node;
}
#else
static inline int dev_to_node(struct device *dev)
{
	return NUMA_NO_NODE;
}
static inline void set_dev_node(struct device *dev, int node)
{
}
#endif

static inline struct irq_domain *dev_get_msi_domain(const struct device *dev)
{
#ifdef CONFIG_GENERIC_MSI_IRQ_DOMAIN
	return dev->msi_domain;
#else
	return NULL;
#endif
}

static inline void dev_set_msi_domain(struct device *dev, struct irq_domain *d)
{
#ifdef CONFIG_GENERIC_MSI_IRQ_DOMAIN
	dev->msi_domain = d;
#endif
}

static inline void *dev_get_drvdata(const struct device *dev)
{
	return dev->driver_data;
}

static inline void dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}

static inline struct pm_subsys_data *dev_to_psd(struct device *dev)
{
	return dev ? dev->power.subsys_data : NULL;
}

static inline unsigned int dev_get_uevent_suppress(const struct device *dev)
{
	return dev->kobj.uevent_suppress;
}

static inline void dev_set_uevent_suppress(struct device *dev, int val)
{
	dev->kobj.uevent_suppress = val;
}

static inline int device_is_registered(struct device *dev)
{
	return dev->kobj.state_in_sysfs;
}

static inline void device_enable_async_suspend(struct device *dev)
{
	if (!dev->power.is_prepared)
		dev->power.async_suspend = true;
}

static inline void device_disable_async_suspend(struct device *dev)
{
	if (!dev->power.is_prepared)
		dev->power.async_suspend = false;
}

static inline bool device_async_suspend_enabled(struct device *dev)
{
	return !!dev->power.async_suspend;
}

static inline bool device_pm_not_required(struct device *dev)
{
	return dev->power.no_pm;
}

static inline void device_set_pm_not_required(struct device *dev)
{
	dev->power.no_pm = true;
}

static inline void dev_pm_syscore_device(struct device *dev, bool val)
{
#ifdef CONFIG_PM_SLEEP
	dev->power.syscore = val;
#endif
}

static inline void dev_pm_set_driver_flags(struct device *dev, u32 flags)
{
	dev->power.driver_flags = flags;
}

static inline bool dev_pm_test_driver_flags(struct device *dev, u32 flags)
{
	return !!(dev->power.driver_flags & flags);
}

static inline void device_lock(struct device *dev)
{
	mutex_lock(&dev->mutex);
}

static inline int device_lock_interruptible(struct device *dev)
{
	return mutex_lock_interruptible(&dev->mutex);
}

static inline int device_trylock(struct device *dev)
{
	return mutex_trylock(&dev->mutex);
}

static inline void device_unlock(struct device *dev)
{
	mutex_unlock(&dev->mutex);
}

static inline void device_lock_assert(struct device *dev)
{
	lockdep_assert_held(&dev->mutex);
}

static inline struct device_node *dev_of_node(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_OF) || !dev)
		return NULL;
	return dev->of_node;
}

void driver_init(void);

/*
 * High level routines for use by the bus drivers
 */
extern int __must_check device_register(struct device *dev);
extern void device_unregister(struct device *dev);
extern void device_initialize(struct device *dev);
extern int __must_check device_add(struct device *dev);
extern void device_del(struct device *dev);
extern int device_for_each_child(struct device *dev, void *data,
		     int (*fn)(struct device *dev, void *data));
extern int device_for_each_child_reverse(struct device *dev, void *data,
		     int (*fn)(struct device *dev, void *data));
extern struct device *device_find_child(struct device *dev, void *data,
				int (*match)(struct device *dev, void *data));
extern struct device *device_find_child_by_name(struct device *parent,
						const char *name);
extern int device_rename(struct device *dev, const char *new_name);
extern int device_move(struct device *dev, struct device *new_parent,
		       enum dpm_order dpm_order);
extern const char *device_get_devnode(struct device *dev,
				      umode_t *mode, kuid_t *uid, kgid_t *gid,
				      const char **tmp);

static inline bool device_supports_offline(struct device *dev)
{
	return dev->bus && dev->bus->offline && dev->bus->online;
}

extern void lock_device_hotplug(void);
extern void unlock_device_hotplug(void);
extern int lock_device_hotplug_sysfs(void);
extern int device_offline(struct device *dev);
extern int device_online(struct device *dev);
extern void set_primary_fwnode(struct device *dev, struct fwnode_handle *fwnode);
extern void set_secondary_fwnode(struct device *dev, struct fwnode_handle *fwnode);
void device_set_of_node_from_dev(struct device *dev, const struct device *dev2);

static inline int dev_num_vf(struct device *dev)
{
	if (dev->bus && dev->bus->num_vf)
		return dev->bus->num_vf(dev);
	return 0;
}

/*
 * Root device objects for grouping under /sys/devices
 */
extern struct device *__root_device_register(const char *name,
					     struct module *owner);

/* This is a macro to avoid include problems with THIS_MODULE */
#define root_device_register(name) \
	__root_device_register(name, THIS_MODULE)

extern void root_device_unregister(struct device *root);

static inline void *dev_get_platdata(const struct device *dev)
{
	return dev->platform_data;
}

/*
 * Manual binding of a device to driver. See drivers/base/bus.c
 * for information on use.
 */
extern int __must_check device_bind_driver(struct device *dev);
extern void device_release_driver(struct device *dev);
extern int  __must_check device_attach(struct device *dev);
extern int __must_check driver_attach(struct device_driver *drv);
extern void device_initial_probe(struct device *dev);
extern int __must_check device_reprobe(struct device *dev);

extern bool device_is_bound(struct device *dev);

/*
 * Easy functions for dynamically creating devices on the fly
 */
extern __printf(5, 0)
struct device *device_create_vargs(struct class *cls, struct device *parent,
				   dev_t devt, void *drvdata,
				   const char *fmt, va_list vargs);
extern __printf(5, 6)
struct device *device_create(struct class *cls, struct device *parent,
			     dev_t devt, void *drvdata,
			     const char *fmt, ...);
extern __printf(6, 7)
struct device *device_create_with_groups(struct class *cls,
			     struct device *parent, dev_t devt, void *drvdata,
			     const struct attribute_group **groups,
			     const char *fmt, ...);
extern void device_destroy(struct class *cls, dev_t devt);

extern int __must_check device_add_groups(struct device *dev,
					const struct attribute_group **groups);
extern void device_remove_groups(struct device *dev,
				 const struct attribute_group **groups);

static inline int __must_check device_add_group(struct device *dev,
					const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };

	return device_add_groups(dev, groups);
}

static inline void device_remove_group(struct device *dev,
				       const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };

	return device_remove_groups(dev, groups);
}

extern int __must_check devm_device_add_groups(struct device *dev,
					const struct attribute_group **groups);
extern void devm_device_remove_groups(struct device *dev,
				      const struct attribute_group **groups);
extern int __must_check devm_device_add_group(struct device *dev,
					const struct attribute_group *grp);
extern void devm_device_remove_group(struct device *dev,
				     const struct attribute_group *grp);

/*
 * Platform "fixup" functions - allow the platform to have their say
 * about devices and actions that the general device layer doesn't
 * know about.
 */
/* Notify platform of device discovery */
extern int (*platform_notify)(struct device *dev);

extern int (*platform_notify_remove)(struct device *dev);


/*
 * get_device - atomically increment the reference count for the device.
 *
 */
extern struct device *get_device(struct device *dev);
extern void put_device(struct device *dev);
extern bool kill_device(struct device *dev);

#ifdef CONFIG_DEVTMPFS
extern int devtmpfs_create_node(struct device *dev);
extern int devtmpfs_delete_node(struct device *dev);
extern int devtmpfs_mount(const char *mntdir);
#else
static inline int devtmpfs_create_node(struct device *dev) { return 0; }
static inline int devtmpfs_delete_node(struct device *dev) { return 0; }
static inline int devtmpfs_mount(const char *mountpoint) { return 0; }
#endif

/* drivers/base/power/shutdown.c */
extern void device_shutdown(void);

/* debugging and troubleshooting/diagnostic helpers. */
extern const char *dev_driver_string(const struct device *dev);

/* Device links interface. */
struct device_link *device_link_add(struct device *consumer,
				    struct device *supplier, u32 flags);
void device_link_del(struct device_link *link);
void device_link_remove(void *consumer, struct device *supplier);

#ifndef dev_fmt
#define dev_fmt(fmt) fmt
#endif

#ifdef CONFIG_PRINTK

__printf(3, 0) __cold
int dev_vprintk_emit(int level, const struct device *dev,
		     const char *fmt, va_list args);
__printf(3, 4) __cold
int dev_printk_emit(int level, const struct device *dev, const char *fmt, ...);

__printf(3, 4) __cold
void dev_printk(const char *level, const struct device *dev,
		const char *fmt, ...);
__printf(2, 3) __cold
void _dev_emerg(const struct device *dev, const char *fmt, ...);
__printf(2, 3) __cold
void _dev_alert(const struct device *dev, const char *fmt, ...);
__printf(2, 3) __cold
void _dev_crit(const struct device *dev, const char *fmt, ...);
__printf(2, 3) __cold
void _dev_err(const struct device *dev, const char *fmt, ...);
__printf(2, 3) __cold
void _dev_warn(const struct device *dev, const char *fmt, ...);
__printf(2, 3) __cold
void _dev_notice(const struct device *dev, const char *fmt, ...);
__printf(2, 3) __cold
void _dev_info(const struct device *dev, const char *fmt, ...);

#else

static inline __printf(3, 0)
int dev_vprintk_emit(int level, const struct device *dev,
		     const char *fmt, va_list args)
{ return 0; }
static inline __printf(3, 4)
int dev_printk_emit(int level, const struct device *dev, const char *fmt, ...)
{ return 0; }

static inline void __dev_printk(const char *level, const struct device *dev,
				struct va_format *vaf)
{}
static inline __printf(3, 4)
void dev_printk(const char *level, const struct device *dev,
		 const char *fmt, ...)
{}

static inline __printf(2, 3)
void _dev_emerg(const struct device *dev, const char *fmt, ...)
{}
static inline __printf(2, 3)
void _dev_crit(const struct device *dev, const char *fmt, ...)
{}
static inline __printf(2, 3)
void _dev_alert(const struct device *dev, const char *fmt, ...)
{}
static inline __printf(2, 3)
void _dev_err(const struct device *dev, const char *fmt, ...)
{}
static inline __printf(2, 3)
void _dev_warn(const struct device *dev, const char *fmt, ...)
{}
static inline __printf(2, 3)
void _dev_notice(const struct device *dev, const char *fmt, ...)
{}
static inline __printf(2, 3)
void _dev_info(const struct device *dev, const char *fmt, ...)
{}

#endif

/*
 * #defines for all the dev_<level> macros to prefix with whatever
 * possible use of #define dev_fmt(fmt) ...
 */

#define dev_emerg(dev, fmt, ...)					\
	_dev_emerg(dev, dev_fmt(fmt), ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)						\
	_dev_crit(dev, dev_fmt(fmt), ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...)					\
	_dev_alert(dev, dev_fmt(fmt), ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)						\
	_dev_err(dev, dev_fmt(fmt), ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)						\
	_dev_warn(dev, dev_fmt(fmt), ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...)					\
	_dev_notice(dev, dev_fmt(fmt), ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)						\
	_dev_info(dev, dev_fmt(fmt), ##__VA_ARGS__)

#if defined(CONFIG_DYNAMIC_DEBUG)
#define dev_dbg(dev, fmt, ...)						\
	dynamic_dev_dbg(dev, dev_fmt(fmt), ##__VA_ARGS__)
#elif defined(DEBUG)
#define dev_dbg(dev, fmt, ...)						\
	dev_printk(KERN_DEBUG, dev, dev_fmt(fmt), ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...)						\
({									\
	if (0)								\
		dev_printk(KERN_DEBUG, dev, dev_fmt(fmt), ##__VA_ARGS__); \
})
#endif

#ifdef CONFIG_PRINTK
#define dev_level_once(dev_level, dev, fmt, ...)			\
do {									\
	static bool __print_once __read_mostly;				\
									\
	if (!__print_once) {						\
		__print_once = true;					\
		dev_level(dev, fmt, ##__VA_ARGS__);			\
	}								\
} while (0)
#else
#define dev_level_once(dev_level, dev, fmt, ...)			\
do {									\
	if (0)								\
		dev_level(dev, fmt, ##__VA_ARGS__);			\
} while (0)
#endif

#define dev_emerg_once(dev, fmt, ...)					\
	dev_level_once(dev_emerg, dev, fmt, ##__VA_ARGS__)
#define dev_alert_once(dev, fmt, ...)					\
	dev_level_once(dev_alert, dev, fmt, ##__VA_ARGS__)
#define dev_crit_once(dev, fmt, ...)					\
	dev_level_once(dev_crit, dev, fmt, ##__VA_ARGS__)
#define dev_err_once(dev, fmt, ...)					\
	dev_level_once(dev_err, dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...)					\
	dev_level_once(dev_warn, dev, fmt, ##__VA_ARGS__)
#define dev_notice_once(dev, fmt, ...)					\
	dev_level_once(dev_notice, dev, fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...)					\
	dev_level_once(dev_info, dev, fmt, ##__VA_ARGS__)
#define dev_dbg_once(dev, fmt, ...)					\
	dev_level_once(dev_dbg, dev, fmt, ##__VA_ARGS__)

#define dev_level_ratelimited(dev_level, dev, fmt, ...)			\
do {									\
	static DEFINE_RATELIMIT_STATE(_rs,				\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
	if (__ratelimit(&_rs))						\
		dev_level(dev, fmt, ##__VA_ARGS__);			\
} while (0)

#define dev_emerg_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_emerg, dev, fmt, ##__VA_ARGS__)
#define dev_alert_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_alert, dev, fmt, ##__VA_ARGS__)
#define dev_crit_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_crit, dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_err, dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_warn, dev, fmt, ##__VA_ARGS__)
#define dev_notice_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_notice, dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_info, dev, fmt, ##__VA_ARGS__)
#if defined(CONFIG_DYNAMIC_DEBUG)
/* descriptor check is first to prevent flooding with "callbacks suppressed" */
#define dev_dbg_ratelimited(dev, fmt, ...)				\
do {									\
	static DEFINE_RATELIMIT_STATE(_rs,				\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
	DEFINE_DYNAMIC_DEBUG_METADATA(descriptor, fmt);			\
	if (DYNAMIC_DEBUG_BRANCH(descriptor) &&				\
	    __ratelimit(&_rs))						\
		__dynamic_dev_dbg(&descriptor, dev, dev_fmt(fmt),	\
				  ##__VA_ARGS__);			\
} while (0)
#elif defined(DEBUG)
#define dev_dbg_ratelimited(dev, fmt, ...)				\
do {									\
	static DEFINE_RATELIMIT_STATE(_rs,				\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
	if (__ratelimit(&_rs))						\
		dev_printk(KERN_DEBUG, dev, dev_fmt(fmt), ##__VA_ARGS__); \
} while (0)
#else
#define dev_dbg_ratelimited(dev, fmt, ...)				\
do {									\
	if (0)								\
		dev_printk(KERN_DEBUG, dev, dev_fmt(fmt), ##__VA_ARGS__); \
} while (0)
#endif

#ifdef VERBOSE_DEBUG
#define dev_vdbg	dev_dbg
#else
#define dev_vdbg(dev, fmt, ...)						\
({									\
	if (0)								\
		dev_printk(KERN_DEBUG, dev, dev_fmt(fmt), ##__VA_ARGS__); \
})
#endif

/*
 * dev_WARN*() acts like dev_printk(), but with the key difference of
 * using WARN/WARN_ONCE to include file/line information and a backtrace.
 */
#define dev_WARN(dev, format, arg...) \
	WARN(1, "%s %s: " format, dev_driver_string(dev), dev_name(dev), ## arg);

#define dev_WARN_ONCE(dev, condition, format, arg...) \
	WARN_ONCE(condition, "%s %s: " format, \
			dev_driver_string(dev), dev_name(dev), ## arg)

/* Create alias, so I can be autoloaded. */
#define MODULE_ALIAS_CHARDEV(major,minor) \
	MODULE_ALIAS("char-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_CHARDEV_MAJOR(major) \
	MODULE_ALIAS("char-major-" __stringify(major) "-*")

#ifdef CONFIG_SYSFS_DEPRECATED
extern long sysfs_deprecated;
#else
#define sysfs_deprecated 0
#endif

/**
 * module_driver() - Helper macro for drivers that don't do anything
 * special in module init/exit. This eliminates a lot of boilerplate.
 * Each module may only use this macro once, and calling it replaces
 * module_init() and module_exit().
 *
 * @__driver: driver name
 * @__register: register function for this driver type
 * @__unregister: unregister function for this driver type
 * @...: Additional arguments to be passed to __register and __unregister.
 *
 * Use this macro to construct bus specific macros for registering
 * drivers, and do not use it on its own.
 */
#define module_driver(__driver, __register, __unregister, ...) \
static int __init __driver##_init(void) \
{ \
	return __register(&(__driver) , ##__VA_ARGS__); \
} \
module_init(__driver##_init); \
static void __exit __driver##_exit(void) \
{ \
	__unregister(&(__driver) , ##__VA_ARGS__); \
} \
module_exit(__driver##_exit);

/**
 * builtin_driver() - Helper macro for drivers that don't do anything
 * special in init and have no exit. This eliminates some boilerplate.
 * Each driver may only use this macro once, and calling it replaces
 * device_initcall (or in some cases, the legacy __initcall).  This is
 * meant to be a direct parallel of module_driver() above but without
 * the __exit stuff that is not used for builtin cases.
 *
 * @__driver: driver name
 * @__register: register function for this driver type
 * @...: Additional arguments to be passed to __register
 *
 * Use this macro to construct bus specific macros for registering
 * drivers, and do not use it on its own.
 */
#define builtin_driver(__driver, __register, ...) \
static int __init __driver##_init(void) \
{ \
	return __register(&(__driver) , ##__VA_ARGS__); \
} \
device_initcall(__driver##_init);

#endif /* _DEVICE_H_ */
