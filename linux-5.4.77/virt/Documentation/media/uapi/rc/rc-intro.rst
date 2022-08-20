.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _Remote_controllers_Intro:

************
Introduction
************

Currently, most analog and digital devices have a Infrared input for
remote controllers. Each manufacturer has their own type of control. It
is not rare for the same manufacturer to ship different types of
controls, depending on the device.

A Remote Controller interface is mapped as a normal evdev/input
interface, just like a keyboard or a mouse. So, it uses all ioctls
already defined for any other input devices.

However, remove controllers are more flexible than a normal input
device, as the IR receiver (and/or transmitter) can be used in
conjunction with a wide variety of different IR remotes.

In order to allow flexibility, the Remote Controller subsystem allows
controlling the RC-specific attributes via
:ref:`the sysfs class nodes <remote_controllers_sysfs_nodes>`.
