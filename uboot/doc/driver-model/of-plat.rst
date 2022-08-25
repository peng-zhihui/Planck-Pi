.. SPDX-License-Identifier: GPL-2.0+

Compiled-in Device Tree / Platform Data
=======================================


Introduction
------------

Device tree is the standard configuration method in U-Boot. It is used to
define what devices are in the system and provide configuration information
to these devices.

The overhead of adding device tree access to U-Boot is fairly modest,
approximately 3KB on Thumb 2 (plus the size of the DT itself). This means
that in most cases it is best to use device tree for configuration.

However there are some very constrained environments where U-Boot needs to
work. These include SPL with severe memory limitations. For example, some
SoCs require a 16KB SPL image which must include a full MMC stack. In this
case the overhead of device tree access may be too great.

It is possible to create platform data manually by defining C structures
for it, and reference that data in a U_BOOT_DEVICE() declaration. This
bypasses the use of device tree completely, effectively creating a parallel
configuration mechanism. But it is an available option for SPL.

As an alternative, a new 'of-platdata' feature is provided. This converts the
device tree contents into C code which can be compiled into the SPL binary.
This saves the 3KB of code overhead and perhaps a few hundred more bytes due
to more efficient storage of the data.

Note: Quite a bit of thought has gone into the design of this feature.
However it still has many rough edges and comments and suggestions are
strongly encouraged! Quite possibly there is a much better approach.


Caveats
-------

There are many problems with this features. It should only be used when
strictly necessary. Notable problems include:

   - Device tree does not describe data types. But the C code must define a
     type for each property. These are guessed using heuristics which
     are wrong in several fairly common cases. For example an 8-byte value
     is considered to be a 2-item integer array, and is byte-swapped. A
     boolean value that is not present means 'false', but cannot be
     included in the structures since there is generally no mention of it
     in the device tree file.

   - Naming of nodes and properties is automatic. This means that they follow
     the naming in the device tree, which may result in C identifiers that
     look a bit strange.

   - It is not possible to find a value given a property name. Code must use
     the associated C member variable directly in the code. This makes
     the code less robust in the face of device-tree changes. It also
     makes it very unlikely that your driver code will be useful for more
     than one SoC. Even if the code is common, each SoC will end up with
     a different C struct name, and a likely a different format for the
     platform data.

   - The platform data is provided to drivers as a C structure. The driver
     must use the same structure to access the data. Since a driver
     normally also supports device tree it must use #ifdef to separate
     out this code, since the structures are only available in SPL.

   - Correct relations between nodes are not implemented. This means that
     parent/child relations (like bus device iteration) do not work yet.
     Some phandles (those that are recognised as such) are converted into
     a pointer to platform data. This pointer can potentially be used to
     access the referenced device (by searching for the pointer value).
     This feature is not yet implemented, however.


How it works
------------

The feature is enabled by CONFIG OF_PLATDATA. This is only available in
SPL/TPL and should be tested with:

.. code-block:: c

    #if CONFIG_IS_ENABLED(OF_PLATDATA)

A new tool called 'dtoc' converts a device tree file either into a set of
struct declarations, one for each compatible node, and a set of
U_BOOT_DEVICE() declarations along with the actual platform data for each
device. As an example, consider this MMC node:

.. code-block:: none

    sdmmc: dwmmc@ff0c0000 {
            compatible = "rockchip,rk3288-dw-mshc";
            clock-freq-min-max = <400000 150000000>;
            clocks = <&cru HCLK_SDMMC>, <&cru SCLK_SDMMC>,
                     <&cru SCLK_SDMMC_DRV>, <&cru SCLK_SDMMC_SAMPLE>;
            clock-names = "biu", "ciu", "ciu_drv", "ciu_sample";
            fifo-depth = <0x100>;
            interrupts = <GIC_SPI 32 IRQ_TYPE_LEVEL_HIGH>;
            reg = <0xff0c0000 0x4000>;
            bus-width = <4>;
            cap-mmc-highspeed;
            cap-sd-highspeed;
            card-detect-delay = <200>;
            disable-wp;
            num-slots = <1>;
            pinctrl-names = "default";
            pinctrl-0 = <&sdmmc_clk>, <&sdmmc_cmd>, <&sdmmc_cd>, <&sdmmc_bus4>;
                vmmc-supply = <&vcc_sd>;
                status = "okay";
                u-boot,dm-pre-reloc;
        };


