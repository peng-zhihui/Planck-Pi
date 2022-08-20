.. SPDX-License-Identifier: GPL-2.0+
.. Copyright (C) 2017, Tuomas Tynkkynen <tuomas.tynkkynen@iki.fi>

QEMU ARM
========

QEMU for ARM supports a special 'virt' machine designed for emulation and
virtualization purposes. This document describes how to run U-Boot under it.
Both 32-bit ARM and AArch64 are supported.

The 'virt' platform provides the following as the basic functionality:

    - A freely configurable amount of CPU cores
    - U-Boot loaded and executing in the emulated flash at address 0x0
    - A generated device tree blob placed at the start of RAM
    - A freely configurable amount of RAM, described by the DTB
    - A PL011 serial port, discoverable via the DTB
    - An ARMv7/ARMv8 architected timer
    - PSCI for rebooting the system
    - A generic ECAM-based PCI host controller, discoverable via the DTB

Additionally, a number of optional peripherals can be added to the PCI bus.

Building U-Boot
---------------
Set the CROSS_COMPILE environment variable as usual, and run:

- For ARM::

    make qemu_arm_defconfig
    make

- For AArch64::

    make qemu_arm64_defconfig
    make

Running U-Boot
--------------
The minimal QEMU command line to get U-Boot up and running is:

- For ARM::

    qemu-system-arm -machine virt -bios u-boot.bin

- For AArch64::

    qemu-system-aarch64 -machine virt -cpu cortex-a57 -bios u-boot.bin

Note that for some odd reason qemu-system-aarch64 needs to be explicitly
told to use a 64-bit CPU or it will boot in 32-bit mode.

Additional persistent U-boot environment support can be added as follows:

- Create envstore.img using qemu-img::

    qemu-img create -f raw envstore.img 64M

- Add a pflash drive parameter to the command line::

    -drive if=pflash,format=raw,index=1,file=envstore.img

Additional peripherals that have been tested to work in both U-Boot and Linux
can be enabled with the following command line parameters:

- To add a Serial ATA disk via an Intel ICH9 AHCI controller, pass e.g.::

    -drive if=none,file=disk.img,id=mydisk -device ich9-ahci,id=ahci -device ide-drive,drive=mydisk,bus=ahci.0

- To add an Intel E1000 network adapter, pass e.g.::

    -netdev user,id=net0 -device e1000,netdev=net0

- To add an EHCI-compliant USB host controller, pass e.g.::

    -device usb-ehci,id=ehci

- To add a NVMe disk, pass e.g.::

    -drive if=none,file=disk.img,id=mydisk -device nvme,drive=mydisk,serial=foo

These have been tested in QEMU 2.9.0 but should work in at least 2.5.0 as well.
