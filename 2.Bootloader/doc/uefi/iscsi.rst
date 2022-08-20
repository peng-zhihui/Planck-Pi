.. SPDX-License-Identifier: GPL-2.0+
.. Copyright (c) 2018 Heinrich Schuchardt

iSCSI booting with U-Boot and iPXE
==================================

Motivation
----------

U-Boot has only a reduced set of supported network protocols. The focus for
network booting has been on UDP based protocols. A TCP stack and HTTP support
are expected to be integrated in 2018 together with a wget command.

For booting a diskless computer this leaves us with BOOTP or DHCP to get the
address of a boot script. TFTP or NFS can be used to load the boot script, the
operating system kernel and the initial file system (initrd).

These protocols are insecure. The client cannot validate the authenticity
of the contacted servers. And the server cannot verify the identity of the
client.

Furthermore the services providing the operating system loader or kernel are
not the ones that the operating system typically will use. Especially in a SAN
environment this makes updating the operating system a hassle. After installing
a new kernel version the boot files have to be copied to the TFTP server
directory.

The HTTPS protocol provides certificate based validation of servers. Sensitive
data like passwords can be securely transmitted.

The iSCSI protocol is used for connecting storage attached networks. It
provides mutual authentication using the CHAP protocol. It typically runs on
a TCP transport.

Thus a better solution than DHCP/TFTP/NFS boot would be to load a boot script
via HTTPS and to download any other files needed for booting via iSCSI from the
same target where the operating system is installed.

An alternative to implementing these protocols in U-Boot is to use an existing
software that can run on top of U-Boot. iPXE[1] is the "swiss army knife" of
network booting. It supports both HTTPS and iSCSI. It has a scripting engine for
fine grained control of the boot process and can provide a command shell.

iPXE can be built as an EFI application (named snp.efi) which can be loaded and
run by U-Boot.

Boot sequence
-------------

U-Boot loads the EFI application iPXE snp.efi using the bootefi command. This
application has network access via the simple network protocol offered by
U-Boot.

iPXE executes its internal script. This script may optionally chain load a
secondary boot script via HTTPS or open a shell.

For the further boot process iPXE connects to the iSCSI server. This includes
the mutual authentication using the CHAP protocol. After the authentication iPXE
has access to the iSCSI targets.

For a selected iSCSI target iPXE sets up a handle with the block IO protocol. It
uses the ConnectController boot service of U-Boot to request U-Boot to connect a
file system driver. U-Boot reads from the iSCSI drive via the block IO protocol
offered by iPXE. It creates the partition handles and installs the simple file
protocol. Now iPXE can call the simple file protocol to load GRUB[2]. U-Boot
uses the block IO protocol offered by iPXE to fulfill the request.

Once GRUB is started it uses the same block IO protocol to load Linux. Via
the EFI stub Linux is called as an EFI application::

                  +--------+         +--------+
                  |        | Runs    |        |
                  | U-Boot |========>| iPXE   |
                  | EFI    |         | snp.efi|
    +--------+    |        | DHCP    |        |
    |        |<===|********|<========|        |
    | DHCP   |    |        | Get IP  |        |
    | Server |    |        | Address |        |
    |        |===>|********|========>|        |
    +--------+    |        | Response|        |
                  |        |         |        |
                  |        |         |        |
    +--------+    |        | HTTPS   |        |
    |        |<===|********|<========|        |
    | HTTPS  |    |        | Load    |        |
    | Server |    |        | Script  |        |
    |        |===>|********|========>|        |
    +--------+    |        |         |        |
                  |        |         |        |
                  |        |         |        |
    +--------+    |        | iSCSI   |        |
    |        |<===|********|<========|        |
    | iSCSI  |    |        | Auth    |        |
    | Server |===>|********|========>|        |
    |        |    |        |         |        |
    |        |    |        | Loads   |        |
    |        |<===|********|<========|        |       +--------+
    |        |    |        | GRUB    |        | Runs  |        |
    |        |===>|********|========>|        |======>| GRUB   |
    |        |    |        |         |        |       |        |
    |        |    |        |         |        |       |        |
    |        |    |        |         |        | Loads |        |
    |        |<===|********|<========|********|<======|        |      +--------+
    |        |    |        |         |        | Linux |        | Runs |        |
    |        |===>|********|========>|********|======>|        |=====>| Linux  |
    |        |    |        |         |        |       |        |      |        |
    +--------+    +--------+         +--------+       +--------+      |        |
                                                                      |        |
                                                                      |        |
                                                                      | ~ ~ ~ ~|

Security
--------

The iSCSI protocol is not encrypted. The traffic could be secured using IPsec
but neither U-Boot nor iPXE does support this. So we should at least separate
the iSCSI traffic from all other network traffic. This can be achieved using a
virtual local area network (VLAN).

Configuration
-------------

iPXE
~~~~

For running iPXE on arm64 the bin-arm64-efi/snp.efi build target is needed::

    git clone http://git.ipxe.org/ipxe.git
    cd ipxe/src
    make bin-arm64-efi/snp.efi -j6 EMBED=myscript.ipxe

The available commands for the boot script are documented at:

http://ipxe.org/cmd

Credentials are managed as environment variables. These are described here:

http://ipxe.org/cfg

iPXE by default will put the CPU to rest when waiting for input. U-Boot does
not wake it up due to missing interrupt support. To avoid this behavior create
file src/config/local/nap.h:

.. code-block:: c

    /* nap.h */
    #undef NAP_EFIX86
    #undef NAP_EFIARM
    #define NAP_NULL

The supported commands in iPXE are controlled by an include, too. Putting the
following into src/config/local/general.h is sufficient for most use cases:

.. code-block:: c

    /* general.h */
    #define NSLOOKUP_CMD            /* Name resolution command */
    #define PING_CMD                /* Ping command */
    #define NTP_CMD                 /* NTP commands */
    #define VLAN_CMD                /* VLAN commands */
    #define IMAGE_EFI               /* EFI image support */
    #define DOWNLOAD_PROTO_HTTPS    /* Secure Hypertext Transfer Protocol */
    #define DOWNLOAD_PROTO_FTP      /* File Transfer Protocol */
    #define DOWNLOAD_PROTO_NFS      /* Network File System Protocol */
    #define DOWNLOAD_PROTO_FILE     /* Local file system access */

Open-iSCSI
~~~~~~~~~~

When the root file system is on an iSCSI drive you should disable pings and set
the replacement timer to a high value in the configuration file [3]::

    node.conn[0].timeo.noop_out_interval = 0
    node.conn[0].timeo.noop_out_timeout = 0
    node.session.timeo.replacement_timeout = 86400

Links
-----

* [1] https://ipxe.org - iPXE open source boot firmware
* [2] https://www.gnu.org/software/grub/ -
  GNU GRUB (Grand Unified Bootloader)
* [3] https://github.com/open-iscsi/open-iscsi/blob/master/README -
  Open-iSCSI README