Some of these properties are dropped by U-Boot under control of the
CONFIG_OF_SPL_REMOVE_PROPS option. The rest are processed. This will produce
the following C struct declaration:

.. code-block:: c

    struct dtd_rockchip_rk3288_dw_mshc {
            fdt32_t         bus_width;
            bool            cap_mmc_highspeed;
            bool            cap_sd_highspeed;
            fdt32_t         card_detect_delay;
            fdt32_t         clock_freq_min_max[2];
            struct phandle_1_arg clocks[4];
            bool            disable_wp;
            fdt32_t         fifo_depth;
            fdt32_t         interrupts[3];
            fdt32_t         num_slots;
            fdt32_t         reg[2];
            fdt32_t         vmmc_supply;
    };

and the following device declaration:

.. code-block:: c

    static struct dtd_rockchip_rk3288_dw_mshc dtv_dwmmc_at_ff0c0000 = {
            .fifo_depth             = 0x100,
            .cap_sd_highspeed       = true,
            .interrupts             = {0x0, 0x20, 0x4},
            .clock_freq_min_max     = {0x61a80, 0x8f0d180},
            .vmmc_supply            = 0xb,
            .num_slots              = 0x1,
            .clocks                 = {{&dtv_clock_controller_at_ff760000, 456},
                                       {&dtv_clock_controller_at_ff760000, 68},
                                       {&dtv_clock_controller_at_ff760000, 114},
                                       {&dtv_clock_controller_at_ff760000, 118}},
            .cap_mmc_highspeed      = true,
            .disable_wp             = true,
            .bus_width              = 0x4,
            .u_boot_dm_pre_reloc    = true,
            .reg                    = {0xff0c0000, 0x4000},
            .card_detect_delay      = 0xc8,
    };

    U_BOOT_DEVICE(dwmmc_at_ff0c0000) = {
            .name           = "rockchip_rk3288_dw_mshc",
            .platdata       = &dtv_dwmmc_at_ff0c0000,
            .platdata_size  = sizeof(dtv_dwmmc_at_ff0c0000),
    };

The device is then instantiated at run-time and the platform data can be
accessed using:

.. code-block:: c

    struct udevice *dev;
    struct dtd_rockchip_rk3288_dw_mshc *plat = dev_get_platdata(dev);

This avoids the code overhead of converting the device tree data to
platform data in the driver. The ofdata_to_platdata() method should
therefore do nothing in such a driver.

Note that for the platform data to be matched with a driver, the 'name'
property of the U_BOOT_DEVICE() declaration has to match a driver declared
via U_BOOT_DRIVER(). This effectively means that a U_BOOT_DRIVER() with a
'name' corresponding to the devicetree 'compatible' string (after converting
it to a valid name for C) is needed, so a dedicated driver is required for
each 'compatible' string.

Where a node has multiple compatible strings, a #define is used to make them
equivalent, e.g.:

.. code-block:: c

    #define dtd_rockchip_rk3299_dw_mshc dtd_rockchip_rk3288_dw_mshc


Converting of-platdata to a useful form
---------------------------------------

Of course it would be possible to use the of-platdata directly in your driver
whenever configuration information is required. However this means that the
driver will not be able to support device tree, since the of-platdata
structure is not available when device tree is used. It would make no sense
to use this structure if device tree were available, since the structure has
all the limitations metioned in caveats above.

Therefore it is recommended that the of-platdata structure should be used
only in the probe() method of your driver. It cannot be used in the
ofdata_to_platdata() method since this is not called when platform data is
already present.


How to structure your driver
----------------------------

Drivers should always support device tree as an option. The of-platdata
feature is intended as a add-on to existing drivers.

Your driver should convert the platdata struct in its probe() method. The
existing device tree decoding logic should be kept in the
ofdata_to_platdata() method and wrapped with #if.

For example:

