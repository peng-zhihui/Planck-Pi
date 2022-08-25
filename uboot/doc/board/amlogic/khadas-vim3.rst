.. SPDX-License-Identifier: GPL-2.0+

U-Boot for Khadas VIM3
======================

Khadas VIM3 is a single board computer manufactured by Shenzhen Wesion
Technology Co., Ltd. with the following specifications:

 - Amlogic A311D Arm Cortex-A53 dual-core + Cortex-A73 quad-core SoC
 - 4GB LPDDR4 SDRAM
 - Gigabit Ethernet
 - HDMI 2.1 display
 - 40-pin GPIO header
 - 1 x USB 3.0 Host, 1 x USB 2.0 Host
 - eMMC, microSD
 - M.2
 - Infrared receiver

Schematics are available on the manufacturer website.

U-Boot compilation
------------------

.. code-block:: bash

    $ export CROSS_COMPILE=aarch64-none-elf-
    $ make khadas-vim3_defconfig
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

    $ DIR=vim3-u-boot
    $ git clone --depth 1 \
       https://github.com/khadas/u-boot.git -b khadas-vims-v2015.01 \
       $DIR

    $ cd vim3-u-boot
    $ make kvim3_defconfig
    $ make
    $ export UBOOTDIR=$PWD

 Go back to mainline U-Boot source tree then :

.. code-block:: bash

    $ mkdir fip

    $ cp $UBOOTDIR/build/scp_task/bl301.bin fip/
    $ cp $UBOOTDIR/build/board/khadas/kvim3/firmware/acs.bin fip/
    $ cp $UBOOTDIR/fip/g12b/bl2.bin fip/
    $ cp $UBOOTDIR/fip/g12b/bl30.bin fip/
    $ cp $UBOOTDIR/fip/g12b/bl31.img fip/
    $ cp $UBOOTDIR/fip/g12b/ddr3_1d.fw fip/
    $ cp $UBOOTDIR/fip/g12b/ddr4_1d.fw fip/
    $ cp $UBOOTDIR/fip/g12b/ddr4_2d.fw fip/
    $ cp $UBOOTDIR/fip/g12b/diag_lpddr4.fw fip/
    $ cp $UBOOTDIR/fip/g12b/lpddr3_1d.fw fip/
    $ cp $UBOOTDIR/fip/g12b/lpddr4_1d.fw fip/
    $ cp $UBOOTDIR/fip/g12b/lpddr4_2d.fw fip/
    $ cp $UBOOTDIR/fip/g12b/piei.fw fip/
    $ cp $UBOOTDIR/fip/g12b/aml_ddr.fw fip/
    $ cp u-boot.bin fip/bl33.bin

    $ sh fip/blx_fix.sh \
    	fip/bl30.bin \
    	fip/zero_tmp \
    	fip/bl30_zero.bin \
    	fip/bl301.bin \
    	fip/bl301_zero.bin \
    	fip/bl30_new.bin \
    	bl30

    $ sh fip/blx_fix.sh \
    	fip/bl2.bin \
    	fip/zero_tmp \
    	fip/bl2_zero.bin \
    	fip/acs.bin \
    	fip/bl21_zero.bin \
    	fip/bl2_new.bin \
    	bl2

    $ $UBOOTDIR/fip/g12b/aml_encrypt_g12b --bl30sig --input fip/bl30_new.bin \
    					--output fip/bl30_new.bin.g12a.enc \
    					--level v3
    $ $UBOOTDIR/fip/g12b/aml_encrypt_g12b --bl3sig --input fip/bl30_new.bin.g12a.enc \
    					--output fip/bl30_new.bin.enc \
    					--level v3 --type bl30
    $ $UBOOTDIR/fip/g12b/aml_encrypt_g12b --bl3sig --input fip/bl31.img \
    					--output fip/bl31.img.enc \
    					--level v3 --type bl31
    $ $UBOOTDIR/fip/g12b/aml_encrypt_g12b --bl3sig --input fip/bl33.bin --compress lz4 \
    					--output fip/bl33.bin.enc \
    					--level v3 --type bl33 --compress lz4
    $ $UBOOTDIR/fip/g12b/aml_encrypt_g12b --bl2sig --input fip/bl2_new.bin \
    					--output fip/bl2.n.bin.sig
    $ $UBOOTDIR/fip/g12b/aml_encrypt_g12b --bootmk \
    		--output fip/u-boot.bin \
    		--bl2 fip/bl2.n.bin.sig \
    		--bl30 fip/bl30_new.bin.enc \
    		--bl31 fip/bl31.img.enc \
    		--bl33 fip/bl33.bin.enc \
    		--ddrfw1 fip/ddr4_1d.fw \
    		--ddrfw2 fip/ddr4_2d.fw \
    		--ddrfw3 fip/ddr3_1d.fw \
    		--ddrfw4 fip/piei.fw \
    		--ddrfw5 fip/lpddr4_1d.fw \
    		--ddrfw6 fip/lpddr4_2d.fw \
    		--ddrfw7 fip/diag_lpddr4.fw \
    		--ddrfw8 fip/aml_ddr.fw \
    		--ddrfw9 fip/lpddr3_1d.fw \
    		--level v3

and then write the image to SD with:

.. code-block:: bash

    $ DEV=/dev/your_sd_device
    $ dd if=fip/u-boot.bin.sd.bin of=$DEV conv=fsync,notrunc bs=512 skip=1 seek=1
    $ dd if=fip/u-boot.bin.sd.bin of=$DEV conv=fsync,notrunc bs=1 count=444
