.. SPDX-License-Identifier: GPL-2.0+

How to port a serial driver to driver model
===========================================

Almost all of the serial drivers have been converted as at January 2016. These
ones remain:

   * serial_bfin.c
   * serial_pxa.c

The deadline for this work was the end of January 2016. If no one steps
forward to convert these, at some point there may come a patch to remove them!

Here is a suggested approach for converting your serial driver over to driver
model. Please feel free to update this file with your ideas and suggestions.

- #ifdef out all your own serial driver code (#ifndef CONFIG_DM_SERIAL)
- Define CONFIG_DM_SERIAL for your board, vendor or architecture
- If the board does not already use driver model, you need CONFIG_DM also
- Your board should then build, but will not boot since there will be no serial
  driver
- Add the U_BOOT_DRIVER piece at the end (e.g. copy serial_s5p.c for example)
- Add a private struct for the driver data - avoid using static variables
- Implement each of the driver methods, perhaps by calling your old methods
- You may need to adjust the function parameters so that the old and new
  implementations can share most of the existing code
- If you convert all existing users of the driver, remove the pre-driver-model
  code

In terms of patches a conversion series typically has these patches:
- clean up / prepare the driver for conversion
- add driver model code
- convert at least one existing board to use driver model serial
- (if no boards remain that don't use driver model) remove the old code

This may be a good time to move your board to use device tree also. Mostly
this involves these steps:

- define CONFIG_OF_CONTROL and CONFIG_OF_SEPARATE
- add your device tree files to arch/<arch>/dts
- update the Makefile there
- Add stdout-path to your /chosen device tree node if it is not already there
- build and get u-boot-dtb.bin so you can test it
- Your drivers can now use device tree
- For device tree in SPL, define CONFIG_SPL_OF_CONTROL
