.. SPDX-License-Identifier: GPL-2.0+

ARM64
=====

Summary
-------
The initial arm64 U-Boot port was developed before hardware was available,
so the first supported platforms were the Foundation and Fast Model for ARMv8.
These days U-Boot runs on a variety of 64-bit capable ARM hardware, from
embedded development boards to servers.

Notes
-----

1. U-Boot can run at any exception level it is entered in, it is
   recommened to enter it in EL3 if U-Boot takes some responsibilities of a
   classical firmware (like initial hardware setup, CPU errata workarounds
   or SMP bringup). U-Boot can be entered in EL2 when its main purpose is
   that of a boot loader. It can drop to lower exception levels before
   entering the OS.

2. U-Boot for arm64 is compiled with AArch64-gcc. AArch64-gcc
   use rela relocation format, a tool(tools/relocate-rela) by Scott Wood
   is used to encode the initial addend of rela to u-boot.bin. After running,
   the U-Boot will be relocated to destination again.

3. Earlier Linux kernel versions required the FDT to be placed at a
   2 MB boundary and within the same 512 MB section as the kernel image,
   resulting in fdt_high to be defined specially.
   Since kernel version 4.2 Linux is more relaxed about the DT location, so it
   can be placed anywhere in memory.
   Please reference linux/Documentation/arm64/booting.txt for detail.

4. Spin-table is used to wake up secondary processors. One location
   (or per processor location) is defined to hold the kernel entry point
   for secondary processors. It must be ensured that the location is
   accessible and zero immediately after secondary processor
   enter slave_cpu branch execution in start.S. The location address
   is encoded in cpu node of DTS. Linux kernel store the entry point
   of secondary processors to it and send event to wakeup secondary
   processors.
   Please reference linux/Documentation/arm64/booting.txt for detail.

5. Generic board is supported.

6. CONFIG_ARM64 instead of CONFIG_ARMV8 is used to distinguish aarch64 and
   aarch32 specific codes.


Contributors
------------
   * Tom Rini            <trini@ti.com>
   * Scott Wood          <scottwood@freescale.com>
   * York Sun            <yorksun@freescale.com>
   * Simon Glass         <sjg@chromium.org>
   * Sharma Bhupesh      <bhupesh.sharma@freescale.com>
   * Rob Herring         <robherring2@gmail.com>
   * Sergey Temerkhanov  <s.temerkhanov@gmail.com>
