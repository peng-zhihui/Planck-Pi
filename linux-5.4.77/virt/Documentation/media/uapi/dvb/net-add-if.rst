.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _NET_ADD_IF:

****************
ioctl NET_ADD_IF
****************

Name
====

NET_ADD_IF - Creates a new network interface for a given Packet ID.


Synopsis
========

.. c:function:: int ioctl( int fd, NET_ADD_IF, struct dvb_net_if *net_if )
    :name: NET_ADD_IF


Arguments
=========

``fd``
    File descriptor returned by :ref:`open() <frontend_f_open>`.

``net_if``
    pointer to struct :c:type:`dvb_net_if`


Description
===========

The NET_ADD_IF ioctl system call selects the Packet ID (PID) that
contains a TCP/IP traffic, the type of encapsulation to be used (MPE or
ULE) and the interface number for the new interface to be created. When
the system call successfully returns, a new virtual network interface is
created.

The struct :c:type:`dvb_net_if`::ifnum field will be
filled with the number of the created interface.

Return Value
============

On success 0 is returned, and :c:type:`ca_slot_info` is filled.

On error -1 is returned, and the ``errno`` variable is set
appropriately.

The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.
