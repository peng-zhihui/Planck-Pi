/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2015 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 * Copyright (c) 2016, NVIDIA CORPORATION.
 */

#ifndef _CLK_H_
#define _CLK_H_

#include <dm/ofnode.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/types.h>

/**
 * A clock is a hardware signal that oscillates autonomously at a specific
 * frequency and duty cycle. Most hardware modules require one or more clock
 * signal to drive their operation. Clock signals are typically generated
 * externally to the HW module consuming them, by an entity this API calls a
 * clock provider. This API provides a standard means for drivers to enable and
 * disable clocks, and to set the rate at which they oscillate.
 *
 * A driver that implements UCLASS_CLK is a clock provider. A provider will
 * often implement multiple separate clocks, since the hardware it manages
 * often has this capability. clk-uclass.h describes the interface which
 * clock providers must implement.
 *
 * Clock consumers/clients are the HW modules driven by the clock signals. This
 * header file describes the API used by drivers for those HW modules.
 */

struct udevice;

/**
 * struct clk - A handle to (allowing control of) a single clock.
 *
 * Clients provide storage for clock handles. The content of the structure is
 * managed solely by the clock API and clock drivers. A clock struct is
 * initialized by "get"ing the clock struct. The clock struct is passed to all
 * other clock APIs to identify which clock signal to operate upon.
 *
 * @dev: The device which implements the clock signal.
 * @rate: The clock rate (in HZ).
 * @flags: Flags used across common clock structure (e.g. CLK_)
 *         Clock IP blocks specific flags (i.e. mux, div, gate, etc) are defined
 *         in struct's for those devices (e.g. struct clk_mux).
 * @id: The clock signal ID within the provider.
 * @data: An optional data field for scenarios where a single integer ID is not
 *	  sufficient. If used, it can be populated through an .of_xlate op and
 *	  processed during the various clock ops.
 *
 * Should additional information to identify and configure any clock signal
 * for any provider be required in the future, the struct could be expanded to
 * either (a) add more fields to allow clock providers to store additional
 * information, or (b) replace the id field with an opaque pointer, which the
 * provider would dynamically allocated during its .of_xlate op, and process
 * during is .request op. This may require the addition of an extra op to clean
 * up the allocation.
 */
struct clk {
	struct udevice *dev;
	long long rate;	/* in HZ */
	u32 flags;
	int enable_count;
	/*
	 * Written by of_xlate. In the future, we might add more fields here.
	 */
	unsigned long id;
	unsigned long data;
};

/**
 * struct clk_bulk - A handle to (allowing control of) a bulk of clocks.
 *
 * Clients provide storage for the clock bulk. The content of the structure is
 * managed solely by the clock API. A clock bulk struct is
 * initialized by "get"ing the clock bulk struct.
 * The clock bulk struct is passed to all other bulk clock APIs to apply
 * the API to all the clock in the bulk struct.
 *
 * @clks: An array of clock handles.
 * @count: The number of clock handles in the clks array.
 */
struct clk_bulk {
	struct clk *clks;
	unsigned int count;
};

#if CONFIG_IS_ENABLED(OF_CONTROL) && CONFIG_IS_ENABLED(CLK)
struct phandle_1_arg;
int clk_get_by_index_platdata(struct udevice *dev, int index,
			      struct phandle_1_arg *cells, struct clk *clk);

/**
 * clk_get_by_index - Get/request a clock by integer index.
 *
 * This looks up and requests a clock. The index is relative to the client
 * device; each device is assumed to have n clocks associated with it somehow,
 * and this function finds and requests one of them. The mapping of client
 * device clock indices to provider clocks may be via device-tree properties,
 * board-provided mapping tables, or some other mechanism.
 *
 * @dev:	The client device.
 * @index:	The index of the clock to request, within the client's list of
 *		clocks.
 * @clock	A pointer to a clock struct to initialize.
 * @return 0 if OK, or a negative error code.
 */
int clk_get_by_index(struct udevice *dev, int index, struct clk *clk);

