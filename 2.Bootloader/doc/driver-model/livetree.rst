.. SPDX-License-Identifier: GPL-2.0+
.. sectionauthor:: Simon Glass <sjg@chromium.org>

Live Device Tree
================


Introduction
------------

Traditionally U-Boot has used a 'flat' device tree. This means that it
reads directly from the device tree binary structure. It is called a flat
device tree because nodes are listed one after the other, with the
hierarchy detected by tags in the format.

This document describes U-Boot's support for a 'live' device tree, meaning
that the tree is loaded into a hierarchical data structure within U-Boot.


Motivation
----------

The flat device tree has several advantages:

- it is the format produced by the device tree compiler, so no translation
  is needed

- it is fairly compact (e.g. there is no need for pointers)

- it is accessed by the libfdt library, which is well tested and stable


However the flat device tree does have some limitations. Adding new
properties can involve copying large amounts of data around to make room.
The overall tree has a fixed maximum size so sometimes the tree must be
rebuilt in a new location to create more space. Even if not adding new
properties or nodes, scanning the tree can be slow. For example, finding
the parent of a node is a slow process. Reading from nodes involves a
small amount parsing which takes a little time.

Driver model scans the entire device tree sequentially on start-up which
avoids the worst of the flat tree's limitations. But if the tree is to be
modified at run-time, a live tree is much faster. Even if no modification
is necessary, parsing the tree once and using a live tree from then on
seems to save a little time.


Implementation
--------------

In U-Boot a live device tree ('livetree') is currently supported only
after relocation. Therefore we need a mechanism to specify a device
tree node regardless of whether it is in the flat tree or livetree.

The 'ofnode' type provides this. An ofnode can point to either a flat tree
node (when the live tree node is not yet set up) or a livetree node. The
caller of an ofnode function does not need to worry about these details.

The main users of the information in a device tree are drivers. These have
a 'struct udevice \*' which is attached to a device tree node. Therefore it
makes sense to be able to read device tree  properties using the
'struct udevice \*', rather than having to obtain the ofnode first.

The 'dev_read\_...()' interface provides this. It allows properties to be
easily read from the device tree using only a device pointer. Under the
hood it uses ofnode so it works with both flat and live device trees.


Enabling livetree
-----------------

CONFIG_OF_LIVE enables livetree. When this option is enabled, the flat
tree will be used in SPL and before relocation in U-Boot proper. Just
before relocation a livetree is built, and this is used for U-Boot proper
after relocation.

Most checks for livetree use CONFIG_IS_ENABLED(OF_LIVE). This means that
for SPL, the CONFIG_SPL_OF_LIVE option is checked. At present this does
not exist, since SPL does not support livetree.


Porting drivers
---------------

Many existing drivers use the fdtdec interface to read device tree
properties. This only works with a flat device tree. The drivers should be
converted to use the dev_read_() interface.

For example, the old code may be like this:

.. code-block:: c

    struct udevice *bus;
    const void *blob = gd->fdt_blob;
    int node = dev_of_offset(bus);

    i2c_bus->regs = (struct i2c_ctlr *)devfdt_get_addr(dev);
    plat->frequency = fdtdec_get_int(blob, node, "spi-max-frequency", 500000);

The new code is:

.. code-block:: c

    struct udevice *bus;

    i2c_bus->regs = (struct i2c_ctlr *)dev_read_addr(dev);
    plat->frequency = dev_read_u32_default(bus, "spi-max-frequency", 500000);

The dev_read\_...() interface is more convenient and works with both the
flat and live device trees. See include/dm/read.h for a list of functions.

Where properties must be read from sub-nodes or other nodes, you must fall
back to using ofnode. For example, for old code like this:

.. code-block:: c

    const void *blob = gd->fdt_blob;
    int subnode;

    fdt_for_each_subnode(subnode, blob, dev_of_offset(dev)) {
        freq = fdtdec_get_int(blob, node, "spi-max-frequency", 500000);
        ...
    }

you should use:

.. code-block:: c

    ofnode subnode;

    ofnode_for_each_subnode(subnode, dev_ofnode(dev)) {
        freq = ofnode_read_u32(node, "spi-max-frequency", 500000);
        ...
    }


