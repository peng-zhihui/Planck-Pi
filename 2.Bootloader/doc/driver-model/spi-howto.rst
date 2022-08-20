.. SPDX-License-Identifier: GPL-2.0+

How to port a SPI driver to driver model
========================================

Here is a rough step-by-step guide. It is based around converting the
exynos SPI driver to driver model (DM) and the example code is based
around U-Boot v2014.10-rc2 (commit be9f643). This has been updated for
v2015.04.

It is quite long since it includes actual code examples.

Before driver model, SPI drivers have their own private structure which
contains 'struct spi_slave'. With driver model, 'struct spi_slave' still
exists, but now it is 'per-child data' for the SPI bus. Each child of the
SPI bus is a SPI slave. The information that was stored in the
driver-specific slave structure can now be port in private data for the
SPI bus.

For example, struct tegra_spi_slave looks like this:

.. code-block:: c

	struct tegra_spi_slave {
		struct spi_slave slave;
		struct tegra_spi_ctrl *ctrl;
	};

In this case 'slave' will be in per-child data, and 'ctrl' will be in the
SPI's buses private data.


How long does this take?
------------------------

You should be able to complete this within 2 hours, including testing but
excluding preparing the patches. The API is basically the same as before
with only minor changes:

- methods to set speed and mode are separated out
- cs_info is used to get information on a chip select


Enable driver mode for SPI and SPI flash
----------------------------------------

Add these to your board config:

* CONFIG_DM_SPI
* CONFIG_DM_SPI_FLASH


Add the skeleton
----------------

Put this code at the bottom of your existing driver file:

.. code-block:: c

	struct spi_slave *spi_setup_slave(unsigned int busnum, unsigned int cs,
					  unsigned int max_hz, unsigned int mode)
	{
		return NULL;
	}

	struct spi_slave *spi_setup_slave_fdt(const void *blob, int slave_node,
					      int spi_node)
	{
		return NULL;
	}

	static int exynos_spi_ofdata_to_platdata(struct udevice *dev)
	{
		return -ENODEV;
	}

	static int exynos_spi_probe(struct udevice *dev)
	{
		return -ENODEV;
	}

	static int exynos_spi_remove(struct udevice *dev)
	{
		return -ENODEV;
	}

	static int exynos_spi_claim_bus(struct udevice *dev)
	{

		return -ENODEV;
	}

	static int exynos_spi_release_bus(struct udevice *dev)
	{

		return -ENODEV;
	}

	static int exynos_spi_xfer(struct udevice *dev, unsigned int bitlen,
				   const void *dout, void *din, unsigned long flags)
	{

		return -ENODEV;
	}

	static int exynos_spi_set_speed(struct udevice *dev, uint speed)
	{
		return -ENODEV;
	}

	static int exynos_spi_set_mode(struct udevice *dev, uint mode)
	{
		return -ENODEV;
	}

	static int exynos_cs_info(struct udevice *bus, uint cs,
				  struct spi_cs_info *info)
	{
		return -EINVAL;
	}

	static const struct dm_spi_ops exynos_spi_ops = {
		.claim_bus	= exynos_spi_claim_bus,
		.release_bus	= exynos_spi_release_bus,
		.xfer		= exynos_spi_xfer,
		.set_speed	= exynos_spi_set_speed,
		.set_mode	= exynos_spi_set_mode,
		.cs_info	= exynos_cs_info,
	};

	static const struct udevice_id exynos_spi_ids[] = {
		{ .compatible = "samsung,exynos-spi" },
		{ }
	};

	U_BOOT_DRIVER(exynos_spi) = {
		.name	= "exynos_spi",
		.id	= UCLASS_SPI,
		.of_match = exynos_spi_ids,
		.ops	= &exynos_spi_ops,
		.ofdata_to_platdata = exynos_spi_ofdata_to_platdata,
		.probe	= exynos_spi_probe,
		.remove	= exynos_spi_remove,
	};


Replace 'exynos' in the above code with your driver name
--------------------------------------------------------


#ifdef out all of the code in your driver except for the above
--------------------------------------------------------------

