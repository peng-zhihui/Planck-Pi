.. SPDX-License-Identifier: GPL-2.0+

Apalis iMX8QM V1.0B Module
==========================

Quick Start
-----------

- Build the ARM trusted firmware binary
- Get scfw_tcm.bin and ahab-container.img
- Build U-Boot
- Load U-Boot binary using uuu
- Flash U-Boot binary into the eMMC
- Boot

Get and Build the ARM Trusted Firmware
--------------------------------------

.. code-block:: bash

    $ git clone -b imx_4.14.78_1.0.0_ga https://source.codeaurora.org/external/imx/imx-atf
    $ cd imx-atf/
    $ make PLAT=imx8qm bl31

Get scfw_tcm.bin and ahab-container.img
---------------------------------------

.. code-block:: bash

    $ wget https://github.com/toradex/meta-fsl-bsp-release/blob/toradex-sumo-4.14.78-1.0.0_ga-bringup/imx/meta-bsp/recipes-
      bsp/imx-sc-firmware/files/mx8qm-apalis-scfw-tcm.bin?raw=true
    $ mv mx8qm-apalis-scfw-tcm.bin\?raw\=true mx8qm-apalis-scfw-tcm.bin
    $ wget https://www.nxp.com/lgfiles/NMG/MAD/YOCTO/firmware-imx-8.0.bin
    $ chmod +x firmware-imx-8.0.bin
    $ ./firmware-imx-8.0.bin

Copy the following binaries to the U-Boot folder:

.. code-block:: bash

    $ cp imx-atf/build/imx8qm/release/bl31.bin .
    $ cp u-boot/u-boot.bin .

Copy the following firmware to the U-Boot folder:

.. code-block:: bash

    $ cp firmware-imx-8.0/firmware/seco/ahab-container.img .

Build U-Boot
------------
.. code-block:: bash

    $ make apalis-imx8qm_defconfig
    $ make u-boot-dtb.imx

Load the U-Boot Binary Using UUU
--------------------------------

Get the latest version of the universal update utility (uuu) aka ``mfgtools 3.0``:

https://community.nxp.com/external-link.jspa?url=https%3A%2F%2Fgithub.com%2FNXPmicro%2Fmfgtools%2Freleases

Put the module into USB recovery aka serial downloader mode, connect USB device
to your host and execute uuu:

.. code-block:: bash

    sudo ./uuu u-boot/u-boot-dtb.imx

Flash the U-Boot Binary into the eMMC
-------------------------------------

Burn the ``u-boot-dtb.imx`` binary to the primary eMMC hardware boot area
partition and boot:

.. code-block:: bash

    load mmc 1:1 $loadaddr u-boot-dtb.imx
    setexpr blkcnt ${filesize} + 0x1ff && setexpr blkcnt ${blkcnt} / 0x200
    mmc dev 0 1
    mmc write ${loadaddr} 0x0 ${blkcnt}
