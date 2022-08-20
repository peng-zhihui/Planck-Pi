// SPDX-License-Identifier: GPL-2.0
/*
 * Memory subsystem support
 *
 * Written by Matt Tolentino <matthew.e.tolentino@intel.com>
 *            Dave Hansen <haveblue@us.ibm.com>
 *
 * This file provides the necessary infrastructure to represent
 * a SPARSEMEM-memory-model system's physical memory in /sysfs.
 * All arch-independent code that assumes MEMORY_HOTPLUG requires
 * SPARSEMEM should be contained here, or in mm/memory_hotplug.c.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/topology.h>
#include <linux/capability.h>
#include <linux/device.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/stat.h>
#include <linux/slab.h>

#include <linux/atomic.h>
#include <linux/uaccess.h>

static DEFINE_MUTEX(mem_sysfs_mutex);

#define MEMORY_CLASS_NAME	"memory"

#define to_memory_block(dev) container_of(dev, struct memory_block, dev)

static int sections_per_block;

static inline unsigned long base_memory_block_id(unsigned long section_nr)
{
	return section_nr / sections_per_block;
}

static inline unsigned long pfn_to_block_id(unsigned long pfn)
{
	return base_memory_block_id(pfn_to_section_nr(pfn));
}

static inline unsigned long phys_to_block_id(unsigned long phys)
{
	return pfn_to_block_id(PFN_DOWN(phys));
}

static int memory_subsys_online(struct device *dev);
static int memory_subsys_offline(struct device *dev);

static struct bus_type memory_subsys = {
	.name = MEMORY_CLASS_NAME,
	.dev_name = MEMORY_CLASS_NAME,
	.online = memory_subsys_online,
	.offline = memory_subsys_offline,
};

static BLOCKING_NOTIFIER_HEAD(memory_chain);

int register_memory_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&memory_chain, nb);
}
EXPORT_SYMBOL(register_memory_notifier);

void unregister_memory_notifier(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&memory_chain, nb);
}
EXPORT_SYMBOL(unregister_memory_notifier);

static ATOMIC_NOTIFIER_HEAD(memory_isolate_chain);

int register_memory_isolate_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&memory_isolate_chain, nb);
}
EXPORT_SYMBOL(register_memory_isolate_notifier);

void unregister_memory_isolate_notifier(struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&memory_isolate_chain, nb);
}
EXPORT_SYMBOL(unregister_memory_isolate_notifier);

static void memory_block_release(struct device *dev)
{
	struct memory_block *mem = to_memory_block(dev);

	kfree(mem);
}

unsigned long __weak memory_block_size_bytes(void)
{
	return MIN_MEMORY_BLOCK_SIZE;
}
EXPORT_SYMBOL_GPL(memory_block_size_bytes);

/*
 * Show the first physical section index (number) of this memory block.
 */
static ssize_t phys_index_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct memory_block *mem = to_memory_block(dev);
	unsigned long phys_index;

	phys_index = mem->start_section_nr / sections_per_block;
	return sprintf(buf, "%08lx\n", phys_index);
}

/*
 * Legacy interface that we cannot remove. Always indicate "removable"
 * with CONFIG_MEMORY_HOTREMOVE - bad heuristic.
 */
static ssize_t removable_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "%d\n", (int)IS_ENABLED(CONFIG_MEMORY_HOTREMOVE));
}

/*
 * online, offline, going offline, etc.
 */
static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct memory_block *mem = to_memory_block(dev);
	ssize_t len = 0;

	/*
	 * We can probably put these states in a nice little array
	 * so that they're not open-coded
	 */
	switch (mem->state) {
	case MEM_ONLINE:
		len = sprintf(buf, "online\n");
		break;
	case MEM_OFFLINE:
		len = sprintf(buf, "offline\n");
		break;
	case MEM_GOING_OFFLINE:
		len = sprintf(buf, "going-offline\n");
		break;
	default:
		len = sprintf(buf, "ERROR-UNKNOWN-%ld\n",
				mem->state);
		WARN_ON(1);
		break;
	}

	return len;
}

int memory_notify(unsigned long val, void *v)
{
	return blocking_notifier_call_chain(&memory_chain, val, v);
}