/**
 * clk_get_by_index_nodev - Get/request a clock by integer index
 * without a device.
 *
 * This is a version of clk_get_by_index() that does not use a device.
 *
 * @node:	The client ofnode.
 * @index:	The index of the clock to request, within the client's list of
 *		clocks.
 * @clock	A pointer to a clock struct to initialize.
 * @return 0 if OK, or a negative error code.
 */
int clk_get_by_index_nodev(ofnode node, int index, struct clk *clk);

/**
 * clk_get_bulk - Get/request all clocks of a device.
 *
 * This looks up and requests all clocks of the client device; each device is
 * assumed to have n clocks associated with it somehow, and this function finds
 * and requests all of them in a separate structure. The mapping of client
 * device clock indices to provider clocks may be via device-tree properties,
 * board-provided mapping tables, or some other mechanism.
 *
 * @dev:	The client device.
 * @bulk	A pointer to a clock bulk struct to initialize.
 * @return 0 if OK, or a negative error code.
 */
int clk_get_bulk(struct udevice *dev, struct clk_bulk *bulk);

/**
 * clk_get_by_name - Get/request a clock by name.
 *
 * This looks up and requests a clock. The name is relative to the client
 * device; each device is assumed to have n clocks associated with it somehow,
 * and this function finds and requests one of them. The mapping of client
 * device clock names to provider clocks may be via device-tree properties,
 * board-provided mapping tables, or some other mechanism.
 *
 * @dev:	The client device.
 * @name:	The name of the clock to request, within the client's list of
 *		clocks.
 * @clock:	A pointer to a clock struct to initialize.
 * @return 0 if OK, or a negative error code.
 */
int clk_get_by_name(struct udevice *dev, const char *name, struct clk *clk);

/**
 * clk_get_by_name_nodev - Get/request a clock by name without a device.
 *
 * This is a version of clk_get_by_name() that does not use a device.
 *
 * @node:	The client ofnode.
 * @name:	The name of the clock to request, within the client's list of
 *		clocks.
 * @clock:	A pointer to a clock struct to initialize.
 * @return 0 if OK, or a negative error code.
 */
int clk_get_by_name_nodev(ofnode node, const char *name, struct clk *clk);

/**
 * clk_get_optional_nodev - Get/request an optinonal clock by name
 *		without a device.
 * @node:	The client ofnode.
 * @name:	The name of the clock to request.
 * @name:	The name of the clock to request, within the client's list of
 *		clocks.
 * @clock:	A pointer to a clock struct to initialize.
 *
 * Behaves the same as clk_get_by_name_nodev() except where there is
 * no clock producer, in this case, skip the error number -ENODATA, and
 * the function returns 0.
 */
int clk_get_optional_nodev(ofnode node, const char *name, struct clk *clk);

/**
 * devm_clk_get - lookup and obtain a managed reference to a clock producer.
 * @dev: device for clock "consumer"
 * @id: clock consumer ID
 *
 * Returns a struct clk corresponding to the clock producer, or
 * valid IS_ERR() condition containing errno.  The implementation
 * uses @dev and @id to determine the clock consumer, and thereby
 * the clock producer.  (IOW, @id may be identical strings, but
 * clk_get may return different clock producers depending on @dev.)
 *
 * Drivers must assume that the clock source is not enabled.
 *
 * devm_clk_get should not be called from within interrupt context.
 *
 * The clock will automatically be freed when the device is unbound
 * from the bus.
 */
struct clk *devm_clk_get(struct udevice *dev, const char *id);

/**
 * devm_clk_get_optional - lookup and obtain a managed reference to an optional
 *			   clock producer.
 * @dev: device for clock "consumer"
 * @id: clock consumer ID
 *
 * Behaves the same as devm_clk_get() except where there is no clock producer.
 * In this case, instead of returning -ENOENT, the function returns NULL.
 */
struct clk *devm_clk_get_optional(struct udevice *dev, const char *id);

/**
 * clk_release_all() - Disable (turn off)/Free an array of previously
 * requested clocks.
 *
 * For each clock contained in the clock array, this function will check if
 * clock has been previously requested and then will disable and free it.
 *
 * @clk:	A clock struct array that was previously successfully
 *		requested by clk_request/get_by_*().
 * @count	Number of clock contained in the array
 * @return zero on success, or -ve error code.
 */
