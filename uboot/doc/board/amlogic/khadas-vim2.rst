.. SPDX-License-Identifier: GPL-2.0+

U-Boot for Khadas VIM2
=======================

Khadas VIM2 is an Open Source DIY Box manufactured by Shenzhen Wesion
Technology Co., Ltd with the following specifications:

 - Amlogic S912 ARM Cortex-A53 octo-core SoC @ 1.5GHz
 - ARM Mali T860 GPU
 - 2/3GB DDR4 SDRAM
 - 10/100/1000 Ethernet
 - HDMI 2.0 4K/60Hz display
 - 40-pin GPIO header
 - 2 x USB 2.0 Host, 1 x USB 2.0 Type-C OTG
 - 16GB/32GB/64GB eMMC
 - 2MB SPI Flash
 - microSD
 - SDIO Wifi Module, Bluetooth
 - Two channels IR receiver

U-Boot compilation
------------------

.. code-block:: bash

    $ export CROSS_COMPILE=aarch64-none-elf-
    $ make khadas-vim2_defconfig
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
    $ git clone https://github.com/khadas/u-boot -b khadas-vim-v2015.01 vim-u-boot
    $ cd vim-u-boot
    $ make kvim2_defconfig
    $ make
    $ export FIPDIR=$PWD/fip

Go back to mainline U-Boot source tree then :

.. code-block:: bash

    $ mkdir fip

    $ cp $FIPDIR/gxl/bl2.bin fip/
    $ cp $FIPDIR/gxl/acs.bin fip/
    $ cp $FIPDIR/gxl/bl21.bin fip/
    $ cp $FIPDIR/gxl/bl30.bin fip/
    $ cp $FIPDIR/gxl/bl301.bin fip/
    $ cp $FIPDIR/gxl/bl31.img fip/
    $ cp u-boot.bin fip/bl33.bin

    $ $FIPDIR/blx_fix.sh \
    	fip/bl30.bin \
    	fip/zero_tmp \
    	fip/bl30_zero.bin \
    	fip/bl301.bin \
    	fip/bl301_zero.bin \
    	fip/bl30_new.bin \
    	bl30

    $ python $FIPDIR/acs_tool.pyc fip/bl2.bin fip/bl2_acs.bin fip/acs.bin 0

    $ $FIPDIR/blx_fix.sh \
    	fip/bl2_acs.bin \
    	fip/zero_tmp \
    	fip/bl2_zero.bin \
    	fip/bl21.bin \
    	fip/bl21_zero.bin \
    	fip/bl2_new.bin \
    	bl2

    $ $FIPDIR/gxl/aml_encrypt_gxl --bl3enc --input fip/bl30_new.bin
    $ $FIPDIR/gxl/aml_encrypt_gxl --bl3enc --input fip/bl31.img
    $ $FIPDIR/gxl/aml_encrypt_gxl --bl3enc --input fip/bl33.bin
    $ $FIPDIR/gxl/aml_encrypt_gxl --bl2sig --input fip/bl2_new.bin --output fip/bl2.n.bin.sig
    $ $FIPDIR/gxl/aml_encrypt_gxl --bootmk \
    		--output fip/u-boot.bin \
    		--bl2 fip/bl2.n.bin.sig \
    		--bl30 fip/bl30_new.bin.enc \
    		--bl31 fip/bl31.img.enc \
    		--bl33 fip/bl33.bin.enc

and then write the image to SD with:

.. code-block:: bash

    $ DEV=/dev/your_sd_device
    $ dd if=fip/u-boot.bin.sd.bin of=$DEV conv=fsync,notrunc bs=512 skip=1 seek=1
    $ dd if=fip/u-boot.bin.sd.bin of=$DEV conv=fsync,notrunc bs=1 count=444