int memory_isolate_notify(unsigned long val, void *v)
{
	return atomic_notifier_call_chain(&memory_isolate_chain, val, v);
}

/*
 * The probe routines leave the pages uninitialized, just as the bootmem code
 * does. Make sure we do not access them, but instead use only information from
 * within sections.
 */
static bool pages_correctly_probed(unsigned long start_pfn)
{
	unsigned long section_nr = pfn_to_section_nr(start_pfn);
	unsigned long section_nr_end = section_nr + sections_per_block;
	unsigned long pfn = start_pfn;

	/*
	 * memmap between sections is not contiguous except with
	 * SPARSEMEM_VMEMMAP. We lookup the page once per section
	 * and assume memmap is contiguous within each section
	 */
	for (; section_nr < section_nr_end; section_nr++) {
		if (WARN_ON_ONCE(!pfn_valid(pfn)))
			return false;

		if (!present_section_nr(section_nr)) {
			pr_warn("section %ld pfn[%lx, %lx) not present\n",
				section_nr, pfn, pfn + PAGES_PER_SECTION);
			return false;
		} else if (!valid_section_nr(section_nr)) {
			pr_warn("section %ld pfn[%lx, %lx) no valid memmap\n",
				section_nr, pfn, pfn + PAGES_PER_SECTION);
			return false;
		} else if (online_section_nr(section_nr)) {
			pr_warn("section %ld pfn[%lx, %lx) is already online\n",
				section_nr, pfn, pfn + PAGES_PER_SECTION);
			return false;
		}
		pfn += PAGES_PER_SECTION;
	}

	return true;
}

/*
 * MEMORY_HOTPLUG depends on SPARSEMEM in mm/Kconfig, so it is
 * OK to have direct references to sparsemem variables in here.
 */
static int
memory_block_action(unsigned long start_section_nr, unsigned long action,
		    int online_type)
{
	unsigned long start_pfn;
	unsigned long nr_pages = PAGES_PER_SECTION * sections_per_block;
	int ret;

	start_pfn = section_nr_to_pfn(start_section_nr);

	switch (action) {
	case MEM_ONLINE:
		if (!pages_correctly_probed(start_pfn))
			return -EBUSY;

		ret = online_pages(start_pfn, nr_pages, online_type);
		break;
	case MEM_OFFLINE:
		ret = offline_pages(start_pfn, nr_pages);
		break;
	default:
		WARN(1, KERN_WARNING "%s(%ld, %ld) unknown action: "
		     "%ld\n", __func__, start_section_nr, action, action);
		ret = -EINVAL;
	}

	return ret;
}

static int memory_block_change_state(struct memory_block *mem,
		unsigned long to_state, unsigned long from_state_req)
{
	int ret = 0;

	if (mem->state != from_state_req)
		return -EINVAL;

	if (to_state == MEM_OFFLINE)
		mem->state = MEM_GOING_OFFLINE;

	ret = memory_block_action(mem->start_section_nr, to_state,
				mem->online_type);

	mem->state = ret ? from_state_req : to_state;

	return ret;
}

/* The device lock serializes operations on memory_subsys_[online|offline] */
static int memory_subsys_online(struct device *dev)
{
	struct memory_block *mem = to_memory_block(dev);
	int ret;

	if (mem->state == MEM_ONLINE)
		return 0;

	/*
	 * If we are called from state_store(), online_type will be
	 * set >= 0 Otherwise we were called from the device online
	 * attribute and need to set the online_type.
	 */
	if (mem->online_type < 0)
		mem->online_type = MMOP_ONLINE_KEEP;

	ret = memory_block_change_state(mem, MEM_ONLINE, MEM_OFFLINE);

	/* clear online_type */
	mem->online_type = -1;

	return ret;
}

