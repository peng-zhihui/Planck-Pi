.. SPDX-License-Identifier: GPL-2.0+

U-Boot for NanoPi-K2
====================

NanoPi-K2 is a single board computer manufactured by FriendlyElec
with the following specifications:

 - Amlogic S905 ARM Cortex-A53 quad-core SoC @ 1.5GHz
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
    $ make nanopi-k2_defconfig
    $ make

Image creation
--------------

Amlogic doesn't provide sources for the firmware and for tools needed
to create the bootloader image, so it is necessary to obtain them from
the git tree published by the board vendor:

.. code-block:: bash

    $ wget https://releases.linaro.org/archive/13.11/components/toolchain/binaries/gcc-linaro-aarch64-none-elf-4.8-2013.11_linux.tar.xz
    $ wget https://releases.linaro.org/archive/13.11/components/toolchain/binaries/gcc-linaro-arm-none-eabi-4.8-2013.11_linux.tar.xz
    $ tar xvfJ gcc-linaro-aarch64-none-elf-4.8-2013.11_linux.tar.xz
    $ tar xvfJ gcc-linaro-arm-none-eabi-4.8-2013.11_linux.tar.xz
    $ export PATH=$PWD/gcc-linaro-aarch64-none-elf-4.8-2013.11_linux/bin:$PWD/gcc-linaro-arm-none-eabi-4.8-2013.11_linux/bin:$PATH
    $ git clone https://github.com/BayLibre/u-boot.git -b libretech-cc amlogic-u-boot
    $ git clone https://github.com/friendlyarm/u-boot.git -b nanopi-k2-v2015.01 amlogic-u-boot
    $ cd amlogic-u-boot
    $ sed -i 's/aarch64-linux-gnu-/aarch64-none-elf-/' Makefile
    $ sed -i 's/arm-linux-/arm-none-eabi-/' arch/arm/cpu/armv8/gxb/firmware/scp_task/Makefile
    $ make nanopi-k2_defconfig
    $ make
    $ export FIPDIR=$PWD/fip

Go back to mainline U-Boot source tree then :

.. code-block:: bash

    $ mkdir fip

    $ cp $FIPDIR/gxb/bl2.bin fip/
    $ cp $FIPDIR/gxb/acs.bin fip/
    $ cp $FIPDIR/gxb/bl21.bin fip/
    $ cp $FIPDIR/gxb/bl30.bin fip/
    $ cp $FIPDIR/gxb/bl301.bin fip/
    $ cp $FIPDIR/gxb/bl31.img fip/
    $ cp u-boot.bin fip/bl33.bin

    $ $FIPDIR/blx_fix.sh \
    	fip/bl30.bin \
    	fip/zero_tmp \
    	fip/bl30_zero.bin \
    	fip/bl301.bin \
    	fip/bl301_zero.bin \
    	fip/bl30_new.bin \
    	bl30

    $ $FIPDIR/fip_create \
    	 --bl30 fip/bl30_new.bin \
    	 --bl31 fip/bl31.img \
    	 --bl33 fip/bl33.bin \
    	 fip/fip.bin

    $ python $FIPDIR/acs_tool.pyc fip/bl2.bin fip/bl2_acs.bin fip/acs.bin 0

    $ $FIPDIR/blx_fix.sh \
    	fip/bl2_acs.bin \
    	fip/zero_tmp \
    	fip/bl2_zero.bin \
    	fip/bl21.bin \
    	fip/bl21_zero.bin \
    	fip/bl2_new.bin \
    	bl2

    $ cat fip/bl2_new.bin fip/fip.bin > fip/boot_new.bin

    $ $FIPDIR/gxb/aml_encrypt_gxb --bootsig \
    		--input fip/boot_new.bin
    		--output fip/u-boot.bin

and then write the image to SD with:

.. code-block:: bash

    $ DEV=/dev/your_sd_device
    $ dd if=fip/u-boot.bin of=$DEV conv=fsync,notrunc bs=512 seek=1
