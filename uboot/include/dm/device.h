/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2013 Google, Inc
 *
 * (C) Copyright 2012
 * Pavel Herrmann <morpheus.ibis@gmail.com>
 * Marek Vasut <marex@denx.de>
 */

#ifndef _DM_DEVICE_H
#define _DM_DEVICE_H

#include <dm/ofnode.h>
#include <dm/uclass-id.h>
#include <fdtdec.h>
#include <linker_lists.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>

struct driver_info;

/* Driver is active (probed). Cleared when it is removed */
#define DM_FLAG_ACTIVATED		(1 << 0)

/* DM is responsible for allocating and freeing platdata */
#define DM_FLAG_ALLOC_PDATA		(1 << 1)

/* DM should init this device prior to relocation */
#define DM_FLAG_PRE_RELOC		(1 << 2)

/* DM is responsible for allocating and freeing parent_platdata */
#define DM_FLAG_ALLOC_PARENT_PDATA	(1 << 3)

/* DM is responsible for allocating and freeing uclass_platdata */
#define DM_FLAG_ALLOC_UCLASS_PDATA	(1 << 4)

/* Allocate driver private data on a DMA boundary */
#define DM_FLAG_ALLOC_PRIV_DMA		(1 << 5)

/* Device is bound */
#define DM_FLAG_BOUND			(1 << 6)

/* Device name is allocated and should be freed on unbind() */
#define DM_FLAG_NAME_ALLOCED		(1 << 7)

/* Device has platform data provided by of-platdata */
#define DM_FLAG_OF_PLATDATA		(1 << 8)

/*
 * Call driver remove function to stop currently active DMA transfers or
 * give DMA buffers back to the HW / controller. This may be needed for
 * some drivers to do some final stage cleanup before the OS is called
 * (U-Boot exit)
 */
#define DM_FLAG_ACTIVE_DMA		(1 << 9)

/*
 * Call driver remove function to do some final configuration, before
 * U-Boot exits and the OS is started
 */
#define DM_FLAG_OS_PREPARE		(1 << 10)

/* DM does not enable/disable the power domains corresponding to this device */
#define DM_FLAG_DEFAULT_PD_CTRL_OFF	(1 << 11)

/* Driver platdata has been read. Cleared when the device is removed */
#define DM_FLAG_PLATDATA_VALID		(1 << 12)

/*
 * Device is removed without switching off its power domain. This might
 * be required, i. e. for serial console (debug) output when booting OS.
 */
#define DM_FLAG_REMOVE_WITH_PD_ON	(1 << 13)

/*
 * One or multiple of these flags are passed to device_remove() so that
 * a selective device removal as specified by the remove-stage and the
 * driver flags can be done.
 */
enum {
	/* Normal remove, remove all devices */
	DM_REMOVE_NORMAL	= 1 << 0,

	/* Remove devices with active DMA */
	DM_REMOVE_ACTIVE_DMA	= DM_FLAG_ACTIVE_DMA,

	/* Remove devices which need some final OS preparation steps */
	DM_REMOVE_OS_PREPARE	= DM_FLAG_OS_PREPARE,

	/* Add more use cases here */

	/* Remove devices with any active flag */
	DM_REMOVE_ACTIVE_ALL	= DM_REMOVE_ACTIVE_DMA | DM_REMOVE_OS_PREPARE,

	/* Don't power down any attached power domains */
	DM_REMOVE_NO_PD		= 1 << 1,
};

