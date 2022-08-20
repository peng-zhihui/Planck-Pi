.. SPDX-License-Identifier: GPL-2.0+

FastBoot Version 0.4
====================

The fastboot protocol is a mechanism for communicating with bootloaders
over USB.  It is designed to be very straightforward to implement, to
allow it to be used across a wide range of devices and from hosts running
Linux, Windows, or OSX.

Basic Requirements
------------------

* Two bulk endpoints (in, out) are required
* Max packet size must be 64 bytes for full-speed and 512 bytes for
  high-speed USB
* The protocol is entirely host-driven and synchronous (unlike the
  multi-channel, bi-directional, asynchronous ADB protocol)


Transport and Framing
---------------------

1. Host sends a command, which is an ascii string in a single
   packet no greater than 64 bytes.

2. Client response with a single packet no greater than 64 bytes.
   The first four bytes of the response are "OKAY", "FAIL", "DATA",
   or "INFO".  Additional bytes may contain an (ascii) informative
   message.

   a. INFO -> the remaining 60 bytes are an informative message
      (providing progress or diagnostic messages).  They should
      be displayed and then step #2 repeats

   b. FAIL -> the requested command failed.  The remaining 60 bytes
      of the response (if present) provide a textual failure message
      to present to the user.  Stop.

   c. OKAY -> the requested command completed successfully.  Go to #5

   d. DATA -> the requested command is ready for the data phase.
      A DATA response packet will be 12 bytes long, in the form of
      DATA00000000 where the 8 digit hexidecimal number represents
      the total data size to transfer.

3. Data phase.  Depending on the command, the host or client will
   send the indicated amount of data.  Short packets are always
   acceptable and zero-length packets are ignored.  This phase continues
   until the client has sent or received the number of bytes indicated
   in the "DATA" response above.

4. Client responds with a single packet no greater than 64 bytes.
   The first four bytes of the response are "OKAY", "FAIL", or "INFO".
   Similar to #2:

   a. INFO -> display the remaining 60 bytes and return to #4

   b. FAIL -> display the remaining 60 bytes (if present) as a failure
      reason and consider the command failed.  Stop.

   c. OKAY -> success.  Go to #5

5. Success.  Stop.


Example Session
---------------

.. code-block:: none

    Host:    "getvar:version"        request version variable

    Client:  "OKAY0.4"               return version "0.4"

    Host:    "getvar:nonexistant"    request some undefined variable

    Client:  "OKAY"                  return value ""

    Host:    "download:00001234"     request to send 0x1234 bytes of data

    Client:  "DATA00001234"          ready to accept data

    Host:    < 0x1234 bytes >        send data

    Client:  "OKAY"                  success

    Host:    "flash:bootloader"      request to flash the data to the bootloader

    Client:  "INFOerasing flash"     indicate status / progress
             "INFOwriting flash"
             "OKAY"                  indicate success

    Host:    "powerdown"             send a command

    Client:  "FAILunknown command"   indicate failure


Command Reference
-----------------

* Command parameters are indicated by printf-style escape sequences.

* Commands are ascii strings and sent without the quotes (which are
  for illustration only here) and without a trailing 0 byte.

* Commands that begin with a lowercase letter are reserved for this
  specification.  OEM-specific commands should not begin with a
  lowercase letter, to prevent incompatibilities with future specs.

.. code-block:: none

 "getvar:%s"           Read a config/version variable from the bootloader.
                       The variable contents will be returned after the
                       OKAY response.

 "download:%08x"       Write data to memory which will be later used
                       by "boot", "ramdisk", "flash", etc.  The client
                       will reply with "DATA%08x" if it has enough
                       space in RAM or "FAIL" if not.  The size of
                       the download is remembered.

  "verify:%08x"        Send a digital signature to verify the downloaded
                       data.  Required if the bootloader is "secure"
                       otherwise "flash" and "boot" will be ignored.

  "flash:%s"           Write the previously downloaded image to the
                       named partition (if possible).

  "erase:%s"           Erase the indicated partition (clear to 0xFFs)

  "boot"               The previously downloaded data is a boot.img
                       and should be booted according to the normal
                       procedure for a boot.img

  "continue"           Continue booting as normal (if possible)

  "reboot"             Reboot the device.

  "reboot-bootloader"  Reboot back into the bootloader.
                       Useful for upgrade processes that require upgrading
                       the bootloader and then upgrading other partitions
                       using the new bootloader.

  "powerdown"          Power off the device.

Client Variables
----------------

The ``getvar:%s`` command is used to read client variables which
represent various information about the device and the software
on it.

The various currently defined names are::

  version             Version of FastBoot protocol supported.
                      It should be "0.3" for this document.

  version-bootloader  Version string for the Bootloader.

  version-baseband    Version string of the Baseband Software

  product             Name of the product

  serialno            Product serial number

  secure              If the value is "yes", this is a secure
                      bootloader requiring a signature before
                      it will install or boot images.

Names starting with a lowercase character are reserved by this
specification.  OEM-specific names should not start with lowercase
characters.