static int memory_subsys_offline(struct device *dev)
{
	struct memory_block *mem = to_memory_block(dev);

	if (mem->state == MEM_OFFLINE)
		return 0;

	/* Can't offline block with non-present sections */
	if (mem->section_count != sections_per_block)
		return -EINVAL;

	return memory_block_change_state(mem, MEM_OFFLINE, MEM_ONLINE);
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct memory_block *mem = to_memory_block(dev);
	int ret, online_type;

	ret = lock_device_hotplug_sysfs();
	if (ret)
		return ret;

	if (sysfs_streq(buf, "online_kernel"))
		online_type = MMOP_ONLINE_KERNEL;
	else if (sysfs_streq(buf, "online_movable"))
		online_type = MMOP_ONLINE_MOVABLE;
	else if (sysfs_streq(buf, "online"))
		online_type = MMOP_ONLINE_KEEP;
	else if (sysfs_streq(buf, "offline"))
		online_type = MMOP_OFFLINE;
	else {
		ret = -EINVAL;
		goto err;
	}

	switch (online_type) {
	case MMOP_ONLINE_KERNEL:
	case MMOP_ONLINE_MOVABLE:
	case MMOP_ONLINE_KEEP:
		/* mem->online_type is protected by device_hotplug_lock */
		mem->online_type = online_type;
		ret = device_online(&mem->dev);
		break;
	case MMOP_OFFLINE:
		ret = device_offline(&mem->dev);
		break;
	default:
		ret = -EINVAL; /* should never happen */
	}

err:
	unlock_device_hotplug();

	if (ret < 0)
		return ret;
	if (ret)
		return -EINVAL;

	return count;
}

/*
 * phys_device is a bad name for this.  What I really want
 * is a way to differentiate between memory ranges that
 * are part of physical devices that constitute
 * a complete removable unit or fru.
 * i.e. do these ranges belong to the same physical device,
 * s.t. if I offline all of these sections I can then
 * remove the physical device?
 */
static ssize_t phys_device_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct memory_block *mem = to_memory_block(dev);
	return sprintf(buf, "%d\n", mem->phys_device);
}

#ifdef CONFIG_MEMORY_HOTREMOVE
static void print_allowed_zone(char *buf, int nid, unsigned long start_pfn,
		unsigned long nr_pages, int online_type,
		struct zone *default_zone)
{
	struct zone *zone;

	zone = zone_for_pfn_range(online_type, nid, start_pfn, nr_pages);
	if (zone != default_zone) {
		strcat(buf, " ");
		strcat(buf, zone->name);
	}
}

static ssize_t valid_zones_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct memory_block *mem = to_memory_block(dev);
	unsigned long start_pfn = section_nr_to_pfn(mem->start_section_nr);
	unsigned long nr_pages = PAGES_PER_SECTION * sections_per_block;
	unsigned long valid_start_pfn, valid_end_pfn;
	struct zone *default_zone;
	int nid;

	/*
	 * Check the existing zone. Make sure that we do that only on the
	 * online nodes otherwise the page_zone is not reliable
	 */
	if (mem->state == MEM_ONLINE) {
		/*
		 * The block contains more than one zone can not be offlined.
		 * This can happen e.g. for ZONE_DMA and ZONE_DMA32
		 */
		if (!test_pages_in_a_zone(start_pfn, start_pfn + nr_pages,
					  &valid_start_pfn, &valid_end_pfn))
			return sprintf(buf, "none\n");
		start_pfn = valid_start_pfn;
		strcat(buf, page_zone(pfn_to_page(start_pfn))->name);
		goto out;
	}

	nid = mem->nid;
	default_zone = zone_for_pfn_range(MMOP_ONLINE_KEEP, nid, start_pfn, nr_pages);
	strcat(buf, default_zone->name);

	print_allowed_zone(buf, nid, start_pfn, nr_pages, MMOP_ONLINE_KERNEL,
			default_zone);
	print_allowed_zone(buf, nid, start_pfn, nr_pages, MMOP_ONLINE_MOVABLE,
			default_zone);
out:
	strcat(buf, "\n");

	return strlen(buf);
}
static DEVICE_ATTR_RO(valid_zones);
#endif

static DEVICE_ATTR_RO(phys_index);
static DEVICE_ATTR_RW(state);
static DEVICE_ATTR_RO(phys_device);
static DEVICE_ATTR_RO(removable);

/*
 * Show the memory block size (shared by all memory blocks).
 */
static ssize_t block_size_bytes_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%lx\n", memory_block_size_bytes());
}

static DEVICE_ATTR_RO(block_size_bytes);

/*
 * Memory auto online policy.
 */

