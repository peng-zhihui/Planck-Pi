.. SPDX-License-Identifier: GPL-2.0+
.. (C) Copyright 2014-2015 Samsung Electronics
.. sectionauthor:: Przemyslaw Marczak <p.marczak@samsung.com>

PMIC framework based on Driver Model
====================================

Introduction
------------
This is an introduction to driver-model multi uclass PMIC IC's support.
At present it's based on two uclass types:

UCLASS_PMIC:
  basic uclass type for PMIC I/O, which provides common
  read/write interface.
UCLASS_REGULATOR:
  additional uclass type for specific PMIC features, which are
  Voltage/Current regulators.

New files:

UCLASS_PMIC:
  - drivers/power/pmic/pmic-uclass.c
  - include/power/pmic.h
UCLASS_REGULATOR:
  - drivers/power/regulator/regulator-uclass.c
  - include/power/regulator.h

Commands:
- common/cmd_pmic.c
- common/cmd_regulator.c

How doees it work
-----------------
The Power Management Integrated Circuits (PMIC) are used in embedded systems
to provide stable, precise and specific voltage power source with over-voltage
and thermal protection circuits.

The single PMIC can provide various functions by single or multiple interfaces,
like in the example below::

   -- SoC
    |
    |            ______________________________________
    | BUS 0     |       Multi interface PMIC IC        |--> LDO out 1
    | e.g.I2C0  |                                      |--> LDO out N
    |-----------|---- PMIC device 0 (READ/WRITE ops)   |
    | or SPI0   |    |_ REGULATOR device (ldo/... ops) |--> BUCK out 1
    |           |    |_ CHARGER device (charger ops)   |--> BUCK out M
    |           |    |_ MUIC device (microUSB con ops) |
    | BUS 1     |    |_ ...                            |---> BATTERY
    | e.g.I2C1  |                                      |
    |-----------|---- PMIC device 1 (READ/WRITE ops)   |---> USB in 1
    . or SPI1   |    |_ RTC device (rtc ops)           |---> USB in 2
    .           |______________________________________|---> USB out
    .

Since U-Boot provides driver model features for I2C and SPI bus drivers,
the PMIC devices should also support this. By the pmic and regulator API's,
PMIC drivers can simply provide a common functions, for multi-interface and
and multi-instance device support.

Basic design assumptions:

- Common I/O API:
    UCLASS_PMIC. For the multi-function PMIC devices, this can be used as
    parent I/O device for each IC's interface. Then, each children uses the
    same dev for read/write.

- Common regulator API:
    UCLASS_REGULATOR. For driving the regulator attributes, auto setting
    function or command line interface, based on kernel-style regulator device
    tree constraints.

For simple implementations, regulator drivers are not required, so the code can
use pmic read/write directly.

Pmic uclass
-----------
The basic information:

* Uclass:   'UCLASS_PMIC'
* Header:   'include/power/pmic.h'
* Core:     'drivers/power/pmic/pmic-uclass.c' (config 'CONFIG_DM_PMIC')
* Command:  'common/cmd_pmic.c' (config 'CONFIG_CMD_PMIC')
* Example:  'drivers/power/pmic/max77686.c'

For detailed API description, please refer to the header file.

As an example of the pmic driver, please refer to the MAX77686 driver.

Please pay attention for the driver's bind() method. Exactly the function call:
'pmic_bind_children()', which is used to bind the regulators by using the array
of regulator's node, compatible prefixes.

The 'pmic; command also supports the new API. So the pmic command can be enabled
by adding CONFIG_CMD_PMIC.
The new pmic command allows to:
- list pmic devices
- choose the current device (like the mmc command)
- read or write the pmic register
- dump all pmic registers

This command can use only UCLASS_PMIC devices, since this uclass is designed
for pmic I/O operations only.

For more information, please refer to the core file.

Regulator uclass
----------------
The basic information:

* Uclass: 'UCLASS_REGULATOR'

* Header: 'include/power/regulator.h'

* Core: 'drivers/power/regulator/regulator-uclass.c'
  (config 'CONFIG_DM_REGULATOR')

* Binding: 'doc/device-tree-bindings/regulator/regulator.txt'

* Command: 'common/cmd_regulator.c' (config 'CONFIG_CMD_REGULATOR')

* Example: 'drivers/power/regulator/max77686.c'
  'drivers/power/pmic/max77686.c' (required I/O driver for the above)

* Example: 'drivers/power/regulator/fixed.c'
  (config 'CONFIG_DM_REGULATOR_FIXED')

For detailed API description, please refer to the header file.

For the example regulator driver, please refer to the MAX77686 regulator driver,
but this driver can't operate without pmic's example driver, which provides an
I/O interface for MAX77686 regulator.

The second example is a fixed Voltage/Current regulator for a common use.

The 'regulator' command also supports the new API. The command allow:
- list regulator devices
- choose the current device (like the mmc command)
- do all regulator-specific operations

For more information, please refer to the command file.
