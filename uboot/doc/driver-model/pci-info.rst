.. SPDX-License-Identifier: GPL-2.0+

PCI with Driver Model
=====================

How busses are scanned
----------------------

Any config read will end up at pci_read_config(). This uses
uclass_get_device_by_seq() to get the PCI bus for a particular bus number.
Bus number 0 will need to be requested first, and the alias in the device
tree file will point to the correct device::

	aliases {
		pci0 = &pcic;
	};

	pcic: pci@0 {
		compatible = "sandbox,pci";
		...
	};


If there is no alias the devices will be numbered sequentially in the device
tree.

The call to uclass_get_device() will cause the PCI bus to be probed.
This does a scan of the bus to locate available devices. These devices are
bound to their appropriate driver if available. If there is no driver, then
they are bound to a generic PCI driver which does nothing.

After probing a bus, the available devices will appear in the device tree
under that bus.

Note that this is all done on a lazy basis, as needed, so until something is
touched on PCI (eg: a call to pci_find_devices()) it will not be probed.

PCI devices can appear in the flattened device tree. If they do, their node
often contains extra information which cannot be derived from the PCI IDs or
PCI class of the device. Each PCI device node must have a <reg> property, as
defined by the IEEE Std 1275-1994 PCI bus binding document v2.1. Compatible
string list is optional and generally not needed, since PCI is discoverable
bus, albeit there are justified exceptions. If the compatible string is
present, matching on it takes precedence over PCI IDs and PCI classes.

Note we must describe PCI devices with the same bus hierarchy as the
hardware, otherwise driver model cannot detect the correct parent/children
relationship during PCI bus enumeration thus PCI devices won't be bound to
their drivers accordingly. A working example like below::

	pci {
		#address-cells = <3>;
		#size-cells = <2>;
		compatible = "pci-x86";
		u-boot,dm-pre-reloc;
		ranges = <0x02000000 0x0 0x40000000 0x40000000 0 0x80000000
			  0x42000000 0x0 0xc0000000 0xc0000000 0 0x20000000
			  0x01000000 0x0 0x2000 0x2000 0 0xe000>;

		pcie@17,0 {
			#address-cells = <3>;
			#size-cells = <2>;
			compatible = "pci-bridge";
			u-boot,dm-pre-reloc;
			reg = <0x0000b800 0x0 0x0 0x0 0x0>;

			topcliff@0,0 {
				#address-cells = <3>;
				#size-cells = <2>;
				compatible = "pci-bridge";
				u-boot,dm-pre-reloc;
				reg = <0x00010000 0x0 0x0 0x0 0x0>;

				pciuart0: uart@a,1 {
					compatible = "pci8086,8811.00",
							"pci8086,8811",
							"pciclass,070002",
							"pciclass,0700",
							"x86-uart";
					u-boot,dm-pre-reloc;
					reg = <0x00025100 0x0 0x0 0x0 0x0
					       0x01025110 0x0 0x0 0x0 0x0>;
					......
				};

				......
			};
		};

		......
	};

In this example, the root PCI bus node is the "/pci" which matches "pci-x86"
driver. It has a subnode "pcie@17,0" with driver "pci-bridge". "pcie@17,0"
also has subnode "topcliff@0,0" which is a "pci-bridge" too. Under that bridge,
a PCI UART device "uart@a,1" is described. This exactly reflects the hardware
bus hierarchy: on the root PCI bus, there is a PCIe root port which connects
to a downstream device Topcliff chipset. Inside Topcliff chipset, it has a
PCIe-to-PCI bridge and all the chipset integrated devices like the PCI UART
device are on the PCI bus. Like other devices in the device tree, if we want
to bind PCI devices before relocation, "u-boot,dm-pre-reloc" must be declared
in each of these nodes.

If PCI devices are not listed in the device tree, U_BOOT_PCI_DEVICE can be used
to specify the driver to use for the device. The device tree takes precedence
over U_BOOT_PCI_DEVICE. Please note with U_BOOT_PCI_DEVICE, only drivers with
DM_FLAG_PRE_RELOC will be bound before relocation. If neither device tree nor
U_BOOT_PCI_DEVICE is provided, the built-in driver (either pci_bridge_drv or
pci_generic_drv) will be used.


Sandbox
-------

With sandbox we need a device emulator for each device on the bus since there
is no real PCI bus. This works by looking in the device tree node for an
emulator driver. For example::

	pci@1f,0 {
		compatible = "pci-generic";
		reg = <0xf800 0 0 0 0>;
		sandbox,emul = <&emul_1f>;
	};
	pci-emul {
		compatible = "sandbox,pci-emul-parent";
		emul_1f: emul@1f,0 {
			compatible = "sandbox,swap-case";
		};
	};

This means that there is a 'sandbox,swap-case' driver at that bus position.
Note that the first cell in the 'reg' value is the bus/device/function. See
PCI_BDF() for the encoding (it is also specified in the IEEE Std 1275-1994
PCI bus binding document, v2.1)

The pci-emul node should go outside the pci bus node, since otherwise it will
be scanned as a PCI device, causing confusion.

When this bus is scanned we will end up with something like this::

   `- * pci@0 @ 05c660c8, 0
    `-   pci@1f,0 @ 05c661c8, 63488
   `-   emul@1f,0 @ 05c662c8

When accesses go to the pci@1f,0 device they are forwarded to its emulator.

The sandbox PCI drivers also support dynamic driver binding, allowing device
driver to declare the driver binding information via U_BOOT_PCI_DEVICE(),
eliminating the need to provide any device tree node under the host controller
node. It is required a "sandbox,dev-info" property must be provided in the
host controller node for this functionality to work.

.. code-block:: none

	pci1: pci@1 {
		compatible = "sandbox,pci";
		...
		sandbox,dev-info = <0x08 0x00 0x1234 0x5678
				    0x0c 0x00 0x1234 0x5678>;
	};

The "sandbox,dev-info" property specifies all dynamic PCI devices on this bus.
Each dynamic PCI device is encoded as 4 cells a group. The first and second
cells are PCI device number and function number respectively. The third and
fourth cells are PCI vendor ID and device ID respectively.

When this bus is scanned we will end up with something like this::

 pci        [ + ]   pci_sandbo  |-- pci1
 pci_emul   [   ]   sandbox_sw  |   |-- sandbox_swap_case_emul
 pci_emul   [   ]   sandbox_sw  |   `-- sandbox_swap_case_emul
