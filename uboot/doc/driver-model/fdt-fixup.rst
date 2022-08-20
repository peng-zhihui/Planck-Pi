.. SPDX-License-Identifier: GPL-2.0+
.. 2017-01-06, Mario Six <mario.six@gdsys.cc>

Pre-relocation device tree manipulation
=======================================

Purpose
-------

In certain markets, it is beneficial for manufacturers of embedded devices to
offer certain ranges of products, where the functionality of the devices within
one series either don't differ greatly from another, or can be thought of as
"extensions" of each other, where one device only differs from another in the
addition of a small number of features (e.g. an additional output connector).

To realize this in hardware, one method is to have a motherboard, and several
possible daughter boards that can be attached to this mother board. Different
daughter boards then either offer the slightly different functionality, or the
addition of the daughter board to the device realizes the "extension" of
functionality to the device described previously.

For the software, we obviously want to reuse components for all these
variations of the device. This means that the software somehow needs to cope
with the situation that certain ICs may or may not be present on any given
system, depending on which daughter boards are connected to the motherboard.

In the Linux kernel, one possible solution to this problem is to employ the
device tree overlay mechanism: There exists one "base" device tree, which
features only the components guaranteed to exist in all varieties of the
device. At the start of the kernel, the presence and type of the daughter
boards is then detected, and the corresponding device tree overlays are applied
to support the components on the daughter boards.

Note that the components present on every variety of the board must, of course,
provide a way to find out if and which daughter boards are installed for this
mechanism to work.

In the U-Boot boot loader, support for device tree overlays has recently been
integrated, and is used on some boards to alter the device tree that is later
passed to Linux. But since U-Boot's driver model, which is device tree-based as
well, is being used in more and more drivers, the same problem of altering the
device tree starts cropping up in U-Boot itself as well.

An additional problem with the device tree in U-Boot is that it is read-only,
and the current mechanisms don't allow easy manipulation of the device tree
after the driver model has been initialized. While migrating to a live device
tree (at least after the relocation) would greatly simplify the solution of
this problem, it is a non-negligible task to implement it, an a interim
solution is needed to address the problem at least in the medium-term.

Hence, we propose a solution to this problem by offering a board-specific
call-back function, which is passed a writeable pointer to the device tree.
This function is called before the device tree is relocated, and specifically
before the main U-Boot's driver model is instantiated, hence the main U-Boot
"sees" all modifications to the device tree made in this function. Furthermore,
we have the pre-relocation driver model at our disposal at this stage, which
means that we can query the hardware for the existence and variety of the
components easily.

Implementation
--------------

To take advantage of the pre-relocation device tree manipulation mechanism,
boards have to implement the function board_fix_fdt, which has the following
signature:

.. code-block:: c

   int board_fix_fdt (void *rw_fdt_blob)

The passed-in void pointer is a writeable pointer to the device tree, which can
be used to manipulate the device tree using e.g. functions from
include/fdt_support.h. The return value should either be 0 in case of
successful execution of the device tree manipulation or something else for a
failure. Note that returning a non-null value from the function will
unrecoverably halt the boot process, as with any function from init_sequence_f
(in common/board_f.c).

Furthermore, the Kconfig option OF_BOARD_FIXUP has to be set for the function
to be called::

   Device Tree Control
   -> [*] Board-specific manipulation of Device Tree

+----------------------------------------------------------+
| WARNING: The actual manipulation of the device tree has  |
| to be the _last_ set of operations in board_fix_fdt!     |
| Since the pre-relocation driver model does not adapt to  |
| changes made to the device tree either, its references   |
| into the device tree will be invalid after manipulating  |
| it, and unpredictable behavior might occur when          |
| functions that rely on them are executed!                |
+----------------------------------------------------------+

Hence, the recommended layout of the board_fixup_fdt call-back function is the
following:

.. code-block:: c

	int board_fix_fdt(void *rw_fdt_blob)
	{
		/*
		 * Collect information about device's hardware and store
		 * them in e.g. local variables
		 */

		/* Do device tree manipulation using the values previously collected */

		/* Return 0 on successful manipulation and non-zero otherwise */
	}

If this convention is kept, both an "additive" approach, meaning that nodes for
detected components are added to the device tree, as well as a "subtractive"
approach, meaning that nodes for absent components are removed from the tree,
as well as a combination of both approaches should work.

Example
-------

The controlcenterdc board (board/gdsys/a38x/controlcenterdc.c) features a
board_fix_fdt function, in which six GPIO expanders (which might be present or
not, since they are on daughter boards) on a I2C bus are queried for, and
subsequently deactivated in the device tree if they are not present.

Note that the dm_i2c_simple_probe function does not use the device tree, hence
it is safe to call it after the tree has already been manipulated.

Work to be done
---------------

* The application of device tree overlay should be possible in board_fixup_fdt,
  but has not been tested at this stage.
