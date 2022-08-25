.. SPDX-License-Identifier: GPL-2.0+

U-Boot for ODROID-C2
====================

ODROID-C2 is a single board computer manufactured by Hardkernel
Co. Ltd with the following specifications:

 - Amlogic S905 ARM Cortex-A53 quad-core SoC @ 2GHz
 - ARM Mali 450 GPU
 - 2GB DDR3 SDRAM
 - Gigabit Ethernet
 - HDMI 2.0 4K/60Hz display
 - 40-pin GPIO header
 - 4 x USB 2.0 Host, 1 x USB OTG
 - eMMC, microSD
 - Infrared receiver

Schematics are available on the manufacturer website.

U-Boot compilation
------------------

.. code-block:: bash

    $ export CROSS_COMPILE=aarch64-none-elf-
    $ make odroid-c2_defconfig
    $ make

Image creation
--------------

Amlogic doesn't provide sources for the firmware and for tools needed
to create the bootloader image, so it is necessary to obtain them from
the git tree published by the board vendor:

.. code-block:: bash

    $ DIR=odroid-c2
    $ git clone --depth 1 \
       https://github.com/hardkernel/u-boot.git -b odroidc2-v2015.01 \
       $DIR
    $ $DIR/fip/fip_create --bl30  $DIR/fip/gxb/bl30.bin \
                       --bl301 $DIR/fip/gxb/bl301.bin \
                       --bl31  $DIR/fip/gxb/bl31.bin \
                       --bl33  u-boot.bin \
                       $DIR/fip.bin
    $ $DIR/fip/fip_create --dump $DIR/fip.bin
    $ cat $DIR/fip/gxb/bl2.package $DIR/fip.bin > $DIR/boot_new.bin
    $ $DIR/fip/gxb/aml_encrypt_gxb --bootsig \
                                --input $DIR/boot_new.bin \
                                --output $DIR/u-boot.img
    $ dd if=$DIR/u-boot.img of=$DIR/u-boot.gxbb bs=512 skip=96

and then write the image to SD with:

.. code-block:: bash

    $ DEV=/dev/your_sd_device
    $ BL1=$DIR/sd_fuse/bl1.bin.hardkernel
    $ dd if=$BL1 of=$DEV conv=fsync bs=1 count=442
    $ dd if=$BL1 of=$DEV conv=fsync bs=512 skip=1 seek=1
    $ dd if=$DIR/u-boot.gxbb of=$DEV conv=fsync bs=512 seek=97
