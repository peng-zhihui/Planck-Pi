.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _CA_GET_SLOT_INFO:

================
CA_GET_SLOT_INFO
================

Name
----

CA_GET_SLOT_INFO


Synopsis
--------

.. c:function:: int ioctl(fd, CA_GET_SLOT_INFO, struct ca_slot_info *info)
    :name: CA_GET_SLOT_INFO


Arguments
---------

``fd``
  File descriptor returned by a previous call to :c:func:`open() <cec-open>`.

``info``
  Pointer to struct :c:type:`ca_slot_info`.

Description
-----------

Returns information about a CA slot identified by
:c:type:`ca_slot_info`.slot_num.


Return Value
------------

On success 0 is returned, and :c:type:`ca_slot_info` is filled.

On error -1 is returned, and the ``errno`` variable is set
appropriately.

.. tabularcolumns:: |p{2.5cm}|p{15.0cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 1 16

    -  -  ``ENODEV``
       -  the slot is not available.

The generic error codes are described at the
:ref:`Generic Error Codes <gen-errors>` chapter.
