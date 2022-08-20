.. SPDX-License-Identifier: GPL-2.0+
.. (C) Copyright 2015
.. Texas Instruments Incorporated - http://www.ti.com/

Remote Processor Framework
==========================

Introduction
------------

This is an introduction to driver-model for Remote Processors found
on various System on Chip(SoCs). The term remote processor is used to
indicate that this is not the processor on which U-Boot is operating
on, instead is yet another processing entity that may be controlled by
the processor on which we are functional.

The simplified model depends on a single UCLASS - UCLASS_REMOTEPROC

UCLASS_REMOTEPROC:
  - drivers/remoteproc/rproc-uclass.c
  - include/remoteproc.h

Commands:
  - common/cmd_remoteproc.c

Configuration:
  - CONFIG_REMOTEPROC is selected by drivers as needed
  - CONFIG_CMD_REMOTEPROC for the commands if required.

How does it work - The driver
-----------------------------

Overall, the driver statemachine transitions are typically as follows::

           (entry)
           +-------+
       +---+ init  |
       |   |       | <---------------------+
       |   +-------+                       |
       |                                   |
       |                                   |
       |   +--------+                      |
   Load|   |  reset |                      |
       |   |        | <----------+         |
       |   +--------+            |         |
       |        |Load            |         |
       |        |                |         |
       |   +----v----+   reset   |         |
       +-> |         |    (opt)  |         |
           |  Loaded +-----------+         |
           |         |                     |
           +----+----+                     |
                | Start                    |
            +---v-----+        (opt)       |
         +->| Running |        Stop        |
   Ping  +- |         +--------------------+
   (opt)    +---------+

(is_running does not change state)
opt: Optional state transition implemented by driver.

NOTE: It depends on the remote processor as to the exact behavior
of the statemachine, remoteproc core does not intent to implement
statemachine logic. Certain processors may allow start/stop without
reloading the image in the middle, certain other processors may only
allow us to start the processor(image from a EEPROM/OTP) etc.

It is hence the responsibility of the driver to handle the requisite
state transitions of the device as necessary.

Basic design assumptions:

Remote processor can operate on a certain firmware that maybe loaded
and released from reset.

The driver follows a standard UCLASS DM.

in the bare minimum form:

.. code-block:: c

	static const struct dm_rproc_ops sandbox_testproc_ops = {
		.load = sandbox_testproc_load,
		.start = sandbox_testproc_start,
	};

	static const struct udevice_id sandbox_ids[] = {
		{.compatible = "sandbox,test-processor"},
		{}
	};

	U_BOOT_DRIVER(sandbox_testproc) = {
		.name = "sandbox_test_proc",
		.of_match = sandbox_ids,
		.id = UCLASS_REMOTEPROC,
		.ops = &sandbox_testproc_ops,
		.probe = sandbox_testproc_probe,
	};

This allows for the device to be probed as part of the "init" command
or invocation of 'rproc_init()' function as the system dependencies define.

The driver is expected to maintain it's own statemachine which is
appropriate for the device it maintains. It must, at the very least
provide a load and start function. We assume here that the device
needs to be loaded and started, else, there is no real purpose of
using the remoteproc framework.

Describing the device using platform data
-----------------------------------------

*IMPORTANT* NOTE: THIS SUPPORT IS NOT MEANT FOR USE WITH NEWER PLATFORM
SUPPORT. THIS IS ONLY FOR LEGACY DEVICES. THIS MODE OF INITIALIZATION
*WILL* BE EVENTUALLY REMOVED ONCE ALL NECESSARY PLATFORMS HAVE MOVED
TO DM/FDT.

Considering that many platforms are yet to move to device-tree model,
a simplified definition of a device is as follows:

.. code-block:: c

	struct dm_rproc_uclass_pdata proc_3_test = {
		.name = "proc_3_legacy",
		.mem_type = RPROC_INTERNAL_MEMORY_MAPPED,
		.driver_plat_data = &mydriver_data;
	};

	U_BOOT_DEVICE(proc_3_demo) = {
		.name = "sandbox_test_proc",
		.platdata = &proc_3_test,
	};

There can be additional data that may be desired depending on the
remoteproc driver specific needs (for example: SoC integration
details such as clock handle or something similar). See appropriate
documentation for specific remoteproc driver for further details.
These are passed via driver_plat_data.

Describing the device using device tree
---------------------------------------

.. code-block: none

	/ {
		...
		aliases {
			...
			remoteproc0 = &rproc_1;
			remoteproc1 = &rproc_2;

		};
		...

		rproc_1: rproc@1 {
			compatible = "sandbox,test-processor";
			remoteproc-name = "remoteproc-test-dev1";
		};

		rproc_2: rproc@2 {
			compatible = "sandbox,test-processor";
			internal-memory-mapped;
			remoteproc-name = "remoteproc-test-dev2";
		};
		...
	};

aliases usage is optional, but it is usually recommended to ensure the
users have a consistent usage model for a platform.
the compatible string used here is specific to the remoteproc driver involved.