static ssize_t auto_online_blocks_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	if (memhp_auto_online)
		return sprintf(buf, "online\n");
	else
		return sprintf(buf, "offline\n");
}

static ssize_t auto_online_blocks_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	if (sysfs_streq(buf, "online"))
		memhp_auto_online = true;
	else if (sysfs_streq(buf, "offline"))
		memhp_auto_online = false;
	else
		return -EINVAL;

	return count;
}

static DEVICE_ATTR_RW(auto_online_blocks);

/*
 * Some architectures will have custom drivers to do this, and
 * will not need to do it from userspace.  The fake hot-add code
 * as well as ppc64 will do all of their discovery in userspace
 * and will require this interface.
 */
#ifdef CONFIG_ARCH_MEMORY_PROBE
static ssize_t probe_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	u64 phys_addr;
	int nid, ret;
	unsigned long pages_per_block = PAGES_PER_SECTION * sections_per_block;

	ret = kstrtoull(buf, 0, &phys_addr);
	if (ret)
		return ret;

	if (phys_addr & ((pages_per_block << PAGE_SHIFT) - 1))
		return -EINVAL;

	ret = lock_device_hotplug_sysfs();
	if (ret)
		return ret;

	nid = memory_add_physaddr_to_nid(phys_addr);
	ret = __add_memory(nid, phys_addr,
			   MIN_MEMORY_BLOCK_SIZE * sections_per_block);

	if (ret)
		goto out;

	ret = count;
out:
	unlock_device_hotplug();
	return ret;
}

static DEVICE_ATTR_WO(probe);
#endif

#ifdef CONFIG_MEMORY_FAILURE
/*
 * Support for offlining pages of memory
 */

/* Soft offline a page */
static ssize_t soft_offline_page_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int ret;
	u64 pfn;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (kstrtoull(buf, 0, &pfn) < 0)
		return -EINVAL;
	pfn >>= PAGE_SHIFT;
	if (!pfn_valid(pfn))
		return -ENXIO;
	/* Only online pages can be soft-offlined (esp., not ZONE_DEVICE). */
	if (!pfn_to_online_page(pfn))
		return -EIO;
	ret = soft_offline_page(pfn_to_page(pfn), 0);
	return ret == 0 ? count : ret;
}

/* Forcibly offline a page, including killing processes. */
static ssize_t hard_offline_page_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int ret;
	u64 pfn;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (kstrtoull(buf, 0, &pfn) < 0)
		return -EINVAL;
	pfn >>= PAGE_SHIFT;
	ret = memory_failure(pfn, 0);
	return ret ? ret : count;
}

static DEVICE_ATTR_WO(soft_offline_page);
static DEVICE_ATTR_WO(hard_offline_page);
#endif

/*
 * Note that phys_device is optional.  It is here to allow for
 * differentiation between which *physical* devices each
 * section belongs to...
 */
int __weak arch_get_memory_phys_device(unsigned long start_pfn)
{
	return 0;
}

/* A reference for the returned memory block device is acquired. */
static struct memory_block *find_memory_block_by_id(unsigned long block_id)
{
	struct device *dev;

	dev = subsys_find_device_by_id(&memory_subsys, block_id, NULL);
	return dev ? to_memory_block(dev) : NULL;
}

/*
 * For now, we have a linear search to go find the appropriate
 * memory_block corresponding to a particular phys_index. If
 * this gets to be a real problem, we can always use a radix
 * tree or something here.
 *
 * This could be made generic for all device subsystems.
 */
struct memory_block *find_memory_block(struct mem_section *section)
{
	unsigned long block_id = base_memory_block_id(__section_nr(section));

	return find_memory_block_by_id(block_id);
}

static struct attribute *memory_memblk_attrs[] = {
	&dev_attr_phys_index.attr,
	&dev_attr_state.attr,
	&dev_attr_phys_device.attr,
	&dev_attr_removable.attr,
#ifdef CONFIG_MEMORY_HOTREMOVE
	&dev_attr_valid_zones.attr,
#endif
	NULL
};

static struct attribute_group memory_memblk_attr_group = {
	.attrs = memory_memblk_attrs,
};

static const struct attribute_group *memory_memblk_attr_groups[] = {
	&memory_memblk_attr_group,
	NULL,
};