/**
 * struct udevice - An instance of a driver
 *
 * This holds information about a device, which is a driver bound to a
 * particular port or peripheral (essentially a driver instance).
 *
 * A device will come into existence through a 'bind' call, either due to
 * a U_BOOT_DEVICE() macro (in which case platdata is non-NULL) or a node
 * in the device tree (in which case of_offset is >= 0). In the latter case
 * we translate the device tree information into platdata in a function
 * implemented by the driver ofdata_to_platdata method (called just before the
 * probe method if the device has a device tree node.
 *
 * All three of platdata, priv and uclass_priv can be allocated by the
 * driver, or you can use the auto_alloc_size members of struct driver and
 * struct uclass_driver to have driver model do this automatically.
 *
 * @driver: The driver used by this device
 * @name: Name of device, typically the FDT node name
 * @platdata: Configuration data for this device
 * @parent_platdata: The parent bus's configuration data for this device
 * @uclass_platdata: The uclass's configuration data for this device
 * @node: Reference to device tree node for this device
 * @driver_data: Driver data word for the entry that matched this device with
 *		its driver
 * @parent: Parent of this device, or NULL for the top level device
 * @priv: Private data for this device
 * @uclass: Pointer to uclass for this device
 * @uclass_priv: The uclass's private data for this device
 * @parent_priv: The parent's private data for this device
 * @uclass_node: Used by uclass to link its devices
 * @child_head: List of children of this device
 * @sibling_node: Next device in list of all devices
 * @flags: Flags for this device DM_FLAG_...
 * @req_seq: Requested sequence number for this device (-1 = any)
 * @seq: Allocated sequence number for this device (-1 = none). This is set up
 * when the device is probed and will be unique within the device's uclass.
 * @devres_head: List of memory allocations associated with this device.
 *		When CONFIG_DEVRES is enabled, devm_kmalloc() and friends will
 *		add to this list. Memory so-allocated will be freed
 *		automatically when the device is removed / unbound
 */
struct udevice {
	const struct driver *driver;
	const char *name;
	void *platdata;
	void *parent_platdata;
	void *uclass_platdata;
	ofnode node;
	ulong driver_data;
	struct udevice *parent;
	void *priv;
	struct uclass *uclass;
	void *uclass_priv;
	void *parent_priv;
	struct list_head uclass_node;
	struct list_head child_head;
	struct list_head sibling_node;
	uint32_t flags;
	int req_seq;
	int seq;
#ifdef CONFIG_DEVRES
	struct list_head devres_head;
#endif
};

/* Maximum sequence number supported */
#define DM_MAX_SEQ	999

/* Returns the operations for a device */
#define device_get_ops(dev)	(dev->driver->ops)

/* Returns non-zero if the device is active (probed and not removed) */
#define device_active(dev)	((dev)->flags & DM_FLAG_ACTIVATED)

static inline int dev_of_offset(const struct udevice *dev)
{
	return ofnode_to_offset(dev->node);
}

static inline void dev_set_of_offset(struct udevice *dev, int of_offset)
{
	dev->node = offset_to_ofnode(of_offset);
}

static inline bool dev_has_of_node(struct udevice *dev)
{
	return ofnode_valid(dev->node);
}

/**
 * struct udevice_id - Lists the compatible strings supported by a driver
 * @compatible: Compatible string
 * @data: Data for this compatible string
 */
struct udevice_id {
	const char *compatible;
	ulong data;
};

#if CONFIG_IS_ENABLED(OF_CONTROL)
#define of_match_ptr(_ptr)	(_ptr)
#else
#define of_match_ptr(_ptr)	NULL
#endif /* CONFIG_IS_ENABLED(OF_CONTROL) */

