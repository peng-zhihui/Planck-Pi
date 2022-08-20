.. SPDX-License-Identifier: GPL-2.0+ */
.. Copyright (c) 2014 The Chromium OS Authors.
.. sectionauthor:: Simon Glass <sjg@chromium.org>

Sandbox
=======

Native Execution of U-Boot
--------------------------

The 'sandbox' architecture is designed to allow U-Boot to run under Linux on
almost any hardware. To achieve this it builds U-Boot (so far as possible)
as a normal C application with a main() and normal C libraries.

All of U-Boot's architecture-specific code therefore cannot be built as part
of the sandbox U-Boot. The purpose of running U-Boot under Linux is to test
all the generic code, not specific to any one architecture. The idea is to
create unit tests which we can run to test this upper level code.

CONFIG_SANDBOX is defined when building a native board.

The board name is 'sandbox' but the vendor name is unset, so there is a
single board in board/sandbox.

CONFIG_SANDBOX_BIG_ENDIAN should be defined when running on big-endian
machines.

There are two versions of the sandbox: One using 32-bit-wide integers, and one
using 64-bit-wide integers. The 32-bit version can be build and run on either
32 or 64-bit hosts by either selecting or deselecting CONFIG_SANDBOX_32BIT; by
default, the sandbox it built for a 32-bit host. The sandbox using 64-bit-wide
integers can only be built on 64-bit hosts.

Note that standalone/API support is not available at present.


Prerequisites
-------------

Here are some packages that are worth installing if you are doing sandbox or
tools development in U-Boot:

   python3-pytest lzma lzma-alone lz4 python3 python3-virtualenv
   libssl1.0-dev


Basic Operation
---------------

To run sandbox U-Boot use something like::

   make sandbox_defconfig all
   ./u-boot

Note: If you get errors about 'sdl-config: Command not found' you may need to
install libsdl2.0-dev or similar to get SDL support. Alternatively you can
build sandbox without SDL (i.e. no display/keyboard support) by removing
the CONFIG_SANDBOX_SDL line in include/configs/sandbox.h or using::

   make sandbox_defconfig all NO_SDL=1
   ./u-boot

U-Boot will start on your computer, showing a sandbox emulation of the serial
console::

   U-Boot 2014.04 (Mar 20 2014 - 19:06:00)

   DRAM:  128 MiB
   Using default environment

   In:    serial
   Out:   lcd
   Err:   lcd
   =>

You can issue commands as your would normally. If the command you want is
not supported you can add it to include/configs/sandbox.h.

To exit, type 'reset' or press Ctrl-C.


Console / LCD support
---------------------

Assuming that CONFIG_SANDBOX_SDL is defined when building, you can run the
sandbox with LCD and keyboard emulation, using something like::

   ./u-boot -d u-boot.dtb -l

This will start U-Boot with a window showing the contents of the LCD. If
that window has the focus then you will be able to type commands as you
would on the console. You can adjust the display settings in the device
tree file - see arch/sandbox/dts/sandbox.dts.


Command-line Options
--------------------

Various options are available, mostly for test purposes. Use -h to see
available options. Some of these are described below.

The terminal is normally in what is called 'raw-with-sigs' mode. This means
that you can use arrow keys for command editing and history, but if you
press Ctrl-C, U-Boot will exit instead of handling this as a keypress.

Other options are 'raw' (so Ctrl-C is handled within U-Boot) and 'cooked'
(where the terminal is in cooked mode and cursor keys will not work, Ctrl-C
will exit).

As mentioned above, -l causes the LCD emulation window to be shown.

A device tree binary file can be provided with -d. If you edit the source
(it is stored at arch/sandbox/dts/sandbox.dts) you must rebuild U-Boot to
recreate the binary file.

To use the default device tree, use -D. To use the test device tree, use -T.

To execute commands directly, use the -c option. You can specify a single
command, or multiple commands separated by a semicolon, as is normal in
U-Boot. Be careful with quoting as the shell will normally process and
swallow quotes. When -c is used, U-Boot exits after the command is complete,
but you can force it to go to interactive mode instead with -i.


Memory Emulation
----------------