/*
 * register_memory - Setup a sysfs device for a memory block
 */
static
int register_memory(struct memory_block *memory)
{
	int ret;

	memory->dev.bus = &memory_subsys;
	memory->dev.id = memory->start_section_nr / sections_per_block;
	memory->dev.release = memory_block_release;
	memory->dev.groups = memory_memblk_attr_groups;
	memory->dev.offline = memory->state == MEM_OFFLINE;

	ret = device_register(&memory->dev);
	if (ret)
		put_device(&memory->dev);

	return ret;
}

static int init_memory_block(struct memory_block **memory,
			     unsigned long block_id, unsigned long state)
{
	struct memory_block *mem;
	unsigned long start_pfn;
	int ret = 0;

	mem = find_memory_block_by_id(block_id);
	if (mem) {
		put_device(&mem->dev);
		return -EEXIST;
	}
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	mem->start_section_nr = block_id * sections_per_block;
	mem->state = state;
	start_pfn = section_nr_to_pfn(mem->start_section_nr);
	mem->phys_device = arch_get_memory_phys_device(start_pfn);
	mem->nid = NUMA_NO_NODE;

	ret = register_memory(mem);

	*memory = mem;
	return ret;
}

static int add_memory_block(unsigned long base_section_nr)
{
	int ret, section_count = 0;
	struct memory_block *mem;
	unsigned long nr;

	for (nr = base_section_nr; nr < base_section_nr + sections_per_block;
	     nr++)
		if (present_section_nr(nr))
			section_count++;

	if (section_count == 0)
		return 0;
	ret = init_memory_block(&mem, base_memory_block_id(base_section_nr),
				MEM_ONLINE);
	if (ret)
		return ret;
	mem->section_count = section_count;
	return 0;
}

static void unregister_memory(struct memory_block *memory)
{
	if (WARN_ON_ONCE(memory->dev.bus != &memory_subsys))
		return;

	/* drop the ref. we got via find_memory_block() */
	put_device(&memory->dev);
	device_unregister(&memory->dev);
}

/*
 * Create memory block devices for the given memory area. Start and size
 * have to be aligned to memory block granularity. Memory block devices
 * will be initialized as offline.
 */
int create_memory_block_devices(unsigned long start, unsigned long size)
{
	const unsigned long start_block_id = pfn_to_block_id(PFN_DOWN(start));
	unsigned long end_block_id = pfn_to_block_id(PFN_DOWN(start + size));
	struct memory_block *mem;
	unsigned long block_id;
	int ret = 0;

	if (WARN_ON_ONCE(!IS_ALIGNED(start, memory_block_size_bytes()) ||
			 !IS_ALIGNED(size, memory_block_size_bytes())))
		return -EINVAL;

	mutex_lock(&mem_sysfs_mutex);
	for (block_id = start_block_id; block_id != end_block_id; block_id++) {
		ret = init_memory_block(&mem, block_id, MEM_OFFLINE);
		if (ret)
			break;
		mem->section_count = sections_per_block;
	}
	if (ret) {
		end_block_id = block_id;
		for (block_id = start_block_id; block_id != end_block_id;
		     block_id++) {
			mem = find_memory_block_by_id(block_id);
			mem->section_count = 0;
			unregister_memory(mem);
		}
	}
	mutex_unlock(&mem_sysfs_mutex);
	return ret;
}

/*
 * Remove memory block devices for the given memory area. Start and size
 * have to be aligned to memory block granularity. Memory block devices
 * have to be offline.
 */
void remove_memory_block_devices(unsigned long start, unsigned long size)
{
	const unsigned long start_block_id = pfn_to_block_id(PFN_DOWN(start));
	const unsigned long end_block_id = pfn_to_block_id(PFN_DOWN(start + size));
	struct memory_block *mem;
	unsigned long block_id;

	if (WARN_ON_ONCE(!IS_ALIGNED(start, memory_block_size_bytes()) ||
			 !IS_ALIGNED(size, memory_block_size_bytes())))
		return;

	mutex_lock(&mem_sysfs_mutex);
	for (block_id = start_block_id; block_id != end_block_id; block_id++) {
		mem = find_memory_block_by_id(block_id);
		if (WARN_ON_ONCE(!mem))
			continue;
		mem->section_count = 0;
		unregister_memory_block_under_nodes(mem);
		unregister_memory(mem);
	}
	mutex_unlock(&mem_sysfs_mutex);
}