.. code-block:: c

    #include <dt-structs.h>

    struct mmc_platdata {
    #if CONFIG_IS_ENABLED(OF_PLATDATA)
            /* Put this first since driver model will copy the data here */
            struct dtd_mmc dtplat;
    #endif
            /*
             * Other fields can go here, to be filled in by decoding from
             * the device tree (or the C structures when of-platdata is used).
             */
            int fifo_depth;
    };

    static int mmc_ofdata_to_platdata(struct udevice *dev)
    {
    #if !CONFIG_IS_ENABLED(OF_PLATDATA)
            /* Decode the device tree data */
            struct mmc_platdata *plat = dev_get_platdata(dev);
            const void *blob = gd->fdt_blob;
            int node = dev_of_offset(dev);

            plat->fifo_depth = fdtdec_get_int(blob, node, "fifo-depth", 0);
    #endif

            return 0;
    }

    static int mmc_probe(struct udevice *dev)
    {
            struct mmc_platdata *plat = dev_get_platdata(dev);

    #if CONFIG_IS_ENABLED(OF_PLATDATA)
            /* Decode the of-platdata from the C structures */
            struct dtd_mmc *dtplat = &plat->dtplat;

            plat->fifo_depth = dtplat->fifo_depth;
    #endif
            /* Set up the device from the plat data */
            writel(plat->fifo_depth, ...)
    }

    static const struct udevice_id mmc_ids[] = {
            { .compatible = "vendor,mmc" },
            { }
    };

    U_BOOT_DRIVER(mmc_drv) = {
            .name           = "vendor_mmc",  /* matches compatible string */
            .id             = UCLASS_MMC,
            .of_match       = mmc_ids,
            .ofdata_to_platdata = mmc_ofdata_to_platdata,
            .probe          = mmc_probe,
            .priv_auto_alloc_size = sizeof(struct mmc_priv),
            .platdata_auto_alloc_size = sizeof(struct mmc_platdata),
    };


Note that struct mmc_platdata is defined in the C file, not in a header. This
is to avoid needing to include dt-structs.h in a header file. The idea is to
keep the use of each of-platdata struct to the smallest possible code area.
There is just one driver C file for each struct, that can convert from the
of-platdata struct to the standard one used by the driver.

In the case where SPL_OF_PLATDATA is enabled, platdata_auto_alloc_size is
still used to allocate space for the platform data. This is different from
the normal behaviour and is triggered by the use of of-platdata (strictly
speaking it is a non-zero platdata_size which triggers this).

The of-platdata struct contents is copied from the C structure data to the
start of the newly allocated area. In the case where device tree is used,
the platform data is allocated, and starts zeroed. In this case the
ofdata_to_platdata() method should still set up the platform data (and the
of-platdata struct will not be present).

SPL must use either of-platdata or device tree. Drivers cannot use both at
the same time, but they must support device tree. Supporting of-platdata is
optional.

The device tree becomes in accessible when CONFIG_SPL_OF_PLATDATA is enabled,
since the device-tree access code is not compiled in. A corollary is that
a board can only move to using of-platdata if all the drivers it uses support
it. There would be little point in having some drivers require the device
tree data, since then libfdt would still be needed for those drivers and
there would be no code-size benefit.

Internals
---------

The dt-structs.h file includes the generated file
(include/generated//dt-structs.h) if CONFIG_SPL_OF_PLATDATA is enabled.
Otherwise (such as in U-Boot proper) these structs are not available. This
prevents them being used inadvertently. All usage must be bracketed with
#if CONFIG_IS_ENABLED(OF_PLATDATA).

The dt-platdata.c file contains the device declarations and is is built in
spl/dt-platdata.c.

The beginnings of a libfdt Python module are provided. So far this only
implements a subset of the features.

The 'swig' tool is needed to build the libfdt Python module. If this is not
found then the Python model is not used and a fallback is used instead, which
makes use of fdtget.


Credits
-------

This is an implementation of an idea by Tom Rini <trini@konsulko.com>.


Future work
-----------
- Consider programmatically reading binding files instead of device tree
  contents
- Complete the phandle feature
- Move to using a full Python libfdt module


.. Simon Glass <sjg@chromium.org>
.. Google, Inc
.. 6/6/16
.. Updated Independence Day 2016