Memory emulation is supported, with the size set by CONFIG_SYS_SDRAM_SIZE.
The -m option can be used to read memory from a file on start-up and write
it when shutting down. This allows preserving of memory contents across
test runs. You can tell U-Boot to remove the memory file after it is read
(on start-up) with the --rm_memory option.

To access U-Boot's emulated memory within the code, use map_sysmem(). This
function is used throughout U-Boot to ensure that emulated memory is used
rather than the U-Boot application memory. This provides memory starting
at 0 and extending to the size of the emulation.


Storing State
-------------

With sandbox you can write drivers which emulate the operation of drivers on
real devices. Some of these drivers may want to record state which is
preserved across U-Boot runs. This is particularly useful for testing. For
example, the contents of a SPI flash chip should not disappear just because
U-Boot exits.

State is stored in a device tree file in a simple format which is driver-
specific. You then use the -s option to specify the state file. Use -r to
make U-Boot read the state on start-up (otherwise it starts empty) and -w
to write it on exit (otherwise the stored state is left unchanged and any
changes U-Boot made will be lost). You can also use -n to tell U-Boot to
ignore any problems with missing state. This is useful when first running
since the state file will be empty.

The device tree file has one node for each driver - the driver can store
whatever properties it likes in there. See 'Writing Sandbox Drivers' below
for more details on how to get drivers to read and write their state.


Running and Booting
-------------------

Since there is no machine architecture, sandbox U-Boot cannot actually boot
a kernel, but it does support the bootm command. Filesystems, memory
commands, hashing, FIT images, verified boot and many other features are
supported.

When 'bootm' runs a kernel, sandbox will exit, as U-Boot does on a real
machine. Of course in this case, no kernel is run.

It is also possible to tell U-Boot that it has jumped from a temporary
previous U-Boot binary, with the -j option. That binary is automatically
removed by the U-Boot that gets the -j option. This allows you to write
tests which emulate the action of chain-loading U-Boot, typically used in
a situation where a second 'updatable' U-Boot is stored on your board. It
is very risky to overwrite or upgrade the only U-Boot on a board, since a
power or other failure will brick the board and require return to the
manufacturer in the case of a consumer device.


Supported Drivers
-----------------

U-Boot sandbox supports these emulations:

- Block devices
- Chrome OS EC
- GPIO
- Host filesystem (access files on the host from within U-Boot)
- I2C
- Keyboard (Chrome OS)
- LCD
- Network
- Serial (for console only)
- Sound (incomplete - see sandbox_sdl_sound_init() for details)
- SPI
- SPI flash
- TPM (Trusted Platform Module)

A wide range of commands are implemented. Filesystems which use a block
device are supported.

Also sandbox supports driver model (CONFIG_DM) and associated commands.


Sandbox Variants
----------------

There are unfortunately quite a few variants at present:

sandbox:
  should be used for most tests
sandbox64:
  special build that forces a 64-bit host
sandbox_flattree:
  builds with dev_read\_...() functions defined as inline.
  We need this build so that we can test those inline functions, and we
  cannot build with both the inline functions and the non-inline functions
  since they are named the same.
sandbox_spl:
  builds sandbox with SPL support, so you can run spl/u-boot-spl
  and it will start up and then load ./u-boot. It is also possible to
  run ./u-boot directly.

Of these sandbox_spl can probably be removed since it is a superset of sandbox.

Most of the config options should be identical between these variants.


Linux RAW Networking Bridge
---------------------------

The sandbox_eth_raw driver bridges traffic between the bottom of the network
stack and the RAW sockets API in Linux. This allows much of the U-Boot network
functionality to be tested in sandbox against real network traffic.

For Ethernet network adapters, the bridge utilizes the RAW AF_PACKET API.  This
is needed to get access to the lowest level of the network stack in Linux. This
means that all of the Ethernet frame is included. This allows the U-Boot network
stack to be fully used. In other words, nothing about the Linux network stack is
involved in forming the packets that end up on the wire. To receive the
responses to packets sent from U-Boot the network interface has to be set to
promiscuous mode so that the network card won't filter out packets not destined
for its configured (on Linux) MAC address.