/**
 * struct driver - A driver for a feature or peripheral
 *
 * This holds methods for setting up a new device, and also removing it.
 * The device needs information to set itself up - this is provided either
 * by platdata or a device tree node (which we find by looking up
 * matching compatible strings with of_match).
 *
 * Drivers all belong to a uclass, representing a class of devices of the
 * same type. Common elements of the drivers can be implemented in the uclass,
 * or the uclass can provide a consistent interface to the drivers within
 * it.
 *
 * @name: Device name
 * @id: Identifies the uclass we belong to
 * @of_match: List of compatible strings to match, and any identifying data
 * for each.
 * @bind: Called to bind a device to its driver
 * @probe: Called to probe a device, i.e. activate it
 * @remove: Called to remove a device, i.e. de-activate it
 * @unbind: Called to unbind a device from its driver
 * @ofdata_to_platdata: Called before probe to decode device tree data
 * @child_post_bind: Called after a new child has been bound
 * @child_pre_probe: Called before a child device is probed. The device has
 * memory allocated but it has not yet been probed.
 * @child_post_remove: Called after a child device is removed. The device
 * has memory allocated but its device_remove() method has been called.
 * @priv_auto_alloc_size: If non-zero this is the size of the private data
 * to be allocated in the device's ->priv pointer. If zero, then the driver
 * is responsible for allocating any data required.
 * @platdata_auto_alloc_size: If non-zero this is the size of the
 * platform data to be allocated in the device's ->platdata pointer.
 * This is typically only useful for device-tree-aware drivers (those with
 * an of_match), since drivers which use platdata will have the data
 * provided in the U_BOOT_DEVICE() instantiation.
 * @per_child_auto_alloc_size: Each device can hold private data owned by
 * its parent. If required this will be automatically allocated if this
 * value is non-zero.
 * @per_child_platdata_auto_alloc_size: A bus likes to store information about
 * its children. If non-zero this is the size of this data, to be allocated
 * in the child's parent_platdata pointer.
 * @ops: Driver-specific operations. This is typically a list of function
 * pointers defined by the driver, to implement driver functions required by
 * the uclass.
 * @flags: driver flags - see DM_FLAGS_...
 * @acpi_ops: Advanced Configuration and Power Interface (ACPI) operations,
 * allowing the device to add things to the ACPI tables passed to Linux
 */
struct driver {
	char *name;
	enum uclass_id id;
	const struct udevice_id *of_match;
	int (*bind)(struct udevice *dev);
	int (*probe)(struct udevice *dev);
	int (*remove)(struct udevice *dev);
	int (*unbind)(struct udevice *dev);
	int (*ofdata_to_platdata)(struct udevice *dev);
	int (*child_post_bind)(struct udevice *dev);
	int (*child_pre_probe)(struct udevice *dev);
	int (*child_post_remove)(struct udevice *dev);
	int priv_auto_alloc_size;
	int platdata_auto_alloc_size;
	int per_child_auto_alloc_size;
	int per_child_platdata_auto_alloc_size;
	const void *ops;	/* driver-specific operations */
	uint32_t flags;
#if CONFIG_IS_ENABLED(ACPIGEN)
	struct acpi_ops *acpi_ops;
#endif
};

/* Declare a new U-Boot driver */
#define U_BOOT_DRIVER(__name)						\
	ll_entry_declare(struct driver, __name, driver)

/* Get a pointer to a given driver */
#define DM_GET_DRIVER(__name)						\
	ll_entry_get(struct driver, __name, driver)

/**
 * dev_get_platdata() - Get the platform data for a device
 *
 * This checks that dev is not NULL, but no other checks for now
 *
 * @dev		Device to check
 * @return platform data, or NULL if none
 */
void *dev_get_platdata(const struct udevice *dev);

/**
 * dev_get_parent_platdata() - Get the parent platform data for a device
 *
 * This checks that dev is not NULL, but no other checks for now
 *
 * @dev		Device to check
 * @return parent's platform data, or NULL if none
 */
void *dev_get_parent_platdata(const struct udevice *dev);

/**
 * dev_get_uclass_platdata() - Get the uclass platform data for a device
 *
 * This checks that dev is not NULL, but no other checks for now
 *
 * @dev		Device to check
 * @return uclass's platform data, or NULL if none
 */
void *dev_get_uclass_platdata(const struct udevice *dev);

/**
 * dev_get_priv() - Get the private data for a device
 *
 * This checks that dev is not NULL, but no other checks for now
 *
 * @dev		Device to check
 * @return private data, or NULL if none
 */
void *dev_get_priv(const struct udevice *dev);

