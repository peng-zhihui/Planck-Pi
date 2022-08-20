.. SPDX-License-Identifier: GPL-2.0+

U-Boot for LibreTech CC
=======================

LibreTech CC is a single board computer manufactured by Libre Technology
with the following specifications:

 - Amlogic S905X ARM Cortex-A53 quad-core SoC @ 1.5GHz
 - ARM Mali 450 GPU
 - 2GB DDR3 SDRAM
 - 10/100 Ethernet
 - HDMI 2.0 4K/60Hz display
 - 40-pin GPIO header
 - 4 x USB 2.0 Host
 - eMMC, microSD
 - Infrared receiver

Schematics are available on the manufacturer website.

U-Boot compilation
------------------

.. code-block:: bash

    $ export CROSS_COMPILE=aarch64-none-elf-
    $ make libretech-cc_defconfig
    $ make

Image creation
--------------

To boot the system, u-boot must be combined with several earlier stage
bootloaders:

* bl2.bin: vendor-provided binary blob
* bl21.bin: built from vendor u-boot source
* bl30.bin: vendor-provided binary blob
* bl301.bin: built from vendor u-boot source
* bl31.bin: vendor-provided binary blob
* acs.bin: built from vendor u-boot source

These binaries and the tools required below have been collected and prebuilt
for convenience at <https://github.com/BayLibre/u-boot/releases/>

Download and extract the libretech-cc release from there, and set FIPDIR to
point to the `fip` subdirectory.

.. code-block:: bash

    $ export FIPDIR=/path/to/extracted/fip

Alternatively, you can obtain the original vendor u-boot tree which
contains the required blobs and sources, and build yourself.
Note that old compilers are required for this to build. The compilers here
are suggested by Amlogic, and they are 32-bit x86 binaries.

.. code-block:: bash

    $ wget https://releases.linaro.org/archive/13.11/components/toolchain/binaries/gcc-linaro-aarch64-none-elf-4.8-2013.11_linux.tar.xz
    $ wget https://releases.linaro.org/archive/13.11/components/toolchain/binaries/gcc-linaro-arm-none-eabi-4.8-2013.11_linux.tar.xz
    $ tar xvfJ gcc-linaro-aarch64-none-elf-4.8-2013.11_linux.tar.xz
    $ tar xvfJ gcc-linaro-arm-none-eabi-4.8-2013.11_linux.tar.xz
    $ export PATH=$PWD/gcc-linaro-aarch64-none-elf-4.8-2013.11_linux/bin:$PWD/gcc-linaro-arm-none-eabi-4.8-2013.11_linux/bin:$PATH
    $ git clone https://github.com/BayLibre/u-boot.git -b libretech-cc amlogic-u-boot
    $ cd amlogic-u-boot
    $ make libretech_cc_defconfig
    $ make
    $ export FIPDIR=$PWD/fip

Once you have the binaries available (either through the prebuilt download,
or having built the vendor u-boot yourself), you can then proceed to glue
everything together. Go back to mainline U-Boot source tree then :

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

    $ $FIPDIR/acs_tool.pyc fip/bl2.bin fip/bl2_acs.bin fip/acs.bin 0

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

Note that Amlogic provides aml_encrypt_gxl as a 32-bit x86 binary with no
source code. Should you prefer to avoid that, there are open source reverse
engineered versions available:

1. gxlimg <https://github.com/repk/gxlimg>, which comes with a handy
   Makefile that automates the whole process.
2. meson-tools <https://github.com/afaerber/meson-tools>

However, these community-developed alternatives are not endorsed by or
supported by Amlogic.