/* return true if the memory block is offlined, otherwise, return false */
bool is_memblock_offlined(struct memory_block *mem)
{
	return mem->state == MEM_OFFLINE;
}

static struct attribute *memory_root_attrs[] = {
#ifdef CONFIG_ARCH_MEMORY_PROBE
	&dev_attr_probe.attr,
#endif

#ifdef CONFIG_MEMORY_FAILURE
	&dev_attr_soft_offline_page.attr,
	&dev_attr_hard_offline_page.attr,
#endif

	&dev_attr_block_size_bytes.attr,
	&dev_attr_auto_online_blocks.attr,
	NULL
};

static struct attribute_group memory_root_attr_group = {
	.attrs = memory_root_attrs,
};

static const struct attribute_group *memory_root_attr_groups[] = {
	&memory_root_attr_group,
	NULL,
};

/*
 * Initialize the sysfs support for memory devices...
 */
void __init memory_dev_init(void)
{
	int ret;
	int err;
	unsigned long block_sz, nr;

	/* Validate the configured memory block size */
	block_sz = memory_block_size_bytes();
	if (!is_power_of_2(block_sz) || block_sz < MIN_MEMORY_BLOCK_SIZE)
		panic("Memory block size not suitable: 0x%lx\n", block_sz);
	sections_per_block = block_sz / MIN_MEMORY_BLOCK_SIZE;

	ret = subsys_system_register(&memory_subsys, memory_root_attr_groups);
	if (ret)
		goto out;

	/*
	 * Create entries for memory sections that were found
	 * during boot and have been initialized
	 */
	mutex_lock(&mem_sysfs_mutex);
	for (nr = 0; nr <= __highest_present_section_nr;
	     nr += sections_per_block) {
		err = add_memory_block(nr);
		if (!ret)
			ret = err;
	}
	mutex_unlock(&mem_sysfs_mutex);

out:
	if (ret)
		panic("%s() failed: %d\n", __func__, ret);
}

/**
 * walk_memory_blocks - walk through all present memory blocks overlapped
 *			by the range [start, start + size)
 *
 * @start: start address of the memory range
 * @size: size of the memory range
 * @arg: argument passed to func
 * @func: callback for each memory section walked
 *
 * This function walks through all present memory blocks overlapped by the
 * range [start, start + size), calling func on each memory block.
 *
 * In case func() returns an error, walking is aborted and the error is
 * returned.
 */
int walk_memory_blocks(unsigned long start, unsigned long size,
		       void *arg, walk_memory_blocks_func_t func)
{
	const unsigned long start_block_id = phys_to_block_id(start);
	const unsigned long end_block_id = phys_to_block_id(start + size - 1);
	struct memory_block *mem;
	unsigned long block_id;
	int ret = 0;

	if (!size)
		return 0;

	for (block_id = start_block_id; block_id <= end_block_id; block_id++) {
		mem = find_memory_block_by_id(block_id);
		if (!mem)
			continue;

		ret = func(mem, arg);
		put_device(&mem->dev);
		if (ret)
			break;
	}
	return ret;
}

struct for_each_memory_block_cb_data {
	walk_memory_blocks_func_t func;
	void *arg;
};

static int for_each_memory_block_cb(struct device *dev, void *data)
{
	struct memory_block *mem = to_memory_block(dev);
	struct for_each_memory_block_cb_data *cb_data = data;

	return cb_data->func(mem, cb_data->arg);
}

/**
 * for_each_memory_block - walk through all present memory blocks
 *
 * @arg: argument passed to func
 * @func: callback for each memory block walked
 *
 * This function walks through all present memory blocks, calling func on
 * each memory block.
 *
 * In case func() returns an error, walking is aborted and the error is
 * returned.
 */
int for_each_memory_block(void *arg, walk_memory_blocks_func_t func)
{
	struct for_each_memory_block_cb_data cb_data = {
		.func = func,
		.arg = arg,
	};

	return bus_for_each_dev(&memory_subsys, NULL, &cb_data,
				for_each_memory_block_cb);
}