The RAW sockets Ethernet API requires elevated privileges in Linux. You can
either run as root, or you can add the capability needed like so::

   sudo /sbin/setcap "CAP_NET_RAW+ep" /path/to/u-boot

The default device tree for sandbox includes an entry for eth0 on the sandbox
host machine whose alias is "eth1". The following are a few examples of network
operations being tested on the eth0 interface.

.. code-block:: none

   sudo /path/to/u-boot -D

   DHCP
   ....

   setenv autoload no
   setenv ethrotate no
   setenv ethact eth1
   dhcp

   PING
   ....

   setenv autoload no
   setenv ethrotate no
   setenv ethact eth1
   dhcp
   ping $gatewayip

   TFTP
   ....

   setenv autoload no
   setenv ethrotate no
   setenv ethact eth1
   dhcp
   setenv serverip WWW.XXX.YYY.ZZZ
   tftpboot u-boot.bin

The bridge also supports (to a lesser extent) the localhost interface, 'lo'.

The 'lo' interface cannot use the RAW AF_PACKET API because the lo interface
doesn't support Ethernet-level traffic. It is a higher-level interface that is
expected only to be used at the AF_INET level of the API. As such, the most raw
we can get on that interface is the RAW AF_INET API on UDP. This allows us to
set the IP_HDRINCL option to include everything except the Ethernet header in
the packets we send and receive.

Because only UDP is supported, ICMP traffic will not work, so expect that ping
commands will time out.

The default device tree for sandbox includes an entry for lo on the sandbox
host machine whose alias is "eth5". The following is an example of a network
operation being tested on the lo interface.

.. code-block:: none

   TFTP
   ....

   setenv ethrotate no
   setenv ethact eth5
   tftpboot u-boot.bin


SPI Emulation
-------------

Sandbox supports SPI and SPI flash emulation.

The device can be enabled via a device tree, for example::

    spi@0 {
            #address-cells = <1>;
            #size-cells = <0>;
            reg = <0 1>;
            compatible = "sandbox,spi";
            cs-gpios = <0>, <&gpio_a 0>;
            spi.bin@0 {
                    reg = <0>;
                    compatible = "spansion,m25p16", "jedec,spi-nor";
                    spi-max-frequency = <40000000>;
                    sandbox,filename = "spi.bin";
            };
    };

The file must be created in advance::

   $ dd if=/dev/zero of=spi.bin bs=1M count=2
   $ u-boot -T

Here, you can use "-T" or "-D" option to specify test.dtb or u-boot.dtb,
respectively, or "-d <file>" for your own dtb.

With this setup you can issue SPI flash commands as normal::

   =>sf probe
   SF: Detected M25P16 with page size 64 KiB, total 2 MiB
   =>sf read 0 0 10000
   SF: 65536 bytes @ 0x0 Read: OK

Since this is a full SPI emulation (rather than just flash), you can
also use low-level SPI commands::

   =>sspi 0:0 32 9f
   FF202015

This is issuing a READ_ID command and getting back 20 (ST Micro) part
0x2015 (the M25P16).


Block Device Emulation
----------------------

U-Boot can use raw disk images for block device emulation. To e.g. list
the contents of the root directory on the second partion of the image
"disk.raw", you can use the following commands::

   =>host bind 0 ./disk.raw
   =>ls host 0:2

A disk image can be created using the following commands::

   $> truncate -s 1200M ./disk.raw
   $> echo -e "label: gpt\n,64M,U\n,,L" | /usr/sbin/sgdisk  ./disk.raw
   $> lodev=`sudo losetup -P -f --show ./disk.raw`
   $> sudo mkfs.vfat -n EFI -v ${lodev}p1
   $> sudo mkfs.ext4 -L ROOT -v ${lodev}p2

or utilize the device described in test/py/make_test_disk.py::

   #!/usr/bin/python
   import make_test_disk
   make_test_disk.makeDisk()

Writing Sandbox Drivers
-----------------------

Generally you should put your driver in a file containing the word 'sandbox'
and put it in the same directory as other drivers of its type. You can then
implement the same hooks as the other drivers.

To access U-Boot's emulated memory, use map_sysmem() as mentioned above.