int clk_release_all(struct clk *clk, int count);

/**
 * devm_clk_put	- "free" a managed clock source
 * @dev: device used to acquire the clock
 * @clk: clock source acquired with devm_clk_get()
 *
 * Note: drivers must ensure that all clk_enable calls made on this
 * clock source are balanced by clk_disable calls prior to calling
 * this function.
 *
 * clk_put should not be called from within interrupt context.
 */
void devm_clk_put(struct udevice *dev, struct clk *clk);

#else
static inline int clk_get_by_index(struct udevice *dev, int index,
				   struct clk *clk)
{
	return -ENOSYS;
}

static inline int clk_get_bulk(struct udevice *dev, struct clk_bulk *bulk)
{
	return -ENOSYS;
}

static inline int clk_get_by_name(struct udevice *dev, const char *name,
			   struct clk *clk)
{
	return -ENOSYS;
}

static inline int
clk_get_by_name_nodev(ofnode node, const char *name, struct clk *clk)
{
	return -ENOSYS;
}

static inline int
clk_get_optional_nodev(ofnode node, const char *name, struct clk *clk)
{
	return -ENOSYS;
}

static inline int clk_release_all(struct clk *clk, int count)
{
	return -ENOSYS;
}
#endif

#if (CONFIG_IS_ENABLED(OF_CONTROL) && !CONFIG_IS_ENABLED(OF_PLATDATA)) && \
	CONFIG_IS_ENABLED(CLK)
/**
 * clk_set_defaults - Process 'assigned-{clocks/clock-parents/clock-rates}'
 *                    properties to configure clocks
 *
 * @dev:        A device to process (the ofnode associated with this device
 *              will be processed).
 * @stage:	A integer. 0 indicates that this is called before the device
 *		is probed. 1 indicates that this is called just after the
 *		device has been probed
 */
int clk_set_defaults(struct udevice *dev, int stage);
#else
static inline int clk_set_defaults(struct udevice *dev, int stage)
{
	return 0;
}
#endif

/**
 * clk_release_bulk() - Disable (turn off)/Free an array of previously
 * requested clocks in a clock bulk struct.
 *
 * For each clock contained in the clock bulk struct, this function will check
 * if clock has been previously requested and then will disable and free it.
 *
 * @clk:	A clock bulk struct that was previously successfully
 *		requested by clk_get_bulk().
 * @return zero on success, or -ve error code.
 */
static inline int clk_release_bulk(struct clk_bulk *bulk)
{
	return clk_release_all(bulk->clks, bulk->count);
}

#if CONFIG_IS_ENABLED(CLK)
/**
 * clk_request - Request a clock by provider-specific ID.
 *
 * This requests a clock using a provider-specific ID. Generally, this function
 * should not be used, since clk_get_by_index/name() provide an interface that
 * better separates clients from intimate knowledge of clock providers.
 * However, this function may be useful in core SoC-specific code.
 *
 * @dev:	The clock provider device.
 * @clock:	A pointer to a clock struct to initialize. The caller must
 *		have already initialized any field in this struct which the
 *		clock provider uses to identify the clock.
 * @return 0 if OK, or a negative error code.
 */
int clk_request(struct udevice *dev, struct clk *clk);

/**
 * clk_free - Free a previously requested clock.
 *
 * @clock:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return 0 if OK, or a negative error code.
 */
int clk_free(struct clk *clk);

/**
 * clk_get_rate() - Get current clock rate.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return clock rate in Hz, or -ve error code.
 */
ulong clk_get_rate(struct clk *clk);

/**
 * clk_get_parent() - Get current clock's parent.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return pointer to parent's struct clk, or error code passed as pointer
 */
struct clk *clk_get_parent(struct clk *clk);

/**
 * clk_get_parent_rate() - Get parent of current clock rate.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return clock rate in Hz, or -ve error code.
 */
long long clk_get_parent_rate(struct clk *clk);

/**
 * clk_set_rate() - Set current clock rate.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @rate:	New clock rate in Hz.
 * @return new rate, or -ve error code.
 */