/**
 * dev_get_parent_priv() - Get the parent private data for a device
 *
 * The parent private data is data stored in the device but owned by the
 * parent. For example, a USB device may have parent data which contains
 * information about how to talk to the device over USB.
 *
 * This checks that dev is not NULL, but no other checks for now
 *
 * @dev		Device to check
 * @return parent data, or NULL if none
 */
void *dev_get_parent_priv(const struct udevice *dev);

/**
 * dev_get_uclass_priv() - Get the private uclass data for a device
 *
 * This checks that dev is not NULL, but no other checks for now
 *
 * @dev		Device to check
 * @return private uclass data for this device, or NULL if none
 */
void *dev_get_uclass_priv(const struct udevice *dev);

/**
 * struct dev_get_parent() - Get the parent of a device
 *
 * @child:	Child to check
 * @return parent of child, or NULL if this is the root device
 */
struct udevice *dev_get_parent(const struct udevice *child);

/**
 * dev_get_driver_data() - get the driver data used to bind a device
 *
 * When a device is bound using a device tree node, it matches a
 * particular compatible string in struct udevice_id. This function
 * returns the associated data value for that compatible string. This is
 * the 'data' field in struct udevice_id.
 *
 * As an example, consider this structure:
 * static const struct udevice_id tegra_i2c_ids[] = {
 *	{ .compatible = "nvidia,tegra114-i2c", .data = TYPE_114 },
 *	{ .compatible = "nvidia,tegra20-i2c", .data = TYPE_STD },
 *	{ .compatible = "nvidia,tegra20-i2c-dvc", .data = TYPE_DVC },
 *	{ }
 * };
 *
 * When driver model finds a driver for this it will store the 'data' value
 * corresponding to the compatible string it matches. This function returns
 * that value. This allows the driver to handle several variants of a device.
 *
 * For USB devices, this is the driver_info field in struct usb_device_id.
 *
 * @dev:	Device to check
 * @return driver data (0 if none is provided)
 */
ulong dev_get_driver_data(const struct udevice *dev);

/**
 * dev_get_driver_ops() - get the device's driver's operations
 *
 * This checks that dev is not NULL, and returns the pointer to device's
 * driver's operations.
 *
 * @dev:	Device to check
 * @return void pointer to driver's operations or NULL for NULL-dev or NULL-ops
 */
const void *dev_get_driver_ops(const struct udevice *dev);

/**
 * device_get_uclass_id() - return the uclass ID of a device
 *
 * @dev:	Device to check
 * @return uclass ID for the device
 */
enum uclass_id device_get_uclass_id(const struct udevice *dev);

/**
 * dev_get_uclass_name() - return the uclass name of a device
 *
 * This checks that dev is not NULL.
 *
 * @dev:	Device to check
 * @return  pointer to the uclass name for the device
 */
const char *dev_get_uclass_name(const struct udevice *dev);

/**
 * device_get_child() - Get the child of a device by index
 *
 * Returns the numbered child, 0 being the first. This does not use
 * sequence numbers, only the natural order.
 *
 * @dev:	Parent device to check
 * @index:	Child index
 * @devp:	Returns pointer to device
 * @return 0 if OK, -ENODEV if no such device, other error if the device fails
 *	   to probe
 */
int device_get_child(const struct udevice *parent, int index,
		     struct udevice **devp);

/**
 * device_get_child_count() - Get the available child count of a device
 *
 * Returns the number of children to a device.
 *
 * @parent:	Parent device to check
 */
int device_get_child_count(const struct udevice *parent);

/**
 * device_find_child_by_seq() - Find a child device based on a sequence
 *
 * This searches for a device with the given seq or req_seq.
 *
 * For seq, if an active device has this sequence it will be returned.
 * If there is no such device then this will return -ENODEV.
 *
 * For req_seq, if a device (whether activated or not) has this req_seq
 * value, that device will be returned. This is a strong indication that
 * the device will receive that sequence when activated.
 *
 * @parent: Parent device
 * @seq_or_req_seq: Sequence number to find (0=first)
 * @find_req_seq: true to find req_seq, false to find seq
 * @devp: Returns pointer to device (there is only one per for each seq).
 * Set to NULL if none is found
 * @return 0 if OK, -ve on error
 */
