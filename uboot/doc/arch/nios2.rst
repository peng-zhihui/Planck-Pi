.. SPDX-License-Identifier: GPL-2.0+

Nios II
=======

Nios II is a 32-bit embedded-processor architecture designed
specifically for the Altera family of FPGAs.

Please refer to the link for more information on Nios II:
https://www.altera.com/products/processors/overview.html

Please refer to the link for Linux port and toolchains:
http://rocketboards.org/foswiki/view/Documentation/NiosIILinuxUserManual

The Nios II port of u-boot is controlled by device tree. Please check
out doc/README.fdt-control.

To add a new board/configuration (eg, mysystem) to u-boot, you will need
three files.

1. The device tree source which describes the hardware, dts file:
   arch/nios2/dts/mysystem.dts

2. Default configuration of Kconfig, defconfig file:
   configs/mysystem_defconfig

3. The legacy board header file:
   include/configs/mysystem.h

The device tree source must be generated from your qsys/sopc design
using the sopc2dts tool. Then modified to fit your configuration.

Please find the sopc2dts download and usage at the wiki:
http://www.alterawiki.com/wiki/Sopc2dts

.. code-block:: none

   $ java -jar sopc2dts.jar --force-altr -i mysystem.sopcinfo -o mysystem.dts

You will need to add additional properties to the dts. Please find an
example at, arch/nios2/dts/10m50_devboard.dts.

1. Add "stdout-path=..." property with your serial path to the chosen
   node, like this::

	chosen {
		stdout-path = &uart_0;
	};

2. If you use SPI/EPCS or I2C, you will need to add aliases to number
   the sequence of these devices, like this::

	aliases {
		spi0 = &epcs_controller;
	};

Next, you will need a default config file. You may start with
10m50_defconfig, modify the options and save it.

.. code-block:: none

   $ make 10m50_defconfig
   $ make menuconfig
   $ make savedefconfig
   $ cp defconfig configs/mysystem_defconfig

You will need to change the names of board header file and device tree,
and select the drivers with menuconfig.

.. code-block:: none

   Nios II architecture  --->
     (mysystem) Board header file
   Device Tree Control  --->
     (mysystem) Default Device Tree for DT control

There is a selection of "Provider of DTB for DT control" in the Device
Tree Control menu.

   * Separate DTB for DT control, will cat the dtb to end of u-boot
     binary, output u-boot-dtb.bin. This should be used for production.
     If you use boot copier, like EPCS boot copier, make sure the copier
     copies all the u-boot-dtb.bin, not just u-boot.bin.

   * Embedded DTB for DT control, will include the dtb inside the u-boot
     binary. This is handy for development, eg, using gdb or nios2-download.

The last thing, legacy board header file describes those config options
not covered in Kconfig yet. You may copy it from 10m50_devboard.h::

   $ cp include/configs/10m50_devboard.h include/configs/mysystem.h

Please change the SDRAM base and size to match your board. The base
should be cached virtual address, for Nios II with MMU it is 0xCxxx_xxxx
to 0xDxxx_xxxx.

.. code-block:: c

   #define CONFIG_SYS_SDRAM_BASE		0xc8000000
   #define CONFIG_SYS_SDRAM_SIZE		0x08000000

You will need to change the environment variables location and setting,
too. You may change other configs to fit your board.

After all these changes, you may build and test::

   $ export CROSS_COMPILE=nios2-elf-  (or nios2-linux-gnu-)
   $ make mysystem_defconfig
   $ make

Enjoy!