Useful ofnode functions
-----------------------

The internal data structures of the livetree are defined in include/dm/of.h :

   :struct device_node: holds information about a device tree node
   :struct property: holds information about a property within a node

Nodes have pointers to their first property, their parent, their first child
and their sibling. This allows nodes to be linked together in a hierarchical
tree.

Properties have pointers to the next property. This allows all properties of
a node to be linked together in a chain.

It should not be necessary to use these data structures in normal code. In
particular, you should refrain from using functions which access the livetree
directly, such as of_read_u32(). Use ofnode functions instead, to allow your
code to work with a flat tree also.

Some conversion functions are used internally. Generally these are not needed
for driver code. Note that they will not work if called in the wrong context.
For example it is invalid to call ofnode_to_no() when a flat tree is being
used. Similarly it is not possible to call ofnode_to_offset() on a livetree
node.

ofnode_to_np():
   converts ofnode to struct device_node *
ofnode_to_offset():
   converts ofnode to offset

no_to_ofnode():
   converts node pointer to ofnode
offset_to_ofnode():
   converts offset to ofnode


Other useful functions:

of_live_active():
   returns true if livetree is in use, false if flat tree
ofnode_valid():
   return true if a given node is valid
ofnode_is_np():
   returns true if a given node is a livetree node
ofnode_equal():
   compares two ofnodes
ofnode_null():
   returns a null ofnode (for which ofnode_valid() returns false)


Phandles
--------

There is full phandle support for live tree. All functions make use of
struct ofnode_phandle_args, which has an ofnode within it. This supports both
livetree and flat tree transparently. See for example
ofnode_parse_phandle_with_args().


Reading addresses
-----------------

You should use dev_read_addr() and friends to read addresses from device-tree
nodes.


fdtdec
------

The existing fdtdec interface will eventually be retired. Please try to avoid
using it in new code.


Modifying the livetree
----------------------

This is not currently supported. Once implemented it should provide a much
more efficient implementation for modification of the device tree than using
the flat tree.


Internal implementation
-----------------------

The dev_read\_...() functions have two implementations. When
CONFIG_DM_DEV_READ_INLINE is enabled, these functions simply call the ofnode
functions directly. This is useful when livetree is not enabled. The ofnode
functions call ofnode_is_np(node) which will always return false if livetree
is disabled, just falling back to flat tree code.

This optimisation means that without livetree enabled, the dev_read\_...() and
ofnode interfaces do not noticeably add to code size.

The CONFIG_DM_DEV_READ_INLINE option defaults to enabled when livetree is
disabled.

Most livetree code comes directly from Linux and is modified as little as
possible. This is deliberate since this code is fairly stable and does what
we want. Some features (such as get/put) are not supported. Internal macros
take care of removing these features silently.

Within the of_access.c file there are pointers to the alias node, the chosen
node and the stdout-path alias.


Errors
------

With a flat device tree, libfdt errors are returned (e.g. -FDT_ERR_NOTFOUND).
For livetree normal 'errno' errors are returned (e.g. -ENOTFOUND). At present
the ofnode and dev_read\_...() functions return either one or other type of
error. This is clearly not desirable. Once tests are added for all the
functions this can be tidied up.


Adding new access functions
---------------------------

Adding a new function for device-tree access involves the following steps:

   - Add two dev_read() functions:
      - inline version in the read.h header file, which calls an ofnode function
      - standard version in the read.c file (or perhaps another file), which
        also calls an ofnode function

        The implementations of these functions can be the same. The purpose
        of the inline version is purely to reduce code size impact.

   - Add an ofnode function. This should call ofnode_is_np() to work out
     whether a livetree or flat tree is used. For the livetree it should
     call an of\_...() function. For the flat tree it should call an
     fdt\_...() function. The livetree version will be optimised out at
     compile time if livetree is not enabled.

   - Add an of\_...() function for the livetree implementation. If a similar
     function is available in Linux, the implementation should be taken
     from there and modified as little as possible (generally not at all).


Future work
-----------

Live tree support was introduced in U-Boot 2017.07. There is still quite a bit
of work to do to flesh this out:

- tests for all access functions
- support for livetree modification
- addition of more access functions as needed
- support for livetree in SPL and before relocation (if desired)