int device_find_child_by_seq(const struct udevice *parent, int seq_or_req_seq,
			     bool find_req_seq, struct udevice **devp);

/**
 * device_get_child_by_seq() - Get a child device based on a sequence
 *
 * If an active device has this sequence it will be returned. If there is no
 * such device then this will check for a device that is requesting this
 * sequence.
 *
 * The device is probed to activate it ready for use.
 *
 * @parent: Parent device
 * @seq: Sequence number to find (0=first)
 * @devp: Returns pointer to device (there is only one per for each seq)
 * Set to NULL if none is found
 * @return 0 if OK, -ve on error
 */
int device_get_child_by_seq(const struct udevice *parent, int seq,
			    struct udevice **devp);

/**
 * device_find_child_by_of_offset() - Find a child device based on FDT offset
 *
 * Locates a child device by its device tree offset.
 *
 * @parent: Parent device
 * @of_offset: Device tree offset to find
 * @devp: Returns pointer to device if found, otherwise this is set to NULL
 * @return 0 if OK, -ve on error
 */
int device_find_child_by_of_offset(const struct udevice *parent, int of_offset,
				   struct udevice **devp);

/**
 * device_get_child_by_of_offset() - Get a child device based on FDT offset
 *
 * Locates a child device by its device tree offset.
 *
 * The device is probed to activate it ready for use.
 *
 * @parent: Parent device
 * @of_offset: Device tree offset to find
 * @devp: Returns pointer to device if found, otherwise this is set to NULL
 * @return 0 if OK, -ve on error
 */
int device_get_child_by_of_offset(const struct udevice *parent, int of_offset,
				  struct udevice **devp);

/**
 * device_find_global_by_ofnode() - Get a device based on ofnode
 *
 * Locates a device by its device tree ofnode, searching globally throughout
 * the all driver model devices.
 *
 * The device is NOT probed
 *
 * @node: Device tree ofnode to find
 * @devp: Returns pointer to device if found, otherwise this is set to NULL
 * @return 0 if OK, -ve on error
 */

int device_find_global_by_ofnode(ofnode node, struct udevice **devp);

/**
 * device_get_global_by_ofnode() - Get a device based on ofnode
 *
 * Locates a device by its device tree ofnode, searching globally throughout
 * the all driver model devices.
 *
 * The device is probed to activate it ready for use.
 *
 * @node: Device tree ofnode to find
 * @devp: Returns pointer to device if found, otherwise this is set to NULL
 * @return 0 if OK, -ve on error
 */
int device_get_global_by_ofnode(ofnode node, struct udevice **devp);

/**
 * device_find_first_child() - Find the first child of a device
 *
 * @parent: Parent device to search
 * @devp: Returns first child device, or NULL if none
 * @return 0
 */
int device_find_first_child(const struct udevice *parent,
			    struct udevice **devp);

/**
 * device_find_next_child() - Find the next child of a device
 *
 * @devp: Pointer to previous child device on entry. Returns pointer to next
 *		child device, or NULL if none
 * @return 0
 */
int device_find_next_child(struct udevice **devp);

/**
 * device_find_first_inactive_child() - Find the first inactive child
 *
 * This is used to locate an existing child of a device which is of a given
 * uclass.
 *
 * The device is NOT probed
 *
 * @parent:	Parent device to search
 * @uclass_id:	Uclass to look for
 * @devp:	Returns device found, if any
 * @return 0 if found, else -ENODEV
 */
int device_find_first_inactive_child(const struct udevice *parent,
				     enum uclass_id uclass_id,
				     struct udevice **devp);