This will allow you to get it building, which means you can work
incrementally. Since all the methods return an error initially, there is
less chance that you will accidentally leave something in.

Also, even though your conversion is basically a rewrite, it might help
reviewers if you leave functions in the same place in the file,
particularly for large drivers.


Add some includes
-----------------

Add these includes to your driver:

.. code-block:: c

	#include <dm.h>
	#include <errno.h>


Build
-----

At this point you should be able to build U-Boot for your board with the
empty SPI driver. You still have empty methods in your driver, but we will
write these one by one.

Set up your platform data structure
-----------------------------------

This will hold the information your driver to operate, like its hardware
address or maximum frequency.

You may already have a struct like this, or you may need to create one
from some of the #defines or global variables in the driver.

Note that this information is not the run-time information. It should not
include state that changes. It should be fixed throughout the live of
U-Boot. Run-time information comes later.

Here is what was in the exynos spi driver:

.. code-block:: c

	struct spi_bus {
		enum periph_id periph_id;
		s32 frequency;		/* Default clock frequency, -1 for none */
		struct exynos_spi *regs;
		int inited;		/* 1 if this bus is ready for use */
		int node;
		uint deactivate_delay_us;	/* Delay to wait after deactivate */
	};

Of these, inited is handled by DM and node is the device tree node, which
DM tells you. The name is not quite right. So in this case we would use:

.. code-block:: c

	struct exynos_spi_platdata {
		enum periph_id periph_id;
		s32 frequency;		/* Default clock frequency, -1 for none */
		struct exynos_spi *regs;
		uint deactivate_delay_us;	/* Delay to wait after deactivate */
	};


Write ofdata_to_platdata() [for device tree only]
-------------------------------------------------

This method will convert information in the device tree node into a C
structure in your driver (called platform data). If you are not using
device tree, go to 8b.

DM will automatically allocate the struct for us when we are using device
tree, but we need to tell it the size:

