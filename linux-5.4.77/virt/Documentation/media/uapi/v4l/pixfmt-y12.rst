.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _V4L2-PIX-FMT-Y12:

*************************
V4L2_PIX_FMT_Y12 ('Y12 ')
*************************


Grey-scale image


Description
===========

This is a grey-scale image with a depth of 12 bits per pixel. Pixels are
stored in 16-bit words with unused high bits padded with 0. The least
significant byte is stored at lower memory addresses (little-endian).

**Byte Order.**
Each cell is one byte.




.. flat-table::
    :header-rows:  0
    :stub-columns: 0

    * - start + 0:
      - Y'\ :sub:`00low`
      - Y'\ :sub:`00high`
      - Y'\ :sub:`01low`
      - Y'\ :sub:`01high`
      - Y'\ :sub:`02low`
      - Y'\ :sub:`02high`
      - Y'\ :sub:`03low`
      - Y'\ :sub:`03high`
    * - start + 8:
      - Y'\ :sub:`10low`
      - Y'\ :sub:`10high`
      - Y'\ :sub:`11low`
      - Y'\ :sub:`11high`
      - Y'\ :sub:`12low`
      - Y'\ :sub:`12high`
      - Y'\ :sub:`13low`
      - Y'\ :sub:`13high`
    * - start + 16:
      - Y'\ :sub:`20low`
      - Y'\ :sub:`20high`
      - Y'\ :sub:`21low`
      - Y'\ :sub:`21high`
      - Y'\ :sub:`22low`
      - Y'\ :sub:`22high`
      - Y'\ :sub:`23low`
      - Y'\ :sub:`23high`
    * - start + 24:
      - Y'\ :sub:`30low`
      - Y'\ :sub:`30high`
      - Y'\ :sub:`31low`
      - Y'\ :sub:`31high`
      - Y'\ :sub:`32low`
      - Y'\ :sub:`32high`
      - Y'\ :sub:`33low`
      - Y'\ :sub:`33high`