If your driver needs to store configuration or state (such as SPI flash
contents or emulated chip registers), you can use the device tree as
described above. Define handlers for this with the SANDBOX_STATE_IO macro.
See arch/sandbox/include/asm/state.h for documentation. In short you provide
a node name, compatible string and functions to read and write the state.
Since writing the state can expand the device tree, you may need to use
state_setprop() which does this automatically and avoids running out of
space. See existing code for examples.


Debugging the init sequence
---------------------------

If you get a failure in the initcall sequence, like this::

   initcall sequence 0000560775957c80 failed at call 0000000000048134 (err=-96)

Then you use can use grep to see which init call failed, e.g.::

   $ grep 0000000000048134 u-boot.map
   stdio_add_devices

Of course another option is to run it with a debugger such as gdb::

   $ gdb u-boot
   ...
   (gdb) br initcall.h:41
   Breakpoint 1 at 0x4db9d: initcall.h:41. (2 locations)

Note that two locations are reported, since this function is used in both
board_init_f() and board_init_r().

.. code-block:: none

   (gdb) r
   Starting program: /tmp/b/sandbox/u-boot
   [Thread debugging using libthread_db enabled]
   Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

   U-Boot 2018.09-00264-ge0c2ba9814-dirty (Sep 22 2018 - 12:21:46 -0600)

   DRAM:  128 MiB
   MMC:

   Breakpoint 1, initcall_run_list (init_sequence=0x5555559619e0 <init_sequence_f>)
       at /scratch/sglass/cosarm/src/third_party/u-boot/files/include/initcall.h:41
   41                              printf("initcall sequence %p failed at call %p (err=%d)\n",
   (gdb) print *init_fnc_ptr
   $1 = (const init_fnc_t) 0x55555559c114 <stdio_add_devices>
   (gdb)


This approach can be used on normal boards as well as sandbox.


SDL_CONFIG
----------

If sdl-config is on a different path from the default, set the SDL_CONFIG
environment variable to the correct pathname before building U-Boot.


Using valgrind / memcheck
-------------------------

It is possible to run U-Boot under valgrind to check memory allocations::

   valgrind u-boot

If you are running sandbox SPL or TPL, then valgrind will not by default
notice when U-Boot jumps from TPL to SPL, or from SPL to U-Boot proper. To
fix this, use::

   valgrind --trace-children=yes u-boot


Testing
-------

U-Boot sandbox can be used to run various tests, mostly in the test/
directory. These include:

command_ut:
  Unit tests for command parsing and handling
compression:
  Unit tests for U-Boot's compression algorithms, useful for
  security checking. It supports gzip, bzip2, lzma and lzo.
driver model:
  Run this pytest::

   ./test/py/test.py --bd sandbox --build -k ut_dm -v

image:
  Unit tests for images:
  test/image/test-imagetools.sh - multi-file images
  test/image/test-fit.py        - FIT images
tracing:
  test/trace/test-trace.sh tests the tracing system (see README.trace)
verified boot:
  See test/vboot/vboot_test.sh for this

If you change or enhance any of the above subsystems, you shold write or
expand a test and include it with your patch series submission. Test
coverage in U-Boot is limited, as we need to work to improve it.

Note that many of these tests are implemented as commands which you can
run natively on your board if desired (and enabled).

To run all tests use "make check".

To run a single test in an existing sandbox build, you can use -T to use the
test device tree, and -c to select the test:

  /tmp/b/sandbox/u-boot -T -c "ut dm pci_busdev"

This runs dm_test_pci_busdev() which is in test/dm/pci.c


Memory Map
----------

Sandbox has its own emulated memory starting at 0. Here are some of the things
that are mapped into that memory:

=======   ========================   ===============================
Addr      Config                     Usage
=======   ========================   ===============================
      0   CONFIG_SYS_FDT_LOAD_ADDR   Device tree
   e000   CONFIG_BLOBLIST_ADDR       Blob list
  10000   CONFIG_MALLOC_F_ADDR       Early memory allocation
  f0000   CONFIG_PRE_CON_BUF_ADDR    Pre-console buffer
 100000   CONFIG_TRACE_EARLY_ADDR    Early trace buffer (if enabled)
=======   ========================   ===============================