.. code-block:: c

	U_BOOT_DRIVER(spi_exynos) = {
	...
		.platdata_auto_alloc_size = sizeof(struct exynos_spi_platdata),


Here is a sample function. It gets a pointer to the platform data and
fills in the fields from device tree.

.. code-block:: c

	static int exynos_spi_ofdata_to_platdata(struct udevice *bus)
	{
		struct exynos_spi_platdata *plat = bus->platdata;
		const void *blob = gd->fdt_blob;
		int node = dev_of_offset(bus);

		plat->regs = (struct exynos_spi *)fdtdec_get_addr(blob, node, "reg");
		plat->periph_id = pinmux_decode_periph_id(blob, node);

		if (plat->periph_id == PERIPH_ID_NONE) {
			debug("%s: Invalid peripheral ID %d\n", __func__,
				plat->periph_id);
			return -FDT_ERR_NOTFOUND;
		}

		/* Use 500KHz as a suitable default */
		plat->frequency = fdtdec_get_int(blob, node, "spi-max-frequency",
						500000);
		plat->deactivate_delay_us = fdtdec_get_int(blob, node,
						"spi-deactivate-delay", 0);
		debug("%s: regs=%p, periph_id=%d, max-frequency=%d, deactivate_delay=%d\n",
		      __func__, plat->regs, plat->periph_id, plat->frequency,
		      plat->deactivate_delay_us);

		return 0;
	}


Add the platform data [non-device-tree only]
--------------------------------------------

Specify this data in a U_BOOT_DEVICE() declaration in your board file:

.. code-block:: c

	struct exynos_spi_platdata platdata_spi0 = {
		.periph_id = ...
		.frequency = ...
		.regs = ...
		.deactivate_delay_us = ...
	};

	U_BOOT_DEVICE(board_spi0) = {
		.name = "exynos_spi",
		.platdata = &platdata_spi0,
	};

You will unfortunately need to put the struct definition into a header file
in this case so that your board file can use it.


Add the device private data
---------------------------

Most devices have some private data which they use to keep track of things
while active. This is the run-time information and needs to be stored in
a structure. There is probably a structure in the driver that includes a
'struct spi_slave', so you can use that.

.. code-block:: c

	struct exynos_spi_slave {
		struct spi_slave slave;
		struct exynos_spi *regs;
		unsigned int freq;		/* Default frequency */
		unsigned int mode;
		enum periph_id periph_id;	/* Peripheral ID for this device */
		unsigned int fifo_size;
		int skip_preamble;
		struct spi_bus *bus;		/* Pointer to our SPI bus info */
		ulong last_transaction_us;	/* Time of last transaction end */
	};


We should rename this to make its purpose more obvious, and get rid of
the slave structure, so we have:

.. code-block:: c

	struct exynos_spi_priv {
		struct exynos_spi *regs;
		unsigned int freq;		/* Default frequency */
		unsigned int mode;
		enum periph_id periph_id;	/* Peripheral ID for this device */
		unsigned int fifo_size;
		int skip_preamble;
		ulong last_transaction_us;	/* Time of last transaction end */
	};


DM can auto-allocate this also:

.. code-block:: c

	U_BOOT_DRIVER(spi_exynos) = {
	...
		.priv_auto_alloc_size = sizeof(struct exynos_spi_priv),


Note that this is created before the probe method is called, and destroyed
after the remove method is called. It will be zeroed when the probe
method is called.


Add the probe() and remove() methods
------------------------------------

Note: It's a good idea to build repeatedly as you are working, to avoid a
huge amount of work getting things compiling at the end.

The probe method is supposed to set up the hardware. U-Boot used to use
spi_setup_slave() to do this. So take a look at this function and see
what you can copy out to set things up.

.. code-block:: c

	static int exynos_spi_probe(struct udevice *bus)
	{
		struct exynos_spi_platdata *plat = dev_get_platdata(bus);
		struct exynos_spi_priv *priv = dev_get_priv(bus);

		priv->regs = plat->regs;
		if (plat->periph_id == PERIPH_ID_SPI1 ||
		    plat->periph_id == PERIPH_ID_SPI2)
			priv->fifo_size = 64;
		else
			priv->fifo_size = 256;

		priv->skip_preamble = 0;
		priv->last_transaction_us = timer_get_us();
		priv->freq = plat->frequency;
		priv->periph_id = plat->periph_id;

		return 0;
	}

This implementation doesn't actually touch the hardware, which is somewhat
unusual for a driver. In this case we will do that when the device is
claimed by something that wants to use the SPI bus.

For remove we could shut down the clocks, but in this case there is
nothing to do. DM frees any memory that it allocated, so we can just
remove exynos_spi_remove() and its reference in U_BOOT_DRIVER.


Implement set_speed()
---------------------

This should set up clocks so that the SPI bus is running at the right
speed. With the old API spi_claim_bus() would normally do this and several
of the following functions, so let's look at that function:

.. code-block:: c

	int spi_claim_bus(struct spi_slave *slave)
	{
		struct exynos_spi_slave *spi_slave = to_exynos_spi(slave);
		struct exynos_spi *regs = spi_slave->regs;
		u32 reg = 0;
		int ret;

		ret = set_spi_clk(spi_slave->periph_id,
						spi_slave->freq);
		if (ret < 0) {
			debug("%s: Failed to setup spi clock\n", __func__);
			return ret;
		}

		exynos_pinmux_config(spi_slave->periph_id, PINMUX_FLAG_NONE);

		spi_flush_fifo(slave);

		reg = readl(&regs->ch_cfg);
		reg &= ~(SPI_CH_CPHA_B | SPI_CH_CPOL_L);

		if (spi_slave->mode & SPI_CPHA)
			reg |= SPI_CH_CPHA_B;

		if (spi_slave->mode & SPI_CPOL)
			reg |= SPI_CH_CPOL_L;

		writel(reg, &regs->ch_cfg);
		writel(SPI_FB_DELAY_180, &regs->fb_clk);

		return 0;
	}


It sets up the speed, mode, pinmux, feedback delay and clears the FIFOs.
With DM these will happen in separate methods.


Here is an example for the speed part:

.. code-block:: c

	static int exynos_spi_set_speed(struct udevice *bus, uint speed)
	{
		struct exynos_spi_platdata *plat = bus->platdata;
		struct exynos_spi_priv *priv = dev_get_priv(bus);
		int ret;

		if (speed > plat->frequency)
			speed = plat->frequency;
		ret = set_spi_clk(priv->periph_id, speed);
		if (ret)
			return ret;
		priv->freq = speed;
		debug("%s: regs=%p, speed=%d\n", __func__, priv->regs, priv->freq);

		return 0;
	}


Implement set_mode()
--------------------

This should adjust the SPI mode (polarity, etc.). Again this code probably
comes from the old spi_claim_bus(). Here is an example:

.. code-block:: c

	static int exynos_spi_set_mode(struct udevice *bus, uint mode)
	{
		struct exynos_spi_priv *priv = dev_get_priv(bus);
		uint32_t reg;

		reg = readl(&priv->regs->ch_cfg);
		reg &= ~(SPI_CH_CPHA_B | SPI_CH_CPOL_L);

		if (mode & SPI_CPHA)
			reg |= SPI_CH_CPHA_B;

		if (mode & SPI_CPOL)
			reg |= SPI_CH_CPOL_L;

		writel(reg, &priv->regs->ch_cfg);
		priv->mode = mode;
		debug("%s: regs=%p, mode=%d\n", __func__, priv->regs, priv->mode);

		return 0;
	}


Implement claim_bus()
---------------------

This is where a client wants to make use of the bus, so claims it first.
At this point we need to make sure everything is set up ready for data
transfer. Note that this function is wholly internal to the driver - at
present the SPI uclass never calls it.

Here again we look at the old claim function and see some code that is
needed. It is anything unrelated to speed and mode:

.. code-block:: c

	static int exynos_spi_claim_bus(struct udevice *bus)
	{
		struct exynos_spi_priv *priv = dev_get_priv(bus);

		exynos_pinmux_config(priv->periph_id, PINMUX_FLAG_NONE);
		spi_flush_fifo(priv->regs);

		writel(SPI_FB_DELAY_180, &priv->regs->fb_clk);

		return 0;
	}

The spi_flush_fifo() function is in the removed part of the code, so we
need to expose it again (perhaps with an #endif before it and '#if 0'
after it). It only needs access to priv->regs which is why we have
passed that in:

.. code-block:: c

	/**
	 * Flush spi tx, rx fifos and reset the SPI controller
	 *
	 * @param regs	Pointer to SPI registers
	 */
	static void spi_flush_fifo(struct exynos_spi *regs)
	{
		clrsetbits_le32(&regs->ch_cfg, SPI_CH_HS_EN, SPI_CH_RST);
		clrbits_le32(&regs->ch_cfg, SPI_CH_RST);
		setbits_le32(&regs->ch_cfg, SPI_TX_CH_ON | SPI_RX_CH_ON);
	}


Implement release_bus()
-----------------------

This releases the bus - in our example the old code in spi_release_bus()
is a call to spi_flush_fifo, so we add:

.. code-block:: c

	static int exynos_spi_release_bus(struct udevice *bus)
	{
		struct exynos_spi_priv *priv = dev_get_priv(bus);

		spi_flush_fifo(priv->regs);

		return 0;
	}


Implement xfer()
----------------

This is the final method that we need to create, and it is where all the
work happens. The method parameters are the same as the old spi_xfer() with
the addition of a 'struct udevice' so conversion is pretty easy. Start
by copying the contents of spi_xfer() to your new xfer() method and proceed
from there.

If (flags & SPI_XFER_BEGIN) is non-zero then xfer() normally calls an
activate function, something like this:

.. code-block:: c

	void spi_cs_activate(struct spi_slave *slave)
	{
		struct exynos_spi_slave *spi_slave = to_exynos_spi(slave);

		/* If it's too soon to do another transaction, wait */
		if (spi_slave->bus->deactivate_delay_us &&
		    spi_slave->last_transaction_us) {
			ulong delay_us;		/* The delay completed so far */
			delay_us = timer_get_us() - spi_slave->last_transaction_us;
			if (delay_us < spi_slave->bus->deactivate_delay_us)
				udelay(spi_slave->bus->deactivate_delay_us - delay_us);
		}

		clrbits_le32(&spi_slave->regs->cs_reg, SPI_SLAVE_SIG_INACT);
		debug("Activate CS, bus %d\n", spi_slave->slave.bus);
		spi_slave->skip_preamble = spi_slave->mode & SPI_PREAMBLE;
	}

The new version looks like this:

.. code-block:: c

	static void spi_cs_activate(struct udevice *dev)
	{
		struct udevice *bus = dev->parent;
		struct exynos_spi_platdata *pdata = dev_get_platdata(bus);
		struct exynos_spi_priv *priv = dev_get_priv(bus);

		/* If it's too soon to do another transaction, wait */
		if (pdata->deactivate_delay_us &&
		    priv->last_transaction_us) {
			ulong delay_us;		/* The delay completed so far */
			delay_us = timer_get_us() - priv->last_transaction_us;
			if (delay_us < pdata->deactivate_delay_us)
				udelay(pdata->deactivate_delay_us - delay_us);
		}

		clrbits_le32(&priv->regs->cs_reg, SPI_SLAVE_SIG_INACT);
		debug("Activate CS, bus '%s'\n", bus->name);
		priv->skip_preamble = priv->mode & SPI_PREAMBLE;
	}

All we have really done here is change the pointers and print the device name
instead of the bus number. Other local static functions can be treated in
the same way.


Set up the per-child data and child pre-probe function
------------------------------------------------------

To minimise the pain and complexity of the SPI subsystem while the driver
model change-over is in place, struct spi_slave is used to reference a
SPI bus slave, even though that slave is actually a struct udevice. In fact
struct spi_slave is the device's child data. We need to make sure this space
is available. It is possible to allocate more space that struct spi_slave
needs, but this is the minimum.

.. code-block:: c

	U_BOOT_DRIVER(exynos_spi) = {
	...
		.per_child_auto_alloc_size	= sizeof(struct spi_slave),
	}


Optional: Set up cs_info() if you want it
-----------------------------------------

Sometimes it is useful to know whether a SPI chip select is valid, but this
is not obvious from outside the driver. In this case you can provide a
method for cs_info() to deal with this. If you don't provide it, then the
device tree will be used to determine what chip selects are valid.

Return -EINVAL if the supplied chip select is invalid, or 0 if it is valid.
If you don't provide the cs_info() method, 0 is assumed for all chip selects
that do not appear in the device tree.


Test it
-------

Now that you have the code written and it compiles, try testing it using
the 'sf test' command. You may need to enable CONFIG_CMD_SF_TEST for your
board.


Prepare patches and send them to the mailing lists
--------------------------------------------------

You can use 'tools/patman/patman' to prepare, check and send patches for
your work. See tools/patman/README for details.

A little note about SPI uclass features
---------------------------------------

The SPI uclass keeps some information about each device 'dev' on the bus:

   struct dm_spi_slave_platdata:
     This is device_get_parent_platdata(dev).
     This is where the chip select number is stored, along with
     the default bus speed and mode. It is automatically read
     from the device tree in spi_child_post_bind(). It must not
     be changed at run-time after being set up because platform
     data is supposed to be immutable at run-time.
   struct spi_slave:
     This is device_get_parentdata(dev).
     Already mentioned above. It holds run-time information about
     the device.

There are also some SPI uclass methods that get called behind the scenes:

   spi_post_bind():
     Called when a new bus is bound.
     This scans the device tree for devices on the bus, and binds
     each one. This in turn causes spi_child_post_bind() to be
     called for each, which reads the device tree information
     into the parent (per-child) platform data.
   spi_child_post_bind():
     Called when a new child is bound.
     As mentioned above this reads the device tree information
     into the per-child platform data
   spi_child_pre_probe():
     Called before a new child is probed.
     This sets up the mode and speed in struct spi_slave by
     copying it from the parent's platform data for this child.
     It also sets the 'dev' pointer, needed to permit passing
     'struct spi_slave' around the place without needing a
     separate 'struct udevice' pointer.

The above housekeeping makes it easier to write your SPI driver.