ulong clk_set_rate(struct clk *clk, ulong rate);

/**
 * clk_set_parent() - Set current clock parent.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @parent:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return new rate, or -ve error code.
 */
int clk_set_parent(struct clk *clk, struct clk *parent);

/**
 * clk_enable() - Enable (turn on) a clock.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return zero on success, or -ve error code.
 */
int clk_enable(struct clk *clk);

/**
 * clk_enable_bulk() - Enable (turn on) all clocks in a clock bulk struct.
 *
 * @bulk:	A clock bulk struct that was previously successfully requested
 *		by clk_get_bulk().
 * @return zero on success, or -ve error code.
 */
int clk_enable_bulk(struct clk_bulk *bulk);

/**
 * clk_disable() - Disable (turn off) a clock.
 *
 * @clk:	A clock struct that was previously successfully requested by
 *		clk_request/get_by_*().
 * @return zero on success, or -ve error code.
 */
int clk_disable(struct clk *clk);

/**
 * clk_disable_bulk() - Disable (turn off) all clocks in a clock bulk struct.
 *
 * @bulk:	A clock bulk struct that was previously successfully requested
 *		by clk_get_bulk().
 * @return zero on success, or -ve error code.
 */
int clk_disable_bulk(struct clk_bulk *bulk);

/**
 * clk_is_match - check if two clk's point to the same hardware clock
 * @p: clk compared against q
 * @q: clk compared against p
 *
 * Returns true if the two struct clk pointers both point to the same hardware
 * clock node.
 *
 * Returns false otherwise. Note that two NULL clks are treated as matching.
 */
bool clk_is_match(const struct clk *p, const struct clk *q);

/**
 * clk_get_by_id() - Get the clock by its ID
 *
 * @id:	The clock ID to search for
 *
 * @clkp:	A pointer to clock struct that has been found among added clocks
 *              to UCLASS_CLK
 * @return zero on success, or -ENOENT on error
 */
int clk_get_by_id(ulong id, struct clk **clkp);

/**
 * clk_dev_binded() - Check whether the clk has a device binded
 *
 * @clk		A pointer to the clk
 *
 * @return true on binded, or false on no
 */
bool clk_dev_binded(struct clk *clk);

#else /* CONFIG_IS_ENABLED(CLK) */

static inline int clk_request(struct udevice *dev, struct clk *clk)
{
	return -ENOSYS;
}

static inline int clk_free(struct clk *clk)
{
	return 0;
}

static inline ulong clk_get_rate(struct clk *clk)
{
	return -ENOSYS;
}

static inline struct clk *clk_get_parent(struct clk *clk)
{
	return ERR_PTR(-ENOSYS);
}

static inline long long clk_get_parent_rate(struct clk *clk)
{
	return -ENOSYS;
}

static inline ulong clk_set_rate(struct clk *clk, ulong rate)
{
	return -ENOSYS;
}

static inline int clk_set_parent(struct clk *clk, struct clk *parent)
{
	return -ENOSYS;
}

static inline int clk_enable(struct clk *clk)
{
	return 0;
}

static inline int clk_enable_bulk(struct clk_bulk *bulk)
{
	return 0;
}

static inline int clk_disable(struct clk *clk)
{
	return 0;
}

static inline int clk_disable_bulk(struct clk_bulk *bulk)
{
	return 0;
}

static inline bool clk_is_match(const struct clk *p, const struct clk *q)
{
	return false;
}

static inline int clk_get_by_id(ulong id, struct clk **clkp)
{
	return -ENOSYS;
}

static inline bool clk_dev_binded(struct clk *clk)
{
	return false;
}
#endif /* CONFIG_IS_ENABLED(CLK) */

/**
 * clk_valid() - check if clk is valid
 *
 * @clk:	the clock to check
 * @return true if valid, or false
 */
static inline bool clk_valid(struct clk *clk)
{
	return clk && !!clk->dev;
}

int soc_clk_dump(void);

#endif

#define clk_prepare_enable(clk) clk_enable(clk)
#define clk_disable_unprepare(clk) clk_disable(clk)