/**
 * device_find_first_child_by_uclass() - Find the first child of a device in uc
 *
 * @parent: Parent device to search
 * @uclass_id:	Uclass to look for
 * @devp: Returns first child device in that uclass, if any
 * @return 0 if found, else -ENODEV
 */
int device_find_first_child_by_uclass(const struct udevice *parent,
				      enum uclass_id uclass_id,
				      struct udevice **devp);

/**
 * device_find_child_by_name() - Find a child by device name
 *
 * @parent:	Parent device to search
 * @name:	Name to look for
 * @devp:	Returns device found, if any
 * @return 0 if found, else -ENODEV
 */
int device_find_child_by_name(const struct udevice *parent, const char *name,
			      struct udevice **devp);

/**
 * device_first_child_ofdata_err() - Find the first child and reads its platdata
 *
 * The ofdata_to_platdata() method is called on the child before it is returned,
 * but the child is not probed.
 *
 * @parent: Parent to check
 * @devp: Returns child that was found, if any
 * @return 0 on success, -ENODEV if no children, other -ve on error
 */
int device_first_child_ofdata_err(struct udevice *parent,
				  struct udevice **devp);

/*
 * device_next_child_ofdata_err() - Find the next child and read its platdata
 *
 * The ofdata_to_platdata() method is called on the child before it is returned,
 * but the child is not probed.
 *
 * @devp: On entry, points to the previous child; on exit returns the child that
 *	was found, if any
 * @return 0 on success, -ENODEV if no children, other -ve on error
 */
int device_next_child_ofdata_err(struct udevice **devp);

/**
 * device_first_child_err() - Get the first child of a device
 *
 * The device returned is probed if necessary, and ready for use
 *
 * @parent:	Parent device to search
 * @devp:	Returns device found, if any
 * @return 0 if found, -ENODEV if not, -ve error if device failed to probe
 */
int device_first_child_err(struct udevice *parent, struct udevice **devp);

/**
 * device_next_child_err() - Get the next child of a parent device
 *
 * The device returned is probed if necessary, and ready for use
 *
 * @devp: On entry, pointer to device to lookup. On exit, returns pointer
 * to the next sibling if no error occurred
 * @return 0 if found, -ENODEV if not, -ve error if device failed to probe
 */
int device_next_child_err(struct udevice **devp);

/**
 * device_has_children() - check if a device has any children
 *
 * @dev:	Device to check
 * @return true if the device has one or more children
 */
bool device_has_children(const struct udevice *dev);

/**
 * device_has_active_children() - check if a device has any active children
 *
 * @dev:	Device to check
 * @return true if the device has one or more children and at least one of
 * them is active (probed).
 */
bool device_has_active_children(const struct udevice *dev);

/**
 * device_is_last_sibling() - check if a device is the last sibling
 *
 * This function can be useful for display purposes, when special action needs
 * to be taken when displaying the last sibling. This can happen when a tree
 * view of devices is being displayed.
 *
 * @dev:	Device to check
 * @return true if there are no more siblings after this one - i.e. is it
 * last in the list.
 */
bool device_is_last_sibling(const struct udevice *dev);

/**
 * device_set_name() - set the name of a device
 *
 * This must be called in the device's bind() method and no later. Normally
 * this is unnecessary but for probed devices which don't get a useful name
 * this function can be helpful.
 *
 * The name is allocated and will be freed automatically when the device is
 * unbound.
 *
 * @dev:	Device to update
 * @name:	New name (this string is allocated new memory and attached to
 *		the device)
 * @return 0 if OK, -ENOMEM if there is not enough memory to allocate the
 * string
 */
int device_set_name(struct udevice *dev, const char *name);

/**
 * device_set_name_alloced() - note that a device name is allocated
 *
 * This sets the DM_FLAG_NAME_ALLOCED flag for the device, so that when it is
 * unbound the name will be freed. This avoids memory leaks.
 *
 * @dev:	Device to update
 */
void device_set_name_alloced(struct udevice *dev);

