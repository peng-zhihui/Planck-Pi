.. SPDX-License-Identifier: GPL-2.0+

R0P7752C00000RZ board
=====================

This board specification
------------------------

The R0P7752C00000RZ(board config name:sh7752evb) has the following device:

 - SH7752 (SH-4A)
 - DDR3-SDRAM 512MB
 - SPI ROM 8MB
 - Gigabit Ethernet controllers
 - eMMC 4GB


Configuration for This board
----------------------------

You can select the configuration as follows:

 - make sh7752evb_config


This board specific command
---------------------------

This board has the following its specific command:

write_mac:
  You can write MAC address to SPI ROM.

Usage 1: Write MAC address

.. code-block:: none

   write_mac [GETHERC ch0] [GETHERC ch1]

   For example:
   => write_mac 74:90:50:00:33:9e 74:90:50:00:33:9f

* We have to input the command as a single line (without carriage return)
* We have to reset after input the command.

Usage 2: Show current data

.. code-block:: none

   write_mac

   For example:
   => write_mac
      GETHERC ch0 = 74:90:50:00:33:9e
      GETHERC ch1 = 74:90:50:00:33:9f


Update SPI ROM
--------------

1. Copy u-boot image to RAM area.
2. Probe SPI device.

.. code-block:: none

   => sf probe 0
   SF: Detected MX25L6405D with page size 64KiB, total 8 MiB

3. Erase SPI ROM.

.. code-block:: none

   => sf erase 0 80000

4. Write u-boot image to SPI ROM.

.. code-block:: none

   => sf write 0x48000000 0 80000