/**
 * device_is_compatible() - check if the device is compatible with the compat
 *
 * This allows to check whether the device is comaptible with the compat.
 *
 * @dev:	udevice pointer for which compatible needs to be verified.
 * @compat:	Compatible string which needs to verified in the given
 *		device
 * @return true if OK, false if the compatible is not found
 */
bool device_is_compatible(const struct udevice *dev, const char *compat);

/**
 * of_machine_is_compatible() - check if the machine is compatible with
 *				the compat
 *
 * This allows to check whether the machine is comaptible with the compat.
 *
 * @compat:	Compatible string which needs to verified
 * @return true if OK, false if the compatible is not found
 */
bool of_machine_is_compatible(const char *compat);

/**
 * dev_disable_by_path() - Disable a device given its device tree path
 *
 * @path:	The device tree path identifying the device to be disabled
 * @return 0 on success, -ve on error
 */
int dev_disable_by_path(const char *path);

/**
 * dev_enable_by_path() - Enable a device given its device tree path
 *
 * @path:	The device tree path identifying the device to be enabled
 * @return 0 on success, -ve on error
 */
int dev_enable_by_path(const char *path);

/**
 * device_is_on_pci_bus - Test if a device is on a PCI bus
 *
 * @dev:	device to test
 * @return:	true if it is on a PCI bus, false otherwise
 */
static inline bool device_is_on_pci_bus(const struct udevice *dev)
{
	return device_get_uclass_id(dev->parent) == UCLASS_PCI;
}

/**
 * device_foreach_child_safe() - iterate through child devices safely
 *
 * This allows the @pos child to be removed in the loop if required.
 *
 * @pos: struct udevice * for the current device
 * @next: struct udevice * for the next device
 * @parent: parent device to scan
 */
#define device_foreach_child_safe(pos, next, parent)	\
	list_for_each_entry_safe(pos, next, &parent->child_head, sibling_node)

/**
 * device_foreach_child() - iterate through child devices
 *
 * @pos: struct udevice * for the current device
 * @parent: parent device to scan
 */
#define device_foreach_child(pos, parent)	\
	list_for_each_entry(pos, &parent->child_head, sibling_node)

/**
 * device_foreach_child_ofdata_to_platdata() - iterate through children
 *
 * This stops when it gets an error, with @pos set to the device that failed to
 * read ofdata.

 * This creates a for() loop which works through the available children of
 * a device in order from start to end. Device ofdata is read by calling
 * device_ofdata_to_platdata() on each one. The devices are not probed.
 *
 * @pos: struct udevice * for the current device
 * @parent: parent device to scan
 */
#define device_foreach_child_ofdata_to_platdata(pos, parent)	\
	for (int _ret = device_first_child_ofdata_err(parent, &dev); !_ret; \
	     _ret = device_next_child_ofdata_err(&dev))

/**
 * device_foreach_child_probe() - iterate through children, probing them
 *
 * This creates a for() loop which works through the available children of
 * a device in order from start to end. Devices are probed if necessary,
 * and ready for use.
 *
 * This stops when it gets an error, with @pos set to the device that failed to
 * probe
 *
 * @pos: struct udevice * for the current device
 * @parent: parent device to scan
 */
#define device_foreach_child_probe(pos, parent)	\
	for (int _ret = device_first_child_err(parent, &dev); !_ret; \
	     _ret = device_next_child_err(&dev))

/**
 * dm_scan_fdt_dev() - Bind child device in a the device tree
 *
 * This handles device which have sub-nodes in the device tree. It scans all
 * sub-nodes and binds drivers for each node where a driver can be found.
 *
 * If this is called prior to relocation, only pre-relocation devices will be
 * bound (those marked with u-boot,dm-pre-reloc in the device tree, or where
 * the driver has the DM_FLAG_PRE_RELOC flag set). Otherwise, all devices will
 * be bound.
 *
 * @dev:	Device to scan
 * @return 0 if OK, -ve on error
 */
int dm_scan_fdt_dev(struct udevice *dev);

#endif
